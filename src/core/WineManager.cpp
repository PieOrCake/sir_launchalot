#include "core/WineManager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

WineManager::WineManager(QObject *parent)
    : QObject(parent)
{
}

QList<WineManager::WineRunner> WineManager::discoverRunners() const
{
    QList<WineRunner> runners;
    runners.append(discoverSystemWine());
    runners.append(discoverLutrisRunners());
    runners.append(discoverProtonRunners());
    return runners;
}

WineManager::WineRunner WineManager::findRunner(const QString &name) const
{
    auto runners = discoverRunners();
    for (const auto &r : runners) {
        if (r.name == name) {
            return r;
        }
    }
    return {};
}

void WineManager::setSelectedRunner(const WineRunner &runner)
{
    m_selectedRunner = runner;
}

WineManager::WineRunner WineManager::selectedRunner() const
{
    return m_selectedRunner;
}

QString WineManager::wineVersion(const QString &wineBinary)
{
    QProcess proc;
    proc.start(wineBinary, {"--version"});
    proc.waitForFinished(5000);
    return QString(proc.readAllStandardOutput()).trimmed();
}

QString WineManager::prefixWineVersion(const QString &prefixPath)
{
    // The Wine version is stored in system.reg under
    // [Software\\Microsoft\\Windows NT\\CurrentVersion]
    // But a simpler approach: run wineboot --version against the prefix.
    // Even simpler: parse the update timestamp isn't useful.
    // Best approach: check the wineserver version that last touched the prefix
    // by reading the first few hundred bytes of system.reg for version hints,
    // or by checking the DLL versions in system32.
    //
    // Most reliable: read the PE version of ntdll.dll in the prefix
    QFile ntdll(prefixPath + "/drive_c/windows/system32/ntdll.dll");
    if (!ntdll.exists()) return {};

    // Use file to get the Wine version from ntdll.dll
    QProcess proc;
    proc.start("strings", {"-n", "10", ntdll.fileName()});
    proc.waitForFinished(3000);
    QString output = proc.readAllStandardOutput();
    // Look for "wine-X.Y" pattern
    QRegularExpression re("(wine-[0-9]+\\.[0-9]+[^ \\n]*)");
    auto match = re.match(output);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return {};
}

WineManager::WineRunner WineManager::bestRunnerForPrefix(
    const QList<WineRunner> &runners, const QString &prefixPath) const
{
    QString prefixVer = prefixWineVersion(prefixPath);
    if (prefixVer.isEmpty()) return {};

    // Extract major.minor from prefix version (e.g. "wine-11.2" -> "11.2")
    QRegularExpression verRe("(\\d+\\.\\d+)");
    auto prefixMatch = verRe.match(prefixVer);
    if (!prefixMatch.hasMatch()) return {};
    QString prefixMajorMinor = prefixMatch.captured(1);

    // Find a runner whose version contains the same major.minor
    for (const auto &r : runners) {
        auto runnerMatch = verRe.match(r.version);
        if (runnerMatch.hasMatch() && runnerMatch.captured(1) == prefixMajorMinor) {
            return r;
        }
    }
    return {};
}

QList<WineManager::WineRunner> WineManager::discoverSystemWine() const
{
    QList<WineRunner> runners;

    // Check standard locations
    QStringList candidates = {"wine", "wine64"};
    for (const auto &name : candidates) {
        QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) {
            WineRunner r;
            r.name = name;
            r.path = path;
            r.version = wineVersion(path);
            r.source = "system";
            runners.append(r);
        }
    }

    return runners;
}

QList<WineManager::WineRunner> WineManager::discoverLutrisRunners() const
{
    QList<WineRunner> runners;

    // Lutris stores wine runners in ~/.local/share/lutris/runners/wine/
    QString lutrisWineDir = QDir::homePath() + "/.local/share/lutris/runners/wine";
    QDir dir(lutrisWineDir);
    if (!dir.exists()) {
        return runners;
    }

    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        // Prefer bin/wine (the full wrapper) over bin/wine64 (lower-level)
        // Lutris itself uses bin/wine which handles full initialization
        QString winePath = lutrisWineDir + "/" + entry + "/bin/wine";
        if (!QFile::exists(winePath)) {
            winePath = lutrisWineDir + "/" + entry + "/bin/wine64";
        }
        if (QFile::exists(winePath)) {
            WineRunner r;
            r.name = entry;
            r.path = winePath;
            r.version = wineVersion(winePath);
            r.source = "lutris";
            runners.append(r);
        }
    }

    return runners;
}

QList<WineManager::WineRunner> WineManager::discoverProtonRunners() const
{
    QList<WineRunner> runners;

    // Check common Proton locations
    QStringList protonDirs = {
        QDir::homePath() + "/.steam/steam/compatibilitytools.d",
        QDir::homePath() + "/.local/share/Steam/compatibilitytools.d",
        QDir::homePath() + "/.steam/root/compatibilitytools.d",
    };

    for (const auto &protonDir : protonDirs) {
        QDir dir(protonDir);
        if (!dir.exists()) continue;

        QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &entry : entries) {
            // Proton dirs typically have dist/bin/wine or files/bin/wine
            QStringList subpaths = {
                protonDir + "/" + entry + "/dist/bin/wine64",
                protonDir + "/" + entry + "/dist/bin/wine",
                protonDir + "/" + entry + "/files/bin/wine64",
                protonDir + "/" + entry + "/files/bin/wine",
            };
            for (const auto &winePath : subpaths) {
                if (QFile::exists(winePath)) {
                    WineRunner r;
                    r.name = entry;
                    r.path = winePath;
                    r.version = wineVersion(winePath);
                    r.source = "proton";
                    runners.append(r);
                    break;
                }
            }
        }
    }

    return runners;
}
