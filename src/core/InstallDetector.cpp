#include "core/InstallDetector.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

InstallDetector::InstallDetector(QObject *parent)
    : QObject(parent)
{
}

bool InstallDetector::isLutrisInstalled() const
{
    return !QStandardPaths::findExecutable("lutris").isEmpty();
}

QString InstallDetector::lutrisConfigDir() const
{
    // Modern Lutris stores configs here
    QString dataDir = QDir::homePath() + "/.local/share/lutris/games";
    if (QDir(dataDir).exists()) return dataDir;
    // Legacy location
    return QDir::homePath() + "/.config/lutris/games";
}

bool InstallDetector::isUmuInstalled()
{
    return !QStandardPaths::findExecutable("umu-run").isEmpty();
}

QString InstallDetector::deriveProtonPath(const QString &wineBinary)
{
    if (wineBinary.isEmpty()) return QString();

    // Proton-style: .../GE-Proton9-4/files/bin/wine -> .../GE-Proton9-4
    if (wineBinary.contains("/files/bin/")) {
        return wineBinary.section("/files/bin/", 0, 0);
    }

    // Lutris Wine-GE: .../wine-ge-8-27/bin/wine -> try matching Proton in compatibilitytools
    // (can't derive directly, return empty and let caller default to GE-Proton)
    return QString();
}

QList<InstallDetector::DetectedInstall> InstallDetector::discoverGW2Installs() const
{
    QList<DetectedInstall> results;
    QSet<QString> seenPrefixes;

    // Helper: add results while deduplicating by canonical prefix path
    auto addResults = [&](const QList<DetectedInstall> &newResults) {
        for (const auto &game : newResults) {
            QString canonical = QFileInfo(game.winePrefix).canonicalFilePath();
            if (canonical.isEmpty() || seenPrefixes.contains(canonical)) continue;
            seenPrefixes.insert(canonical);
            results.append(game);
        }
    };

    addResults(scanLutrisConfigs());
    addResults(scanHeroicConfigs());
    addResults(scanFaugusConfigs());
    addResults(scanSteamConfigs());

    return results;
}

InstallDetector::DetectedInstall InstallDetector::parseGameConfig(const QString &configPath) const
{
    DetectedInstall game;

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


QString InstallDetector::gw2ExeInPrefix(const QString &winePrefix)
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

QList<InstallDetector::DetectedInstall> InstallDetector::scanLutrisConfigs() const
{
    QList<DetectedInstall> results;

    QDir configDir(lutrisConfigDir());
    if (!configDir.exists()) {
        return results;
    }

    QSet<QString> seenPrefixes;
    QStringList configs = configDir.entryList({"*.yml"}, QDir::Files);
    for (const auto &configFile : configs) {
        DetectedInstall game = parseGameConfig(configDir.absoluteFilePath(configFile));
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
        game.source = "lutris";
        game.protonPath = deriveProtonPath(game.wineBinary);
        if (game.name.isEmpty()) {
            game.name = "Guild Wars 2 (Lutris)";
        } else if (!game.name.contains("Lutris", Qt::CaseInsensitive)) {
            game.name += " (Lutris)";
        }
        results.append(game);
    }

    return results;
}

QList<InstallDetector::DetectedInstall> InstallDetector::scanHeroicConfigs() const
{
    QList<DetectedInstall> results;

    // Heroic stores per-game config JSON in GamesConfig/
    // Check both native and Flatpak locations
    QStringList heroicConfigDirs = {
        QDir::homePath() + "/.config/heroic",
        QDir::homePath() + "/.var/app/com.heroicgameslauncher.hgl/config/heroic"
    };

    for (const auto &heroicDir : heroicConfigDirs) {
        QString gamesConfigDir = heroicDir + "/GamesConfig";
        QDir configDir(gamesConfigDir);
        if (!configDir.exists()) continue;

        QSet<QString> seenPrefixes;
        QStringList configs = configDir.entryList({"*.json"}, QDir::Files);
        for (const auto &configFile : configs) {
            QFile file(configDir.absoluteFilePath(configFile));
            if (!file.open(QIODevice::ReadOnly)) continue;

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
            file.close();
            if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;

            QJsonObject obj = doc.object();
            QString winePrefix = obj.value("winePrefix").toString();
            if (winePrefix.isEmpty()) continue;

            // Also check inside pfx/ for Proton-style prefixes
            QString effectivePrefix = winePrefix;
            if (QDir(winePrefix + "/pfx/drive_c").exists()) {
                effectivePrefix = winePrefix + "/pfx";
            }

            // Deduplicate
            QString canonical = QFileInfo(effectivePrefix).canonicalFilePath();
            if (canonical.isEmpty() || seenPrefixes.contains(canonical)) continue;

            QString gw2Exe = gw2ExeInPrefix(effectivePrefix);
            if (gw2Exe.isEmpty()) gw2Exe = gw2ExeInPrefix(winePrefix);
            if (gw2Exe.isEmpty()) continue;

            seenPrefixes.insert(canonical);

            DetectedInstall game;
            game.name = "Guild Wars 2 (Heroic)";
            game.source = "heroic";
            game.winePrefix = effectivePrefix;
            game.exePath = gw2Exe;

            // Extract Proton path from wineVersion config
            QJsonObject wineVersion = obj.value("wineVersion").toObject();
            QString wineBin = wineVersion.value("bin").toString();
            if (!wineBin.isEmpty()) {
                game.wineBinary = wineBin;
                game.protonPath = deriveProtonPath(wineBin);
            }
            // If wineVersion has a type "proton", the directory IS the protonpath
            if (game.protonPath.isEmpty()) {
                QString wvType = wineVersion.value("type").toString();
                QString wvDir = wineVersion.value("lib").toString();
                if (wvDir.isEmpty()) wvDir = wineVersion.value("bin").toString();
                if (wvType.contains("proton", Qt::CaseInsensitive) && !wvDir.isEmpty()) {
                    // bin is like /path/to/GE-Proton/proton -> parent dir
                    game.protonPath = QFileInfo(wvDir).absolutePath();
                }
            }

            results.append(game);
        }
    }

    return results;
}

QList<InstallDetector::DetectedInstall> InstallDetector::scanFaugusConfigs() const
{
    QList<DetectedInstall> results;

    // Faugus stores prefixes in a configurable location.
    // Check default locations: native and Flatpak
    QStringList faugusPrefixDirs = {
        QDir::homePath() + "/.config/faugus-launcher/prefixes",
        QDir::homePath() + "/Faugus",
        QDir::homePath() + "/.var/app/io.github.Faugus.faugus-launcher/config/faugus-launcher/prefixes"
    };

    QSet<QString> seenPrefixes;
    for (const auto &prefixParent : faugusPrefixDirs) {
        QDir parentDir(prefixParent);
        if (!parentDir.exists()) continue;

        // Each subdirectory is a game prefix
        for (const auto &entry : parentDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString prefixPath = parentDir.absoluteFilePath(entry);

            // Some Faugus prefixes use Proton-style pfx/ subdir
            QString effectivePrefix = prefixPath;
            if (QDir(prefixPath + "/pfx/drive_c").exists()) {
                effectivePrefix = prefixPath + "/pfx";
            }

            QString canonical = QFileInfo(effectivePrefix).canonicalFilePath();
            if (canonical.isEmpty() || seenPrefixes.contains(canonical)) continue;

            QString gw2Exe = gw2ExeInPrefix(effectivePrefix);
            if (gw2Exe.isEmpty()) gw2Exe = gw2ExeInPrefix(prefixPath);
            if (gw2Exe.isEmpty()) continue;

            seenPrefixes.insert(canonical);

            DetectedInstall game;
            game.name = "Guild Wars 2 (Faugus)";
            game.source = "faugus";
            game.winePrefix = effectivePrefix;
            game.exePath = gw2Exe;
            // Faugus uses runners from Steam compatibilitytools.d — can't determine which
            // without parsing its internal config. Default to GE-Proton (protonPath empty).
            results.append(game);
        }
    }

    return results;
}

QList<InstallDetector::DetectedInstall> InstallDetector::scanSteamConfigs() const
{
    QList<DetectedInstall> results;

    // Common Steam root directories (native + Flatpak)
    QStringList steamRoots = {
        QDir::homePath() + "/.steam/steam",
        QDir::homePath() + "/.local/share/Steam",
        QDir::homePath() + "/.var/app/com.valvesoftware.Steam/.steam/steam",
        QDir::homePath() + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    };

    // Collect all Steam library folders from libraryfolders.vdf
    QStringList libraryFolders;
    for (const auto &root : steamRoots) {
        QString vdfPath = root + "/steamapps/libraryfolders.vdf";
        if (!QFile::exists(vdfPath)) continue;

        // The root itself is always a library folder
        libraryFolders.append(root);

        // Parse libraryfolders.vdf for additional library paths
        // Format: "path"		"/mnt/games/SteamLibrary"
        QFile vdf(vdfPath);
        if (vdf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QRegularExpression pathRe(R"re("path"\s+"([^"]+)")re");
            QString content = vdf.readAll();
            auto it = pathRe.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                QString libPath = match.captured(1);
                if (!libraryFolders.contains(libPath))
                    libraryFolders.append(libPath);
            }
            vdf.close();
        }
    }

    // GW2 Steam app ID is 1284210
    QSet<QString> seenPrefixes;
    for (const auto &libFolder : libraryFolders) {
        QString compatData = libFolder + "/steamapps/compatdata/1284210";
        QString pfxPath = compatData + "/pfx";

        if (!QDir(pfxPath).exists()) continue;

        QString canonical = QFileInfo(pfxPath).canonicalFilePath();
        if (canonical.isEmpty() || seenPrefixes.contains(canonical)) continue;

        // Steam installs game files in steamapps/common/, not inside the prefix
        QString gw2Exe;
        QStringList commonCandidates = {
            libFolder + "/steamapps/common/Guild Wars 2/Gw2-64.exe",
            libFolder + "/steamapps/common/Guild Wars 2/Gw2.exe",
        };
        for (const auto &candidate : commonCandidates) {
            if (QFile::exists(candidate)) {
                gw2Exe = candidate;
                break;
            }
        }
        // Fallback: check inside the prefix too
        if (gw2Exe.isEmpty()) gw2Exe = gw2ExeInPrefix(pfxPath);
        if (gw2Exe.isEmpty()) continue;

        seenPrefixes.insert(canonical);

        DetectedInstall game;
        game.name = "Guild Wars 2 (Steam)";
        game.source = "steam";
        game.winePrefix = pfxPath;
        game.exePath = gw2Exe;

        // Try to derive Proton path from config_info (first line is the Proton dir)
        QString configInfo = compatData + "/config_info";
        QFile ci(configInfo);
        if (ci.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString protonDir = ci.readLine().trimmed();
            ci.close();
            // config_info first line is like /path/to/Proton - Experimental/
            if (!protonDir.isEmpty() && QDir(protonDir).exists()) {
                game.protonPath = protonDir;
            }
        }

        results.append(game);
    }

    return results;
}
