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
    loadCapturedEnv();
}

ProcessManager::~ProcessManager()
{
    stopAll();
}

void ProcessManager::setLaunchDelay(int msec)
{
    m_launchDelay = msec;
}

int ProcessManager::launchDelay() const
{
    return m_launchDelay;
}

void ProcessManager::setLutrisGameId(int id)
{
    m_lutrisGameId = id;
}

int ProcessManager::lutrisGameId() const
{
    return m_lutrisGameId;
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
        // Main account always launches via Lutris
        if (m_lutrisGameId <= 0) {
            emit instanceError(accountId, "No Lutris game ID — run Setup Wizard to detect your GW2 installation");
            return false;
        }

        // Record Gw2.dat modification time for patch detection
        m_gw2DatPath = QFileInfo(exePath).absolutePath() + "/Gw2.dat";
        m_gw2DatMtimeBefore = QFileInfo(m_gw2DatPath).lastModified();

        QString lutrisUri = QString("lutris:rungameid/%1").arg(m_lutrisGameId);
        emit instanceOutput(accountId, QString("Launching via Lutris: %1\n").arg(lutrisUri));

        // Use xdg-open to launch via the desktop's URI handler.
        // This spawns Lutris in a completely separate process context
        // managed by the desktop environment — no env inheritance issues.
        qint64 pid = 0;
        if (!QProcess::startDetached("xdg-open", {lutrisUri},
                                      QString(), &pid)) {
            emit instanceError(accountId, "Failed to launch Lutris via xdg-open");
            return false;
        }

        emit instanceOutput(accountId, QString("Lutris launched via xdg-open (PID %1)\n").arg(pid));

        InstanceInfo info;
        info.accountId = accountId;
        info.state = InstanceState::Running;
        info.pid = pid;
        info.process = nullptr;  // detached — no QProcess to manage
        info.launchTimer.start();
        m_instances[accountId] = info;

        // Start polling for game exit — check if wineserver is still alive
        m_basePrefix = basePrefix;
        if (!m_pollTimer) {
            m_pollTimer = new QTimer(this);
            connect(m_pollTimer, &QTimer::timeout,
                    this, &ProcessManager::pollDetachedInstances);
        }
        m_pollTimer->start(3000);  // check every 3 seconds

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

    // Alt account launch via script
    auto runner = m_wine->selectedRunner();
    if (runner.path.isEmpty()) {
        emit instanceError(accountId, "No Wine runner selected");
        return false;
    }

    // Use captured Lutris env if available, otherwise fall back to buildEnvironment
    QProcessEnvironment procEnv;
    if (m_envCaptured) {
        procEnv = m_capturedEnv;
        // Set WINEPREFIX and STEAM_COMPAT_DATA_PATH to the overlay merged dir
        procEnv.insert("WINEPREFIX", winePrefix);
        procEnv.insert("STEAM_COMPAT_DATA_PATH", winePrefix);

        // Derive PROTONPATH from WINELOADER if not already set.
        // umu-run needs this to select the correct Proton version;
        // without it, it defaults to UMU-Latest which causes prefix upgrades.
        if (procEnv.value("PROTONPATH").isEmpty()) {
            QString wineloader = procEnv.value("WINELOADER");
            if (wineloader.contains("/files/bin/")) {
                QString protonPath = wineloader.section("/files/bin/", 0, 0);
                procEnv.insert("PROTONPATH", protonPath);
                emit instanceOutput(accountId,
                    QString("Derived PROTONPATH: %1\n").arg(protonPath));
            }
        }

        emit instanceOutput(accountId, "Using captured Lutris environment\n");
    } else {
        procEnv = buildEnvironment(accountId, winePrefix);
        emit instanceOutput(accountId, "WARNING: No captured Lutris env — using built env (may show wine updater)\n");
    }

    // For alt accounts, remap exe path to the clone prefix so Wine loads
    // DLLs from the clone directory (where addon files may have been removed)
    QString effectiveExePath = exePath;
    if (!acct.isMain && exePath.startsWith(basePrefix)) {
        QString relPath = exePath.mid(basePrefix.length());  // e.g. /drive_c/.../Gw2-64.exe
        effectiveExePath = winePrefix + relPath;
        procEnv.insert("STEAM_COMPAT_INSTALL_PATH",
                        QFileInfo(effectiveExePath).absolutePath());
    }

    QStringList launchArgs = buildLaunchArgs(effectiveExePath, accountId);
    QString workDir = QFileInfo(effectiveExePath).absolutePath();

    // Debug: log what we're about to launch
    emit instanceOutput(accountId, QString("CMD: %1 %2\n").arg(runner.path, launchArgs.join(" ")));
    emit instanceOutput(accountId, QString("CWD: %1\n").arg(workDir));
    emit instanceOutput(accountId, QString("WINEPREFIX: %1\n").arg(winePrefix));

    // Create a .desktop file so KDE shows this as a separate panel icon
    uint idHash = qHash(accountId) % 10000;
    QString uniqueAppId = QString::number(1284210000 + idHash);
    QString windowClass = "steam_app_" + uniqueAppId;
    QString displayName = acct.displayName.isEmpty() ? accountId : acct.displayName;
    {
        QString appsDir = QDir::homePath() + "/.local/share/applications";
        QDir().mkpath(appsDir);
        QString desktopPath = appsDir + "/sir-launchalot-" + accountId + ".desktop";
        QFile desktop(desktopPath);
        if (desktop.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ds(&desktop);
            ds << "[Desktop Entry]\n";
            ds << "Type=Application\n";
            ds << "Name=Guild Wars 2 - " << displayName << "\n";
            ds << "Exec=true\n";
            ds << "StartupWMClass=" << windowClass << "\n";
            ds << "NoDisplay=true\n";
            ds << "Icon=lutris_guild-wars-2\n";
        }
    }

    // Write the launch script
    QString scriptPath = QDir::tempPath() + "/sir-launchalot-" + accountId + ".sh";
    {
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
            emit instanceError(accountId, "Failed to create launch script");
            return false;
        }
        QTextStream out(&script);
        out << "#!/bin/bash\n";

        // Only export Wine/game-specific vars from captured env.
        // Display, session, and system vars are inherited from the parent
        // process to avoid X11 auth and Vulkan issues.
        QStringList exportKeys = {
            "WINEPREFIX", "WINEARCH", "WINE", "WINELOADER",
            "WINELOADERNOEXEC", "WINEDEBUG",
            "WINEESYNC", "WINEFSYNC", "WINEDLLOVERRIDES",
            "WINE_MONO_CACHE_DIR", "WINE_GECKO_CACHE_DIR",
            "WINE_LARGE_ADDRESS_AWARE", "WINE_FULLSCREEN_FSR",
            "PROTONPATH", "GAMEID", "STORE",
            "STEAM_COMPAT_APP_ID", "STEAM_COMPAT_INSTALL_PATH",
            "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH",
            "STEAM_COMPAT_MOUNTS",
            "DXVK_LOG_LEVEL", "DXVK_NVAPIHACK", "DXVK_ENABLE_NVAPI",
            "PROTON_DXVK_D3D8", "PROTON_BATTLEYE_RUNTIME", "PROTON_EAC_RUNTIME",
            "MANGOHUD", "MANGOHUD_DLSYM", "UMU_LOG",
            "GST_PLUGIN_SYSTEM_PATH_1_0",
            "WINE_GST_REGISTRY_DIR",
            "MEDIACONV_AUDIO_DUMP_FILE", "MEDIACONV_AUDIO_TRANSCODED_FILE",
            "MEDIACONV_VIDEO_DUMP_FILE", "MEDIACONV_VIDEO_TRANSCODED_FILE",
            "__GL_SHADER_DISK_CACHE", "__GL_SHADER_DISK_CACHE_PATH",
            "PULSE_LATENCY_MSEC",
            "LD_LIBRARY_PATH",
            "STAGING_SHARED_MEMORY",
        };
        for (const auto &key : exportKeys) {
            QString val = procEnv.value(key);
            if (val.isEmpty()) continue;
            val.replace("'", "'\\''");
            out << "export " << key << "='" << val << "'\n";
        }

        // Override GAMEID for proper KDE window class identification.
        // Each account gets a unique numeric GAMEID so KDE can apply per-window rules.
        out << "export GAMEID='umu-" << uniqueAppId << "'\n";
        out << "export SteamAppId='" << uniqueAppId << "'\n";
        out << "export STEAM_COMPAT_APP_ID='" << uniqueAppId << "'\n";

        QString escapedWorkDir = workDir;
        escapedWorkDir.replace("'", "'\\''");
        out << "\ncd '" << escapedWorkDir << "'\n";

        // Use the Wine binary from the captured/built env (WINE var) if available,
        // otherwise fall back to our detected runner
        QString wineBin = procEnv.value("WINE");
        if (wineBin.isEmpty()) wineBin = runner.path;
        QString escapedWineBin = wineBin;
        escapedWineBin.replace("'", "'\\''");

        out << "\nexec setsid --wait '" << escapedWineBin << "'";
        for (const auto &arg : launchArgs) {
            QString escaped = arg;
            escaped.replace("'", "'\\''");
            out << " '" << escaped << "'";
        }
        out << "\n";

        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
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

    if (m_lutrisGameId <= 0) {
        emit instanceError(accountId, "No Lutris game ID — cannot launch for setup");
        return false;
    }

    QString basePrefix = m_accounts->basePrefix();
    if (basePrefix.isEmpty()) {
        emit instanceError(accountId, "No base prefix configured");
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
        "Launching GW2 via Lutris. Please:\n"
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
    m_basePrefix = basePrefix;

    // Launch via Lutris
    QString lutrisUri = QString("lutris:rungameid/%1").arg(m_lutrisGameId);
    qint64 pid = 0;
    if (!QProcess::startDetached("xdg-open", {lutrisUri}, QString(), &pid)) {
        // Restore backup on failure
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_setupAccountId.clear();
        emit instanceError(accountId, "Failed to launch Lutris for setup");
        return false;
    }

    emit instanceOutput(accountId, QString("Lutris launched (PID %1) — waiting for game to exit...\n").arg(pid));

    // Track as a detached instance so polling picks it up
    InstanceInfo info;
    info.accountId = accountId;
    info.state = InstanceState::Running;
    info.pid = pid;
    info.process = nullptr;
    m_instances[accountId] = info;

    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout,
                this, &ProcessManager::pollDetachedInstances);
    }
    m_pollTimer->start(3000);

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

    // Build a launch script using the base prefix (not a clone)
    auto runner = m_wine->selectedRunner();
    if (runner.path.isEmpty()) {
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_updateAccountId.clear();
        emit instanceError(accountId, "No Wine runner selected");
        return false;
    }

    QProcessEnvironment procEnv;
    if (m_envCaptured) {
        procEnv = m_capturedEnv;
    } else {
        procEnv = buildEnvironment(accountId, basePrefix);
    }

    QString scriptPath = QDir::tempPath() + "/sir-launchalot-update-" + accountId + ".sh";
    {
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QFile::remove(localDatPath);
            QFile::rename(backupPath, localDatPath);
            m_updateAccountId.clear();
            emit instanceError(accountId, "Failed to create update script");
            return false;
        }
        QTextStream out(&script);
        out << "#!/bin/bash\n";

        QStringList exportKeys = {
            "WINEPREFIX", "WINEARCH", "WINE", "WINELOADER",
            "WINELOADERNOEXEC", "WINEDEBUG",
            "WINEESYNC", "WINEFSYNC", "WINEDLLOVERRIDES",
            "WINE_MONO_CACHE_DIR", "WINE_GECKO_CACHE_DIR",
            "WINE_LARGE_ADDRESS_AWARE", "WINE_FULLSCREEN_FSR",
            "PROTONPATH", "GAMEID", "STORE",
            "STEAM_COMPAT_APP_ID", "STEAM_COMPAT_INSTALL_PATH",
            "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH",
            "STEAM_COMPAT_MOUNTS",
            "DXVK_LOG_LEVEL", "DXVK_NVAPIHACK", "DXVK_ENABLE_NVAPI",
            "PROTON_DXVK_D3D8", "PROTON_BATTLEYE_RUNTIME", "PROTON_EAC_RUNTIME",
            "MANGOHUD", "MANGOHUD_DLSYM", "UMU_LOG",
            "GST_PLUGIN_SYSTEM_PATH_1_0",
            "WINE_GST_REGISTRY_DIR",
            "MEDIACONV_AUDIO_DUMP_FILE", "MEDIACONV_AUDIO_TRANSCODED_FILE",
            "MEDIACONV_VIDEO_DUMP_FILE", "MEDIACONV_VIDEO_TRANSCODED_FILE",
            "__GL_SHADER_DISK_CACHE", "__GL_SHADER_DISK_CACHE_PATH",
            "PULSE_LATENCY_MSEC",
            "LD_LIBRARY_PATH",
            "STAGING_SHARED_MEMORY",
        };
        for (const auto &key : exportKeys) {
            QString val = procEnv.value(key);
            if (val.isEmpty()) continue;
            val.replace("'", "'\\''");
            out << "export " << key << "='" << val << "'\n";
        }

        // Override WINEPREFIX to the base prefix
        QString escapedBasePrefix = basePrefix;
        escapedBasePrefix.replace("'", "'\\''");
        out << "export WINEPREFIX='" << escapedBasePrefix << "'\n";

        QString workDir = QFileInfo(exePath).absolutePath();
        QString escapedWorkDir = workDir;
        escapedWorkDir.replace("'", "'\\''");
        out << "\ncd '" << escapedWorkDir << "'\n";

        QString wineBin = procEnv.value("WINE");
        if (wineBin.isEmpty()) wineBin = runner.path;
        QString escapedWineBin = wineBin;
        escapedWineBin.replace("'", "'\\''");
        QString escapedExe = exePath;
        escapedExe.replace("'", "'\\''");

        out << "\nexec '" << escapedWineBin << "' '" << escapedExe << "' -image\n";

        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
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

void ProcessManager::pollDetachedInstances()
{
    // Try to capture the Lutris environment on the first poll tick
    if (!m_envCaptured) {
        captureLutrisEnvironment();
    }

    // Check if any detached (Lutris-launched) instances are still running
    // by looking for a wineserver process using the base prefix
    bool anyDetached = false;

    for (auto it = m_instances.begin(); it != m_instances.end(); ++it) {
        auto &info = it.value();
        if (info.state != InstanceState::Running || info.process != nullptr)
            continue;  // skip non-detached or non-running

        anyDetached = true;

        // Check if wineserver is still running specifically for our prefix
        // by examining /proc/<pid>/environ of each wineserver process
        QProcess check;
        check.start("pgrep", {"wineserver"});
        check.waitForFinished(3000);
        QString output = check.readAllStandardOutput();

        bool wineserverAlive = false;
        QString basePrefixCanonical = QFileInfo(m_basePrefix).canonicalFilePath();
        for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
            int pid = line.trimmed().toInt();
            if (pid <= 0) continue;

            QFile envFile(QString("/proc/%1/environ").arg(pid));
            if (!envFile.open(QIODevice::ReadOnly)) continue;
            QByteArray envData = envFile.readAll();
            envFile.close();

            for (const QByteArray &entry : envData.split('\0')) {
                if (entry.startsWith("WINEPREFIX=")) {
                    QString prefix = QString::fromLocal8Bit(entry.mid(11));
                    if (QFileInfo(prefix).canonicalFilePath() == basePrefixCanonical) {
                        wineserverAlive = true;
                    }
                    break;
                }
            }
            if (wineserverAlive) break;
        }

        if (!wineserverAlive) {
            // Grace period: don't mark as stopped if launched recently
            // (Lutris/Wine takes time to start the wineserver)
            if (info.launchTimer.isValid() && info.launchTimer.elapsed() < 30000) {
                continue;
            }

            // No wineserver running — game has exited
            info.state = InstanceState::Stopped;

            // Handle Setup Account completion
            if (!m_setupAccountId.isEmpty() && info.accountId == m_setupAccountId) {
                QString accountId = m_setupAccountId;
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
            } else {
                emit instanceOutput(info.accountId, "Game process exited.\n");

                // Patch detection: check if Gw2.dat was modified during main session
                auto acct = m_accounts->account(info.accountId);
                if (acct.isMain && !m_gw2DatPath.isEmpty() && m_gw2DatMtimeBefore.isValid()) {
                    QDateTime after = QFileInfo(m_gw2DatPath).lastModified();
                    if (after.isValid() && after != m_gw2DatMtimeBefore) {
                        emit instanceOutput(info.accountId,
                            "Gw2.dat was modified — a game patch was applied.\n");
                        emit patchDetected();
                    }
                    m_gw2DatMtimeBefore = QDateTime();
                }
            }

            emit instanceStopped(info.accountId);
        }
    }

    // Stop polling if no detached instances remain
    if (!anyDetached && m_pollTimer) {
        m_pollTimer->stop();
    }
}

void ProcessManager::captureLutrisEnvironment()
{
    if (m_envCaptured || m_basePrefix.isEmpty()) return;

    // Find Wine processes and check which one uses our prefix
    QProcess pgrep;
    pgrep.start("pgrep", {"-a", "wine"});
    pgrep.waitForFinished(3000);
    QString pgrepOutput = pgrep.readAllStandardOutput();

    for (const QString &line : pgrepOutput.split('\n', Qt::SkipEmptyParts)) {
        // Extract PID from "12345 /path/to/wine ..."
        QString pidStr = line.section(' ', 0, 0).trimmed();
        bool ok;
        int pid = pidStr.toInt(&ok);
        if (!ok || pid <= 0) continue;

        // Read /proc/<pid>/environ
        QString envPath = QString("/proc/%1/environ").arg(pid);
        QFile envFile(envPath);
        if (!envFile.open(QIODevice::ReadOnly)) continue;

        QByteArray envData = envFile.readAll();
        envFile.close();

        // Parse null-separated KEY=VALUE pairs
        QProcessEnvironment env;
        for (const QByteArray &entry : envData.split('\0')) {
            if (entry.isEmpty()) continue;
            int eq = entry.indexOf('=');
            if (eq <= 0) continue;
            env.insert(QString::fromLocal8Bit(entry.left(eq)),
                       QString::fromLocal8Bit(entry.mid(eq + 1)));
        }

        // Check if this process uses our prefix
        QString prefix = env.value("WINEPREFIX");
        if (prefix.isEmpty()) continue;
        if (QFileInfo(prefix).canonicalFilePath() ==
            QFileInfo(m_basePrefix).canonicalFilePath()) {
            m_capturedEnv = env;
            m_envCaptured = true;
            saveCapturedEnv();
            emit instanceOutput("main",
                QString("Captured Lutris environment from PID %1 (%2 vars)\n")
                    .arg(pid).arg(env.keys().size()));
            return;
        }
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
    QStringList args;
    args << exePath;

    // Only add -shareArchive for alt accounts (multiboxing)
    auto acct = m_accounts->account(accountId);
    if (!acct.isMain) {
        args << "-shareArchive";

        // Auto-login from saved Local.dat (generated via Setup Account)
        QString savedLocalDat = m_overlay->dataDir() + "/" + accountId + "/saved/Local.dat";
        if (QFile::exists(savedLocalDat))
            args << "-autologin";
    }

    // Add per-account extra args
    args.append(acct.extraArgs);

    return args;
}

QProcessEnvironment ProcessManager::buildEnvironment(const QString &accountId,
                                                       const QString &mergedPrefix) const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // Set the WINEPREFIX
    env.insert("WINEPREFIX", mergedPrefix);
    env.insert("WINEARCH", "win64");

    auto runner = m_wine->selectedRunner();

    if (runner.source == "lutris" || runner.source == "proton") {
        // Derive runner base dir from binary path (e.g. .../bin/wine -> ...)
        QFileInfo binInfo(runner.path);
        QString runnerDir = binInfo.dir().absolutePath();  // .../bin
        runnerDir = QFileInfo(runnerDir).dir().absolutePath();  // runner root

        // Wine binary and loader — WINELOADER is critical for subprocess spawning
        env.insert("WINE", runner.path);
        env.insert("WINELOADER", runner.path);

        // Point Wine to the runner's bundled mono/gecko instead of downloading
        env.insert("WINE_MONO_CACHE_DIR", runnerDir + "/mono");
        env.insert("WINE_GECKO_CACHE_DIR", runnerDir + "/gecko");

        // Enable esync/fsync — required to match prefix state set by Lutris
        env.insert("WINEESYNC", "1");
        env.insert("WINEFSYNC", "1");
        env.insert("WINEDEBUG", "-all");

        // DXVK native DLL overrides — must match Lutris exactly (no mscoree/mshtml)
        env.insert("WINEDLLOVERRIDES",
            "d3d10core,d3d11,d3d12,d3d12core,d3d8,d3d9,"
            "d3dcompiler_33,d3dcompiler_34,d3dcompiler_35,d3dcompiler_36,"
            "d3dcompiler_37,d3dcompiler_38,d3dcompiler_39,d3dcompiler_40,"
            "d3dcompiler_41,d3dcompiler_42,d3dcompiler_43,d3dcompiler_46,"
            "d3dcompiler_47,d3dx10,d3dx10_33,d3dx10_34,d3dx10_35,d3dx10_36,"
            "d3dx10_37,d3dx10_38,d3dx10_39,d3dx10_40,d3dx10_41,d3dx10_42,"
            "d3dx10_43,d3dx11_42,d3dx11_43,d3dx9_24,d3dx9_25,d3dx9_26,"
            "d3dx9_27,d3dx9_28,d3dx9_29,d3dx9_30,d3dx9_31,d3dx9_32,"
            "d3dx9_33,d3dx9_34,d3dx9_35,d3dx9_36,d3dx9_37,d3dx9_38,"
            "d3dx9_39,d3dx9_40,d3dx9_41,d3dx9_42,d3dx9_43,"
            "dxgi,nvapi,nvapi64,nvofapi64=n;"
            "winemenubuilder=");

        // DXVK settings
        env.insert("DXVK_LOG_LEVEL", "error");
        env.insert("DXVK_NVAPIHACK", "0");
        env.insert("DXVK_ENABLE_NVAPI", "1");

        // Wine / Proton extras
        env.insert("WINE_FULLSCREEN_FSR", "1");
        env.insert("WINE_LARGE_ADDRESS_AWARE", "1");
        env.insert("PROTON_DXVK_D3D8", "1");
        env.insert("MANGOHUD", "0");
        env.insert("MANGOHUD_DLSYM", "0");
        env.insert("UMU_LOG", "1");

        // Anti-cheat runtimes
        QString lutrisBase = QDir::homePath() + "/.local/share/lutris/runtime";
        env.insert("PROTON_BATTLEYE_RUNTIME", lutrisBase + "/battleye_runtime");
        env.insert("PROTON_EAC_RUNTIME", lutrisBase + "/eac_runtime");

        // GStreamer plugin path — GW2 uses GStreamer for launcher media
        env.insert("GST_PLUGIN_SYSTEM_PATH_1_0",
            runnerDir + "/lib64/gstreamer-1.0/:" +
            runnerDir + "/lib/gstreamer-1.0/");

        // Shader cache and audio
        env.insert("__GL_SHADER_DISK_CACHE", "1");
        env.insert("__GL_SHADER_DISK_CACHE_PATH", mergedPrefix);
        env.insert("PULSE_LATENCY_MSEC", "60");
        env.insert("TERM", "xterm");

        // Library paths — must include runner libs, system libs, and Lutris runtime
        QString ldPath = runnerDir + "/lib:" + runnerDir + "/lib64";
        ldPath += ":/lib64:/lib:/usr/lib64:/usr/lib";
        ldPath += ":" + lutrisBase + "/Ubuntu-18.04-i686";
        ldPath += ":" + lutrisBase + "/steam/i386/lib/i386-linux-gnu";
        ldPath += ":" + lutrisBase + "/steam/i386/lib";
        ldPath += ":" + lutrisBase + "/steam/i386/usr/lib/i386-linux-gnu";
        ldPath += ":" + lutrisBase + "/steam/i386/usr/lib";
        ldPath += ":" + lutrisBase + "/Ubuntu-18.04-x86_64";
        ldPath += ":" + lutrisBase + "/steam/amd64/lib/x86_64-linux-gnu";
        ldPath += ":" + lutrisBase + "/steam/amd64/lib";
        ldPath += ":" + lutrisBase + "/steam/amd64/usr/lib/x86_64-linux-gnu";
        ldPath += ":" + lutrisBase + "/steam/amd64/usr/lib";
        env.insert("LD_LIBRARY_PATH", ldPath);
    } else {
        // System Wine — minimal overrides
        env.insert("WINEDLLOVERRIDES", "mscoree=;mshtml=");
    }

    // Add per-account environment variables (can override above)
    auto acct = m_accounts->account(accountId);
    for (auto it = acct.envVars.constBegin(); it != acct.envVars.constEnd(); ++it) {
        env.insert(it.key(), it.value());
    }

    return env;
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    auto *proc = qobject_cast<QProcess *>(sender());
    if (!proc) return;

    QString accountId = proc->property("accountId").toString();
    bool isUpdate = proc->property("isUpdate").toBool();

    if (m_instances.contains(accountId)) {
        m_instances[accountId].state = InstanceState::Stopped;
    }

    if (exitStatus == QProcess::CrashExit) {
        emit instanceError(accountId,
            QString("Process crashed with exit code %1").arg(exitCode));
    }

    // Capture GFXSettings from clone prefix for alt accounts
    auto acct = m_accounts->account(accountId);
    if (!acct.isMain && !proc->property("isUpdate").toBool()) {
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

    // Handle Update Alt completion
    if (isUpdate && !m_updateAccountId.isEmpty() && accountId == m_updateAccountId) {
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
                    // Try next if this one failed
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

    emit instanceStopped(accountId);
    proc->deleteLater();
}


void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    auto *proc = qobject_cast<QProcess *>(sender());
    if (!proc) return;

    QString accountId = proc->property("accountId").toString();
    emit instanceError(accountId, proc->errorString());
}

QString ProcessManager::envFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + "/captured_env.json";
}

void ProcessManager::saveCapturedEnv() const
{
    if (!m_envCaptured) return;

    QJsonObject obj;
    for (const auto &key : m_capturedEnv.keys()) {
        obj[key] = m_capturedEnv.value(key);
    }

    QString path = envFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

void ProcessManager::loadCapturedEnv()
{
    QFile file(envFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    m_capturedEnv = QProcessEnvironment();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        m_capturedEnv.insert(it.key(), it.value().toString());
    }
    m_envCaptured = true;
}
