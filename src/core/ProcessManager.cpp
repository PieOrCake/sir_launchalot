#include "core/ProcessManager.h"
#include "core/OverlayManager.h"
#include "core/AccountManager.h"
#include "core/WineManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QTextStream>
#include <QTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QFont>

static QString fileMd5(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return "(cannot open)";
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(&f);
    return hash.result().toHex();
}

static QString fileInfo(const QString &path) {
    QFileInfo fi(path);
    if (!fi.exists()) return "MISSING";
    return QString("%1 bytes, md5=%2").arg(fi.size()).arg(fileMd5(path));
}



ProcessManager::ProcessManager(OverlayManager *overlay,
                               AccountManager *accounts,
                               WineManager *wine,
                               QObject *parent)
    : QObject(parent)
    , m_overlay(overlay)
    , m_accounts(accounts)
    , m_wine(wine)
{
}

ProcessManager::~ProcessManager()
{
    stopAll();
}

void ProcessManager::setProtonPath(const QString &path)
{
    m_protonPath = path;
}

QString ProcessManager::protonPath() const
{
    return m_protonPath;
}

bool ProcessManager::launchAccount(const QString &accountId,
                                    const QString &basePrefix,
                                    const QString &exePath)
{
    if (m_instances.contains(accountId) &&
        m_instances[accountId].state != InstanceState::Stopped) {
        emit instanceError(accountId, "Instance already running");
        return false;
    }

    auto acct = m_accounts->account(accountId);
    QString winePrefix;

    if (acct.isMain) {
        // Main account launches via umu-run
        // Record Gw2.dat modification time for patch detection
        m_gw2DatPath = QFileInfo(exePath).absolutePath() + "/Gw2.dat";
        m_gw2DatMtimeBefore = QFileInfo(m_gw2DatPath).lastModified();

        emit instanceOutput(accountId, "=== Main account launch (umu-run) ===\n");

        // Per-account GAMEID + .desktop for separate taskbar icons
        QString appId = uniqueAppId(accountId);
        QString gameid = "umu-" + appId;
        QString displayName = acct.displayName.isEmpty() ? accountId : acct.displayName;
        QString badge = accountBadgeLabel(accountId);
        ensureGw2Icon(exePath);
        installDesktopEntry(accountId, displayName, appId, badge);

        QStringList mainArgs;
        mainArgs.append(acct.extraArgs);

        QString scriptPath = writeUmuScript(accountId, basePrefix, exePath,
                                             mainArgs, gameid, false);
        if (scriptPath.isEmpty()) {
            emit instanceError(accountId, "Failed to create launch script");
            return false;
        }

        auto *proc = new QProcess(this);
        proc->setProperty("accountId", accountId);
        proc->setProperty("scriptPath", scriptPath);

        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &ProcessManager::onProcessFinished);
        connect(proc, &QProcess::errorOccurred,
                this, &ProcessManager::onProcessError);
        connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
            QString accountId = proc->property("accountId").toString();
            emit instanceOutput(accountId, proc->readAllStandardOutput());
        });
        connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
            QString accountId = proc->property("accountId").toString();
            emit instanceOutput(accountId, proc->readAllStandardError());
        });

        InstanceInfo info;
        info.accountId = accountId;
        info.state = InstanceState::Starting;
        info.process = proc;
        m_instances[accountId] = info;

        proc->start("/bin/bash", {scriptPath});
        if (!proc->waitForStarted(10000)) {
            emit instanceError(accountId, "Failed to start launch script");
            m_instances[accountId].state = InstanceState::Stopped;
            QFile::remove(scriptPath);
            return false;
        }

        m_instances[accountId].pid = proc->processId();
        m_instances[accountId].state = InstanceState::Running;
        emit instanceStarted(accountId);
        return true;
    } else {
        emit instanceOutput(accountId, "=== Alt account launch (prefix clone) ===\n");

        // Alt account: rsync clone of the base prefix (excluding Gw2.dat).
        QString accountDir = m_overlay->dataDir() + "/" + accountId;
        QString clonePrefix = accountDir + "/prefix";
        QDir().mkpath(clonePrefix);

        emit instanceOutput(accountId, QString("Syncing prefix from %1 ...\n").arg(basePrefix));
        QProcess rsync;
        rsync.setProcessChannelMode(QProcess::MergedChannels);
        rsync.start("rsync", {
            "-a", "--delete",
            "--exclude", "Gw2.dat",
            "--exclude", "Gw2.tmp",
            "--exclude", "Gw2-64.tmp",
            basePrefix + "/",
            clonePrefix + "/"
        });
        if (!rsync.waitForFinished(120000)) {
            emit instanceError(accountId, "rsync timed out cloning prefix");
            return false;
        }
        if (rsync.exitCode() != 0) {
            emit instanceError(accountId, "rsync failed: " + rsync.readAll());
            return false;
        }
        emit instanceOutput(accountId, "Prefix synced.\n");

        // Remove addon files from clone if addons are disabled for this account
        if (!acct.enableAddons) {
            QString gameDir = clonePrefix;
            if (exePath.startsWith(basePrefix)) {
                QString relExeDir = QFileInfo(exePath.mid(basePrefix.length() + 1)).path();
                gameDir = clonePrefix + "/" + relExeDir;
            }
            emit instanceOutput(accountId, QString("Addon removal — game dir: %1\n").arg(gameDir));

            // List DLL files in game dir for diagnostics
            QDir gameDirObj(gameDir);
            QStringList dlls = gameDirObj.entryList({"*.dll"}, QDir::Files);
            if (!dlls.isEmpty()) {
                emit instanceOutput(accountId, QString("DLLs in game dir: %1\n").arg(dlls.join(", ")));
            }

            QStringList addonFiles = {
                "d3d11.dll", "d3d9.dll", "dxgi.dll",
                "ReShade.ini", "GW2-UOAOM.ini", "arcdps.ini"
            };
            QStringList addonDirs = {
                "bin64", "addons", "reshade-shaders"
            };
            int removed = 0;
            for (const auto &f : addonFiles) {
                QString path = gameDir + "/" + f;
                if (QFile::exists(path)) {
                    QFile::remove(path);
                    emit instanceOutput(accountId, QString("  Removed: %1\n").arg(f));
                    ++removed;
                }
            }
            for (const auto &d : addonDirs) {
                QString path = gameDir + "/" + d;
                if (QDir(path).exists()) {
                    QDir(path).removeRecursively();
                    emit instanceOutput(accountId, QString("  Removed dir: %1/\n").arg(d));
                    ++removed;
                }
            }
            if (removed > 0) {
                emit instanceOutput(accountId,
                    QString("Addons disabled — removed %1 addon file(s)/dir(s) from clone.\n").arg(removed));
            } else {
                emit instanceOutput(accountId, "Addons disabled — no addon files found to remove.\n");
            }

            // Verify deletion
            QStringList remaining = gameDirObj.entryList({"*.dll"}, QDir::Files);
            if (!remaining.isEmpty()) {
                emit instanceOutput(accountId, QString("Remaining DLLs: %1\n").arg(remaining.join(", ")));
            }
        }

        // Symlink Gw2.dat from base prefix so alts share main's updated game data.
        // This avoids each alt needing its own ~50GB copy and ensures patches
        // applied to main automatically propagate to all alts.
        if (exePath.startsWith(basePrefix)) {
            QString relExeDir = QFileInfo(exePath.mid(basePrefix.length() + 1)).path();
            QString baseGw2Dat = basePrefix + "/" + relExeDir + "/Gw2.dat";
            QString cloneGw2Dat = clonePrefix + "/" + relExeDir + "/Gw2.dat";
            if (QFile::exists(baseGw2Dat)) {
                QFileInfo cloneInfo(cloneGw2Dat);
                // Remove existing file or stale symlink
                if (cloneInfo.exists() || cloneInfo.isSymLink()) {
                    QFile::remove(cloneGw2Dat);
                }
                QFile::link(baseGw2Dat, cloneGw2Dat);
                emit instanceOutput(accountId, QString("Symlinked Gw2.dat -> %1\n").arg(baseGw2Dat));
            }
        }

        // Find the GW2 user data dir in the clone
        QString usersDir = clonePrefix + "/drive_c/users";
        QDir users(usersDir);
        if (users.exists()) {
            for (const auto &user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString gw2Dir = usersDir + "/" + user + "/AppData/Roaming/Guild Wars 2";
                if (!QDir(gw2Dir).exists()) continue;

                // Inject saved Local.dat (generated via Setup Account)
                QString savedDir = accountDir + "/saved";
                QString savedLocalDat = savedDir + "/Local.dat";
                if (QFile::exists(savedLocalDat)) {
                    QString dest = gw2Dir + "/Local.dat";
                    QFile::remove(dest);
                    QFile::copy(savedLocalDat, dest);
                    emit instanceOutput(accountId, QString("Injected saved Local.dat [%1]\n")
                        .arg(fileInfo(dest)));
                    emit instanceOutput(accountId, "Will use -autologin with saved credentials.\n");
                } else {
                    emit instanceOutput(accountId,
                        "WARNING: No saved credentials found. Use 'Setup Account' first.\n");
                }

                // Inject saved GFXSettings (captured from previous session)
                QString savedGfx = savedDir + "/GFXSettings.Gw2-64.exe.xml";
                if (QFile::exists(savedGfx)) {
                    QString dest = gw2Dir + "/GFXSettings.Gw2-64.exe.xml";
                    QFile::remove(dest);
                    QFile::copy(savedGfx, dest);
                    emit instanceOutput(accountId, "Injected saved GFXSettings into clone.\n");
                }

                break;
            }
        }

        // Fix the pfx symlink so Proton stays inside the clone prefix
        QString pfxLink = clonePrefix + "/pfx";
        QFileInfo pfxInfo(pfxLink);
        if (pfxInfo.isSymLink()) {
            QString target = pfxInfo.symLinkTarget();
            if (target != clonePrefix && target != ".") {
                QFile::remove(pfxLink);
                QFile::link(".", pfxLink);
                emit instanceOutput(accountId, "Fixed pfx symlink -> .\n");
            }
        }

        winePrefix = clonePrefix;
        emit instanceOutput(accountId, QString("WINEPREFIX: %1\n").arg(winePrefix));
        emit instanceOutput(accountId, "=== END Alt account launch ===\n");
    }

    // For alt accounts, remap exe path to the clone prefix so Wine/umu-run loads
    // DLLs from the clone directory (where addon files may have been removed)
    QString effectiveExePath = exePath;
    if (!acct.isMain && exePath.startsWith(basePrefix)) {
        QString relPath = exePath.mid(basePrefix.length());  // e.g. /drive_c/.../Gw2-64.exe
        effectiveExePath = winePrefix + relPath;
    }

    // Build args: -shareArchive for alts, -autologin if saved Local.dat exists
    QStringList gameArgs;
    if (!acct.isMain) {
        gameArgs << "-shareArchive";
        QString savedLocalDat = m_overlay->dataDir() + "/" + accountId + "/saved/Local.dat";
        if (QFile::exists(savedLocalDat))
            gameArgs << "-autologin";
    }
    gameArgs.append(acct.extraArgs);

    // Per-account GAMEID + .desktop for separate taskbar icons
    QString appId = uniqueAppId(accountId);
    QString gameid = "umu-" + appId;
    QString displayName = acct.displayName.isEmpty() ? accountId : acct.displayName;
    QString badge = accountBadgeLabel(accountId);
    ensureGw2Icon(exePath);
    installDesktopEntry(accountId, displayName, appId, badge);

    QString scriptPath = writeUmuScript(accountId, winePrefix, effectiveExePath,
                                         gameArgs, gameid, true);
    if (scriptPath.isEmpty()) {
        emit instanceError(accountId, "Failed to create launch script");
        return false;
    }

    // Launch the script
    auto *proc = new QProcess(this);
    proc->setProperty("accountId", accountId);
    proc->setProperty("scriptPath", scriptPath);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ProcessManager::onProcessFinished);
    connect(proc, &QProcess::errorOccurred,
            this, &ProcessManager::onProcessError);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardOutput());
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardError());
    });

    InstanceInfo info;
    info.accountId = accountId;
    info.state = InstanceState::Starting;
    info.process = proc;
    m_instances[accountId] = info;

    proc->start("/bin/bash", {scriptPath});
    if (!proc->waitForStarted(10000)) {
        emit instanceError(accountId, "Failed to start launch script");
        m_instances[accountId].state = InstanceState::Stopped;
        QFile::remove(scriptPath);
        return false;
    }

    m_instances[accountId].pid = proc->processId();
    m_instances[accountId].state = InstanceState::Running;
    emit instanceStarted(accountId);
    return true;
}

bool ProcessManager::stopAccount(const QString &accountId)
{
    if (!m_instances.contains(accountId)) {
        return true;
    }

    auto &info = m_instances[accountId];
    if (info.state == InstanceState::Stopped) {
        return true;
    }

    info.state = InstanceState::Stopping;

    if (info.process) {
        info.process->terminate();
        if (!info.process->waitForFinished(5000)) {
            info.process->kill();
            info.process->waitForFinished(3000);
        }
    }

    info.state = InstanceState::Stopped;
    emit instanceStopped(accountId);
    return true;
}

void ProcessManager::stopAll()
{
    QStringList ids = m_instances.keys();
    for (const auto &id : ids) {
        stopAccount(id);
    }
}

bool ProcessManager::setupAccount(const QString &accountId)
{
    auto acct = m_accounts->account(accountId);
    if (acct.isMain) {
        emit instanceError(accountId, "Cannot run setup on the main account");
        return false;
    }

    if (!runningAccounts().isEmpty()) {
        emit instanceError(accountId, "Stop all running accounts before setup");
        return false;
    }

    if (!m_setupAccountId.isEmpty()) {
        emit instanceError(accountId, "Setup already in progress for another account");
        return false;
    }

    QString basePrefix = m_accounts->basePrefix();
    if (basePrefix.isEmpty()) {
        emit instanceError(accountId, "No base prefix configured");
        return false;
    }

    QString gw2ExePath = m_accounts->gw2ExePath();
    if (gw2ExePath.isEmpty()) {
        emit instanceError(accountId, "No GW2 exe path configured");
        return false;
    }

    // Find Local.dat in the base prefix
    QString usersDir = basePrefix + "/drive_c/users";
    QDir users(usersDir);
    QString localDatPath;
    if (users.exists()) {
        for (const auto &user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString candidate = usersDir + "/" + user +
                "/AppData/Roaming/Guild Wars 2/Local.dat";
            if (QFile::exists(candidate)) {
                localDatPath = candidate;
                break;
            }
        }
    }
    if (localDatPath.isEmpty()) {
        emit instanceError(accountId, "Cannot find Local.dat in base prefix");
        return false;
    }

    // Back up main's Local.dat
    QString backupPath = localDatPath + ".sir-launchalot-backup";
    QFile::remove(backupPath);
    if (!QFile::copy(localDatPath, backupPath)) {
        emit instanceError(accountId, "Failed to back up Local.dat");
        return false;
    }

    emit instanceOutput(accountId, "=== Setup Account ===\n");
    emit instanceOutput(accountId, QString("Backed up Local.dat [%1]\n").arg(fileInfo(localDatPath)));
    emit instanceOutput(accountId,
        "Launching GW2 via umu-run. Please:\n"
        "  1. Log out from the main account (Character Select > Log Out)\n"
        "  2. Enter your alt account credentials\n"
        "  3. Check 'Remember Account Name' and 'Remember Password'\n"
        "  4. Log in and reach character select\n"
        "  5. Close the game\n"
        "Credentials will be captured automatically.\n");

    // Track setup state
    m_setupAccountId = accountId;
    m_localDatPath = localDatPath;
    m_localDatBackupPath = backupPath;

    // Launch via umu-run in base prefix
    QString scriptPath = writeUmuScript(accountId, basePrefix, gw2ExePath,
                                         {}, "umu-1284210", false);
    if (scriptPath.isEmpty()) {
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_setupAccountId.clear();
        emit instanceError(accountId, "Failed to create setup script");
        return false;
    }

    auto *proc = new QProcess(this);
    proc->setProperty("accountId", accountId);
    proc->setProperty("scriptPath", scriptPath);
    proc->setProperty("isSetup", true);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ProcessManager::onProcessFinished);
    connect(proc, &QProcess::errorOccurred,
            this, &ProcessManager::onProcessError);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardOutput());
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardError());
    });

    InstanceInfo info;
    info.accountId = accountId;
    info.state = InstanceState::Running;
    info.process = proc;
    m_instances[accountId] = info;

    proc->start("/bin/bash", {scriptPath});
    if (!proc->waitForStarted(10000)) {
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_setupAccountId.clear();
        m_instances[accountId].state = InstanceState::Stopped;
        QFile::remove(scriptPath);
        emit instanceError(accountId, "Failed to start setup script");
        return false;
    }

    m_instances[accountId].pid = proc->processId();
    emit instanceOutput(accountId, QString("umu-run launched (PID %1) — waiting for game to exit...\n").arg(proc->processId()));
    emit instanceStarted(accountId);
    return true;
}

bool ProcessManager::updateAlt(const QString &accountId, const QString &basePrefix,
                                const QString &exePath)
{
    auto acct = m_accounts->account(accountId);
    if (acct.isMain) {
        emit instanceError(accountId, "Cannot update the main account");
        return false;
    }

    if (!runningAccounts().isEmpty()) {
        emit instanceError(accountId, "Stop all running accounts before updating");
        return false;
    }

    if (!m_updateAccountId.isEmpty()) {
        emit instanceError(accountId, "Update already in progress");
        return false;
    }

    // Find alt's saved Local.dat
    QString savedDir = m_overlay->dataDir() + "/" + accountId + "/saved";
    QString savedLocalDat = savedDir + "/Local.dat";
    if (!QFile::exists(savedLocalDat)) {
        emit instanceError(accountId, "No saved Local.dat — run 'Setup Account' first");
        return false;
    }

    // Find Local.dat in the base prefix
    QString usersDir = basePrefix + "/drive_c/users";
    QDir users(usersDir);
    QString localDatPath;
    if (users.exists()) {
        for (const auto &user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString candidate = usersDir + "/" + user +
                "/AppData/Roaming/Guild Wars 2/Local.dat";
            if (QFile::exists(candidate)) {
                localDatPath = candidate;
                break;
            }
        }
    }
    if (localDatPath.isEmpty()) {
        emit instanceError(accountId, "Cannot find Local.dat in base prefix");
        return false;
    }

    // Back up main's Local.dat
    QString backupPath = localDatPath + ".sir-launchalot-update-backup";
    QFile::remove(backupPath);
    if (!QFile::copy(localDatPath, backupPath)) {
        emit instanceError(accountId, "Failed to back up main's Local.dat");
        return false;
    }

    // Inject alt's saved Local.dat into base prefix
    QFile::remove(localDatPath);
    if (!QFile::copy(savedLocalDat, localDatPath)) {
        QFile::rename(backupPath, localDatPath);
        emit instanceError(accountId, "Failed to inject alt's Local.dat");
        return false;
    }

    emit instanceOutput(accountId, "=== Update Alt Local.dat ===\n");
    emit instanceOutput(accountId, QString("Injected %1's Local.dat into base prefix\n").arg(acct.displayName));
    emit instanceOutput(accountId, "Launching GW2 with -image to update...\n");

    // Track update state
    m_updateAccountId = accountId;
    m_updateLocalDatPath = localDatPath;
    m_updateBackupPath = backupPath;
    m_updateSavedDir = savedDir;

    // Launch via umu-run with -image flag in base prefix
    QString scriptPath = writeUmuScript(accountId, basePrefix, exePath,
                                         {"-image"}, "umu-1284210", false);
    if (scriptPath.isEmpty()) {
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_updateAccountId.clear();
        emit instanceError(accountId, "Failed to create update script");
        return false;
    }

    // Launch the update script
    auto *proc = new QProcess(this);
    proc->setProperty("accountId", accountId);
    proc->setProperty("scriptPath", scriptPath);
    proc->setProperty("isUpdate", true);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ProcessManager::onProcessFinished);
    connect(proc, &QProcess::errorOccurred,
            this, &ProcessManager::onProcessError);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardOutput());
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        QString accountId = proc->property("accountId").toString();
        emit instanceOutput(accountId, proc->readAllStandardError());
    });

    InstanceInfo info;
    info.accountId = accountId;
    info.state = InstanceState::Running;
    info.process = proc;
    m_instances[accountId] = info;

    proc->start("/bin/bash", {scriptPath});
    if (!proc->waitForStarted(10000)) {
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_updateAccountId.clear();
        m_instances[accountId].state = InstanceState::Stopped;
        QFile::remove(scriptPath);
        emit instanceError(accountId, "Failed to start update script");
        return false;
    }

    m_instances[accountId].pid = proc->processId();
    emit instanceStarted(accountId);
    return true;
}

void ProcessManager::updateAllAlts(const QStringList &altIds, const QString &basePrefix,
                                     const QString &exePath)
{
    m_updateQueue = altIds;
    m_updateBasePrefix = basePrefix;
    m_updateExePath = exePath;

    if (m_updateQueue.isEmpty()) {
        emit allUpdatesComplete();
        return;
    }

    QString nextId = m_updateQueue.takeFirst();
    if (!updateAlt(nextId, basePrefix, exePath)) {
        // Failed to start — try next
        emit updateComplete(nextId, false);
        QTimer::singleShot(500, this, [this]() {
            if (!m_updateQueue.isEmpty()) {
                QString nextId = m_updateQueue.takeFirst();
                updateAlt(nextId, m_updateBasePrefix, m_updateExePath);
            } else {
                emit allUpdatesComplete();
            }
        });
    }
}

ProcessManager::InstanceState ProcessManager::instanceState(const QString &accountId) const
{
    if (m_instances.contains(accountId)) {
        return m_instances[accountId].state;
    }
    return InstanceState::Stopped;
}

QStringList ProcessManager::runningAccounts() const
{
    QStringList result;
    for (auto it = m_instances.constBegin(); it != m_instances.constEnd(); ++it) {
        if (it.value().state == InstanceState::Running) {
            result.append(it.key());
        }
    }
    return result;
}

QStringList ProcessManager::buildLaunchArgs(const QString &exePath,
                                              const QString &accountId) const
{
    Q_UNUSED(exePath)
    Q_UNUSED(accountId)
    return {};  // unused — args now built inline before writeUmuScript
}

QProcessEnvironment ProcessManager::buildEnvironment(const QString &accountId,
                                                       const QString &mergedPrefix) const
{
    Q_UNUSED(accountId)
    Q_UNUSED(mergedPrefix)
    return QProcessEnvironment();  // unused — umu-run handles all env setup
}

QString ProcessManager::writeUmuScript(const QString &accountId, const QString &winePrefix,
                                        const QString &exePath, const QStringList &extraArgs,
                                        const QString &gameid, bool useSetsid) const
{
    QString scriptPath = QDir::tempPath() + "/sir-launchalot-" + accountId + ".sh";
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream out(&script);
    out << "#!/bin/bash\n";

    // WINEPREFIX
    QString escapedPrefix = winePrefix;
    escapedPrefix.replace("'", "'\\''");
    out << "export WINEPREFIX='" << escapedPrefix << "'\n";

    // PROTONPATH — use configured path or default to GE-Proton (auto-download)
    QString proton = m_protonPath.isEmpty() ? "GE-Proton" : m_protonPath;
    QString escapedProton = proton;
    escapedProton.replace("'", "'\\''");
    out << "export PROTONPATH='" << escapedProton << "'\n";

    // GAMEID — used by umu for protonfixes and by KDE for window identification
    out << "export GAMEID='" << gameid << "'\n";
    out << "export STORE='none'\n";

    // Working directory
    QString workDir = QFileInfo(exePath).absolutePath();
    QString escapedWorkDir = workDir;
    escapedWorkDir.replace("'", "'\\''");
    out << "\ncd '" << escapedWorkDir << "'\n";

    // umu-run command
    QString umuBin = QStandardPaths::findExecutable("umu-run");
    if (umuBin.isEmpty()) umuBin = "umu-run";
    QString escapedUmu = umuBin;
    escapedUmu.replace("'", "'\\''");

    QString escapedExe = exePath;
    escapedExe.replace("'", "'\\''");

    if (useSetsid) {
        out << "\nexec setsid --wait '" << escapedUmu << "' '" << escapedExe << "'";
    } else {
        out << "\nexec '" << escapedUmu << "' '" << escapedExe << "'";
    }

    for (const auto &arg : extraArgs) {
        QString escaped = arg;
        escaped.replace("'", "'\\''");
        out << " '" << escaped << "'";
    }
    out << "\n";

    script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return scriptPath;
}

QString ProcessManager::uniqueAppId(const QString &accountId)
{
    // Generate a stable per-account fake Steam app ID so each GW2 window
    // gets its own taskbar entry. Base: 1284210 (GW2's real app ID) * 1000.
    uint idHash = qHash(accountId) % 10000;
    return QString::number(1284210000 + idHash);
}

void ProcessManager::installDesktopEntry(const QString &accountId,
                                          const QString &displayName,
                                          const QString &appId,
                                          const QString &badgeLabel)
{
    QString windowClass = "steam_app_" + appId;
    QString appsDir = QDir::homePath() + "/.local/share/applications";
    QDir().mkpath(appsDir);
    QString desktopPath = appsDir + "/sir-launchalot-" + accountId + ".desktop";

    // Generate a per-account icon with badge overlay (M, 1, 2, ...)
    QString iconPath = compositeAccountIcon(badgeLabel);
    if (iconPath.isEmpty()) {
        // Fallback to base gw2 icon or named icon
        iconPath = QDir::homePath() + "/.local/share/icons/hicolor/256x256/apps/gw2.png";
        if (!QFile::exists(iconPath))
            iconPath = "gw2";
    }

    QFile desktop(desktopPath);
    if (desktop.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ds(&desktop);
        ds << "[Desktop Entry]\n";
        ds << "Type=Application\n";
        ds << "Name=GW2 - " << displayName << "\n";
        ds << "Exec=true\n";
        ds << "StartupWMClass=" << windowClass << "\n";
        ds << "NoDisplay=true\n";
        ds << "Icon=" << iconPath << "\n";
    }

    // Notify the DE about the new/updated .desktop file
    QProcess::startDetached("update-desktop-database", {appsDir});
}

void ProcessManager::ensureGw2Icon(const QString &exePath)
{
    // Install a GW2 icon for .desktop entries. Only needs to run once.
    QString iconDir = QDir::homePath() + "/.local/share/icons/hicolor/256x256/apps";
    QString iconPath = iconDir + "/gw2.png";
    if (QFile::exists(iconPath)) return;

    QDir().mkpath(iconDir);

    // Try extracting icon from the GW2 exe using wrestool + icotool (icoutils)
    QString wrestool = QStandardPaths::findExecutable("wrestool");
    QString icotool = QStandardPaths::findExecutable("icotool");

    if (!wrestool.isEmpty() && !icotool.isEmpty()) {
        // wrestool extracts .ico from PE, icotool converts to .png
        // Use the largest icon available (256x256 preferred)
        QProcess extract;
        extract.setProcessChannelMode(QProcess::SeparateChannels);
        extract.start("bash", {"-c",
            QString("'%1' -x -t 14 '%2' 2>/dev/null | '%3' -x -w 256 -o '%4' - 2>/dev/null || "
                    "'%1' -x -t 14 '%2' 2>/dev/null | '%3' -x -o '%4' - 2>/dev/null")
                .arg(wrestool, exePath, icotool, iconPath)});
        extract.waitForFinished(5000);
        if (QFile::exists(iconPath)) return;
    }

    // Try icoextract (Python tool, often available with umu-launcher)
    QString icoextract = QStandardPaths::findExecutable("icoextract");
    if (!icoextract.isEmpty()) {
        QString tmpIco = QDir::tempPath() + "/sir-launchalot-gw2.ico";
        QProcess extract;
        extract.start(icoextract, {exePath, tmpIco});
        extract.waitForFinished(5000);
        if (QFile::exists(tmpIco)) {
            // Convert .ico to .png via icotool or just copy (some DEs handle .ico)
            if (!icotool.isEmpty()) {
                QProcess convert;
                convert.start(icotool, {"-x", "-w", "256", "-o", iconPath, tmpIco});
                convert.waitForFinished(3000);
            }
            if (!QFile::exists(iconPath)) {
                // Fall back: install .ico directly (KDE/GNOME can usually display it)
                QFile::copy(tmpIco, iconDir + "/gw2.ico");
            }
            QFile::remove(tmpIco);
            if (QFile::exists(iconPath)) return;
        }
    }

    // Final fallback: save our application icon as gw2.png
    QIcon appIcon = QGuiApplication::windowIcon();
    if (!appIcon.isNull()) {
        QPixmap pm = appIcon.pixmap(256, 256);
        if (!pm.isNull())
            pm.save(iconPath, "PNG");
    }
}

QString ProcessManager::accountBadgeLabel(const QString &accountId) const
{
    auto acct = m_accounts->account(accountId);
    if (acct.isMain) return "M";

    // Count only alt accounts in display order to determine badge number
    int altIndex = 0;
    for (const auto &id : m_accounts->orderedItemIds()) {
        if (!m_accounts->hasAccount(id)) continue;  // skip external apps
        auto a = m_accounts->account(id);
        if (a.isMain) continue;
        altIndex++;
        if (id == accountId) return QString::number(altIndex);
    }
    return "?";
}

QString ProcessManager::compositeAccountIcon(const QString &badgeLabel) const
{
    QString iconDir = QDir::homePath() + "/.local/share/icons/hicolor/256x256/apps";
    QString baseIcon = iconDir + "/gw2.png";
    if (!QFile::exists(baseIcon)) return {};

    // Cache per badge label so we only composite once per label
    QString outPath = iconDir + "/gw2-badge-" + badgeLabel + ".png";

    QImage img(baseIcon);
    if (img.isNull()) return {};

    // Ensure we have an alpha channel for compositing
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    int size = img.width();
    int badgeRadius = size * 3 / 10;   // badge circle radius
    int cx = size - badgeRadius - 4;  // center X (bottom-right corner)
    int cy = size - badgeRadius - 4;  // center Y

    // Dark background circle with slight border
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 200));
    p.drawEllipse(QPoint(cx, cy), badgeRadius + 2, badgeRadius + 2);
    p.setBrush(QColor(40, 40, 40, 240));
    p.drawEllipse(QPoint(cx, cy), badgeRadius, badgeRadius);

    // White text centered in the badge
    QFont font("Sans", badgeRadius, QFont::Bold);
    font.setPixelSize(badgeRadius * 5 / 3);
    p.setFont(font);
    p.setPen(Qt::white);
    QRect badgeRect(cx - badgeRadius, cy - badgeRadius, badgeRadius * 2, badgeRadius * 2);
    p.drawText(badgeRect, Qt::AlignCenter, badgeLabel);

    p.end();
    img.save(outPath, "PNG");
    return outPath;
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    auto *proc = qobject_cast<QProcess *>(sender());
    if (!proc) return;

    QString accountId = proc->property("accountId").toString();
    bool isUpdate = proc->property("isUpdate").toBool();
    bool isSetup = proc->property("isSetup").toBool();

    if (m_instances.contains(accountId)) {
        m_instances[accountId].state = InstanceState::Stopped;
    }

    if (exitStatus == QProcess::CrashExit) {
        emit instanceError(accountId,
            QString("Process crashed with exit code %1").arg(exitCode));
    }

    auto acct = m_accounts->account(accountId);

    // Capture GFXSettings from clone prefix for alt accounts (non-update, non-setup)
    if (!acct.isMain && !isUpdate && !isSetup) {
        QString accountDir = m_overlay->dataDir() + "/" + accountId;
        QString clonePrefix = accountDir + "/prefix";
        QString usersDir = clonePrefix + "/drive_c/users";
        QDir users(usersDir);
        if (users.exists()) {
            for (const auto &user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString gfxSrc = usersDir + "/" + user
                    + "/AppData/Roaming/Guild Wars 2/GFXSettings.Gw2-64.exe.xml";
                if (!QFile::exists(gfxSrc)) continue;

                QString savedDir = accountDir + "/saved";
                QDir().mkpath(savedDir);
                QString savedGfx = savedDir + "/GFXSettings.Gw2-64.exe.xml";
                QFile::remove(savedGfx);
                if (QFile::copy(gfxSrc, savedGfx)) {
                    emit instanceOutput(accountId, "Captured GFXSettings for next session.\n");
                }
                break;
            }
        }
    }

    // Handle Setup Account completion
    if (isSetup && !m_setupAccountId.isEmpty() && accountId == m_setupAccountId) {
        emit instanceOutput(accountId, "Game exited — capturing credentials...\n");

        bool success = false;
        QString savedDir = m_overlay->dataDir() + "/" + accountId + "/saved";
        QDir().mkpath(savedDir);
        QString savedLocalDat = savedDir + "/Local.dat";

        // Check if Local.dat was actually modified
        QString origMd5 = fileMd5(m_localDatBackupPath);
        QString newMd5 = fileMd5(m_localDatPath);

        if (origMd5 != newMd5) {
            // Local.dat changed — alt credentials were saved
            QFile::remove(savedLocalDat);
            if (QFile::copy(m_localDatPath, savedLocalDat)) {
                emit instanceOutput(accountId,
                    QString("Captured alt Local.dat [%1]\n").arg(fileInfo(savedLocalDat)));
                success = true;
            } else {
                emit instanceOutput(accountId, "ERROR: Failed to save captured Local.dat\n");
            }
        } else {
            emit instanceOutput(accountId,
                "WARNING: Local.dat was not modified. Did you log in as the alt "
                "with 'Remember' checked?\n");
        }

        // Restore main's Local.dat from backup
        QFile::remove(m_localDatPath);
        if (QFile::rename(m_localDatBackupPath, m_localDatPath)) {
            emit instanceOutput(accountId, "Restored main's Local.dat from backup.\n");
        } else {
            emit instanceOutput(accountId,
                "ERROR: Failed to restore Local.dat backup! "
                "Manual restore needed from: " + m_localDatBackupPath + "\n");
        }

        emit instanceOutput(accountId, "=== END Setup Account ===\n");
        m_setupAccountId.clear();
        m_localDatPath.clear();
        m_localDatBackupPath.clear();
        emit setupComplete(accountId, success);
    }

    // Handle Update Alt completion
    else if (isUpdate && !m_updateAccountId.isEmpty() && accountId == m_updateAccountId) {
        bool success = false;

        // Capture updated Local.dat back to alt's saved dir
        if (QFile::exists(m_updateLocalDatPath)) {
            QDir().mkpath(m_updateSavedDir);
            QString savedLocalDat = m_updateSavedDir + "/Local.dat";
            QFile::remove(savedLocalDat);
            if (QFile::copy(m_updateLocalDatPath, savedLocalDat)) {
                emit instanceOutput(accountId, "Captured updated Local.dat.\n");
                success = true;
            } else {
                emit instanceOutput(accountId, "ERROR: Failed to save updated Local.dat\n");
            }
        }

        // Restore main's Local.dat from backup
        QFile::remove(m_updateLocalDatPath);
        if (QFile::rename(m_updateBackupPath, m_updateLocalDatPath)) {
            emit instanceOutput(accountId, "Restored main's Local.dat.\n");
        } else {
            emit instanceOutput(accountId,
                "ERROR: Failed to restore Local.dat backup! "
                "Manual restore needed from: " + m_updateBackupPath + "\n");
        }

        emit instanceOutput(accountId, "=== END Update Alt ===\n");
        m_updateAccountId.clear();
        m_updateLocalDatPath.clear();
        m_updateBackupPath.clear();
        m_updateSavedDir.clear();
        emit updateComplete(accountId, success);

        // Process next alt in queue
        QTimer::singleShot(1000, this, [this]() {
            if (!m_updateQueue.isEmpty()) {
                QString nextId = m_updateQueue.takeFirst();
                if (!updateAlt(nextId, m_updateBasePrefix, m_updateExePath)) {
                    emit updateComplete(nextId, false);
                    if (!m_updateQueue.isEmpty()) {
                        QString nextId2 = m_updateQueue.takeFirst();
                        updateAlt(nextId2, m_updateBasePrefix, m_updateExePath);
                    } else {
                        emit allUpdatesComplete();
                    }
                }
            } else {
                emit allUpdatesComplete();
            }
        });
    }

    // Normal exit (main or alt)
    else {
        emit instanceOutput(accountId, "Game process exited.\n");

        // Patch detection: check if Gw2.dat was modified during main session
        if (acct.isMain && !m_gw2DatPath.isEmpty() && m_gw2DatMtimeBefore.isValid()) {
            QDateTime after = QFileInfo(m_gw2DatPath).lastModified();
            if (after.isValid() && after != m_gw2DatMtimeBefore) {
                emit instanceOutput(accountId,
                    "Gw2.dat was modified — a game patch was applied.\n");
                emit patchDetected();
            }
            m_gw2DatMtimeBefore = QDateTime();
        }
    }

    emit instanceStopped(accountId);
    proc->deleteLater();
}


void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    Q_UNUSED(error)
    auto *proc = qobject_cast<QProcess *>(sender());
    if (!proc) return;

    QString accountId = proc->property("accountId").toString();
    emit instanceError(accountId, proc->errorString());
}
