#include "core/LutrisIntegration.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

LutrisIntegration::LutrisIntegration(QObject *parent)
    : QObject(parent)
{
}

bool LutrisIntegration::isLutrisInstalled() const
{
    return !QStandardPaths::findExecutable("lutris").isEmpty();
}

QString LutrisIntegration::lutrisConfigDir() const
{
    // Modern Lutris stores configs here
    QString dataDir = QDir::homePath() + "/.local/share/lutris/games";
    if (QDir(dataDir).exists()) return dataDir;
    // Legacy location
    return QDir::homePath() + "/.config/lutris/games";
}

QList<LutrisIntegration::LutrisGame> LutrisIntegration::discoverGW2Installs() const
{
    QList<LutrisGame> results;

    // Helper: check if a prefix (resolved via symlinks) is already in results
    auto prefixAlreadyFound = [&results](const QString &prefix) -> bool {
        QString canonical = QFileInfo(prefix).canonicalFilePath();
        if (canonical.isEmpty()) return false;
        for (const auto &existing : results) {
            if (QFileInfo(existing.winePrefix).canonicalFilePath() == canonical) {
                return true;
            }
        }
        return false;
    };

    // Scan Lutris game YAML configs (only Lutris is supported for main)
    results.append(scanLutrisConfigs());

    return results;
}

LutrisIntegration::LutrisGame LutrisIntegration::parseGameConfig(const QString &configPath) const
{
    LutrisGame game;

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return game;
    }

    // Simple YAML-like parsing for Lutris game configs
    // Lutris configs are simple enough that we don't need a full YAML parser
    QTextStream stream(&file);
    QString content = stream.readAll();
    file.close();

    // Extract game name
    QRegularExpression nameRe(R"(^name:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto nameMatch = nameRe.match(content);
    if (nameMatch.hasMatch()) {
        game.name = nameMatch.captured(1).trimmed();
    }

    // Extract slug
    QRegularExpression slugRe(R"(^slug:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto slugMatch = slugRe.match(content);
    if (slugMatch.hasMatch()) {
        game.slug = slugMatch.captured(1).trimmed();
    }

    // Extract runner
    QRegularExpression runnerRe(R"(^runner:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto runnerMatch = runnerRe.match(content);
    if (runnerMatch.hasMatch()) {
        game.runner = runnerMatch.captured(1).trimmed();
    }

    // Extract prefix
    QRegularExpression prefixRe(R"(prefix:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto prefixMatch = prefixRe.match(content);
    if (prefixMatch.hasMatch()) {
        game.winePrefix = prefixMatch.captured(1).trimmed();
    }

    // Extract exe path
    QRegularExpression exeRe(R"(exe:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto exeMatch = exeRe.match(content);
    if (exeMatch.hasMatch()) {
        game.exePath = exeMatch.captured(1).trimmed();
    }

    // Extract wine_path directly (e.g. from script.installer[].task.wine_path)
    QRegularExpression winePathRe(R"(wine_path:\s*(.+)$)", QRegularExpression::MultilineOption);
    auto winePathMatch = winePathRe.match(content);
    if (winePathMatch.hasMatch()) {
        QString winePath = winePathMatch.captured(1).trimmed();
        if (QFile::exists(winePath)) {
            game.wineBinary = winePath;
        }
    }

    // Also try extracting wine version and constructing the binary path
    if (game.wineBinary.isEmpty()) {
        QRegularExpression versionRe(R"(^\s+version:\s*(.+)$)", QRegularExpression::MultilineOption);
        auto versionMatch = versionRe.match(content);
        if (versionMatch.hasMatch()) {
            QString wineVer = versionMatch.captured(1).trimmed();
            QString lutrisWineDir = QDir::homePath() + "/.local/share/lutris/runners/wine/" + wineVer;
            QString wineBin = lutrisWineDir + "/bin/wine64";
            if (!QFile::exists(wineBin)) {
                wineBin = lutrisWineDir + "/bin/wine";
            }
            if (QFile::exists(wineBin)) {
                game.wineBinary = wineBin;
            }
        }
    }

    // Handle relative exe paths (relative to prefix)
    if (!game.exePath.isEmpty() && !game.winePrefix.isEmpty() &&
        !game.exePath.startsWith("/")) {
        game.exePath = game.winePrefix + "/" + game.exePath;
    }

    return game;
}

int LutrisIntegration::detectLutrisGameId(const QString &winePrefix) const
{
    if (!isLutrisInstalled()) return -1;

    QProcess proc;
    proc.start("lutris", {"-lo", "-j"});
    if (!proc.waitForFinished(10000)) return -1;

    QString output = proc.readAllStandardOutput();
    QString canonical = QFileInfo(winePrefix).canonicalFilePath();
    if (canonical.isEmpty()) return -1;

    // Parse JSON array manually — each entry has "id" and "directory"
    // Format: {"id": N, ..., "directory": "/path/to/prefix", ...}
    QRegularExpression entryRe(
        R"RE("id"\s*:\s*(\d+)[^}]*"directory"\s*:\s*"([^"]*)")RE");
    auto it = entryRe.globalMatch(output);
    while (it.hasNext()) {
        auto match = it.next();
        int id = match.captured(1).toInt();
        QString dir = match.captured(2);
        if (!dir.isEmpty() && QFileInfo(dir).canonicalFilePath() == canonical) {
            return id;
        }
    }

    // Try reverse field order (JSON field order is not guaranteed)
    QRegularExpression reverseRe(
        R"RE("directory"\s*:\s*"([^"]*)"[^}]*"id"\s*:\s*(\d+))RE");
    auto it2 = reverseRe.globalMatch(output);
    while (it2.hasNext()) {
        auto match = it2.next();
        QString dir = match.captured(1);
        int id = match.captured(2).toInt();
        if (!dir.isEmpty() && QFileInfo(dir).canonicalFilePath() == canonical) {
            return id;
        }
    }

    return -1;
}

QString LutrisIntegration::gw2ExeInPrefix(const QString &winePrefix)
{
    QStringList candidates = {
        winePrefix + "/drive_c/Program Files/Guild Wars 2/Gw2-64.exe",
        winePrefix + "/drive_c/Program Files/Guild Wars 2/Gw2.exe",
        winePrefix + "/drive_c/Program Files (x86)/Guild Wars 2/Gw2-64.exe",
        winePrefix + "/drive_c/Program Files (x86)/Guild Wars 2/Gw2.exe",
    };

    for (const auto &candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

QList<LutrisIntegration::LutrisGame> LutrisIntegration::scanLutrisConfigs() const
{
    QList<LutrisGame> results;

    QDir configDir(lutrisConfigDir());
    if (!configDir.exists()) {
        return results;
    }

    QSet<QString> seenPrefixes;
    QStringList configs = configDir.entryList({"*.yml"}, QDir::Files);
    for (const auto &configFile : configs) {
        LutrisGame game = parseGameConfig(configDir.absoluteFilePath(configFile));
        if (game.winePrefix.isEmpty()) continue;

        // Deduplicate by canonical prefix path
        QString canonical = QFileInfo(game.winePrefix).canonicalFilePath();
        if (canonical.isEmpty() || seenPrefixes.contains(canonical)) continue;

        // Always verify GW2 actually exists in this prefix
        // (the config's exe might be for GW1, Blish HUD, etc.)
        QString gw2Exe = gw2ExeInPrefix(game.winePrefix);
        if (gw2Exe.isEmpty()) continue;

        game.exePath = gw2Exe;
        seenPrefixes.insert(canonical);

        // Label with Lutris source
        if (game.name.isEmpty()) {
            game.name = "Guild Wars 2 (Lutris)";
        } else if (!game.name.contains("Lutris", Qt::CaseInsensitive)) {
            game.name += " (Lutris)";
        }
        results.append(game);
    }

    return results;
}
