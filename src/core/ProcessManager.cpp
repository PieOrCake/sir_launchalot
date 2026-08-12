#include "core/ProcessManager.h"
#include "core/OverlayManager.h"
#include "core/AccountManager.h"
#include "core/WineManager.h"
#include "core/ProtonResolver.h"

#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QTextStream>
#include <QTimer>
#include <QPointer>
#include <QSet>
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

// Files and directories that belong to addon frameworks (Nexus, arcdps, ReShade)
// rather than to the game itself. Note that bin64/ is deliberately absent — that
// is ArenaNet's own embedded browser (gem store, trading post), not an addon.
static const QStringList kAddonFiles = {
    "d3d11.dll", "d3d9.dll", "dxgi.dll",
    "ReShade.ini", "GW2-UOAOM.ini", "arcdps.ini"
};
static const QStringList kAddonDirs = {
    "addons", "reshade-shaders"
};

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
    // For alts: the folder the game is actually run from. Empty means "use the
    // shared install as-is". Set by the alt branch below.
    QString altGameDir;

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

        const QString proton = effectiveProtonPath(accountId);
        if (proton.isEmpty()) return false;  // blocked — dialog already requested

        QString scriptPath = writeUmuScript(accountId, basePrefix, exePath,
                                             mainArgs, gameid, proton, false);
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
        auto checkBusName = [this](const QString &accountId, const QString &output) {
            if (!m_sidecarPendingPrefix.contains(accountId)) return;
            static const QRegularExpression busRe("--bus-name=(:\\d+\\.\\d+)");
            auto m = busRe.match(output);
            if (m.hasMatch()) {
                QString winePrefix = m_sidecarPendingPrefix.take(accountId);
                launchSidecars(accountId, winePrefix, m.captured(1));
            }
        };
        connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, checkBusName]() {
            QString accountId = proc->property("accountId").toString();
            QString output = proc->readAllStandardOutput();
            emit instanceOutput(accountId, output);
            checkBusName(accountId, output);
        });
        connect(proc, &QProcess::readyReadStandardError, this, [this, proc, checkBusName]() {
            QString accountId = proc->property("accountId").toString();
            QString output = proc->readAllStandardError();
            emit instanceOutput(accountId, output);
            checkBusName(accountId, output);
        });

        InstanceInfo info;
        info.accountId = accountId;
        info.state = InstanceState::Starting;
        info.process = proc;
        m_instances[accountId] = info;

        m_sidecarPendingPrefix[accountId] = basePrefix;
        proc->start("/bin/bash", {scriptPath});
        if (!proc->waitForStarted(10000)) {
            m_sidecarPendingPrefix.remove(accountId);
            killSidecars(accountId);
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

        // Work out which copy of the game files this alt should run.
        //
        // Two install layouts exist. Lutris keeps the game inside the Wine prefix,
        // so the rsync above already produced a private copy — strip addons from it
        // and point Gw2.dat back at the base prefix. Faugus, Heroic and Steam keep
        // the game in a separate folder, so the clone holds no game files at all;
        // when addons are disabled we build a stripped copy of the game folder in
        // the account's own data dir, otherwise the shared original is used as-is.
        const QString sourceGameDir = QFileInfo(exePath).absolutePath();
        const bool gameInPrefix = exePath.startsWith(basePrefix + "/");

        if (gameInPrefix) {
            const QString relExeDir = QFileInfo(exePath.mid(basePrefix.length() + 1)).path();
            const QString cloneGameDir = (relExeDir == ".") ? clonePrefix
                                                            : clonePrefix + "/" + relExeDir;
            if (!acct.enableAddons) {
                stripAddons(accountId, cloneGameDir);
            }
            // Symlink Gw2.dat from the base prefix so alts share main's updated
            // game data instead of each needing their own ~80 GB copy.
            linkGw2Dat(accountId, sourceGameDir, cloneGameDir);
            altGameDir = cloneGameDir;
        } else if (!acct.enableAddons) {
            altGameDir = prepareStrippedGameDir(accountId, sourceGameDir);
            if (altGameDir.isEmpty()) return false;
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

    // For alt accounts, run the exe out of the account's own game dir so Wine
    // loads its DLLs from there (that is where addon files have been stripped).
    QString effectiveExePath = exePath;
    if (!acct.isMain && !altGameDir.isEmpty()) {
        effectiveExePath = altGameDir + "/" + QFileInfo(exePath).fileName();
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

    const QString proton = effectiveProtonPath(accountId);
    if (proton.isEmpty()) return false;  // blocked — dialog already requested

    QString scriptPath = writeUmuScript(accountId, winePrefix, effectiveExePath,
                                         gameArgs, gameid, proton, true);
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
    auto checkBusName = [this](const QString &accountId, const QString &output) {
        if (!m_sidecarPendingPrefix.contains(accountId)) return;
        static const QRegularExpression busRe("--bus-name=(:\\d+\\.\\d+)");
        auto m = busRe.match(output);
        if (m.hasMatch()) {
            QString winePrefix = m_sidecarPendingPrefix.take(accountId);
            launchSidecars(accountId, winePrefix, m.captured(1));
        }
    };
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, checkBusName]() {
        QString accountId = proc->property("accountId").toString();
        QString output = proc->readAllStandardOutput();
        emit instanceOutput(accountId, output);
        checkBusName(accountId, output);
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc, checkBusName]() {
        QString accountId = proc->property("accountId").toString();
        QString output = proc->readAllStandardError();
        emit instanceOutput(accountId, output);
        checkBusName(accountId, output);
    });

    InstanceInfo info;
    info.accountId = accountId;
    info.state = InstanceState::Starting;
    info.process = proc;
    m_instances[accountId] = info;

    m_sidecarPendingPrefix[accountId] = winePrefix;
    proc->start("/bin/bash", {scriptPath});
    if (!proc->waitForStarted(10000)) {
        m_sidecarPendingPrefix.remove(accountId);
        killSidecars(accountId);
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
    m_sidecarPendingPrefix.remove(accountId);
    killSidecars(accountId);

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

    // onProcessFinished may have fired inside waitForFinished and already
    // emitted instanceStopped — only emit if it hasn't done so yet
    if (info.state != InstanceState::Stopped) {
        info.state = InstanceState::Stopped;
        emit instanceStopped(accountId);
    }
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
    const QString proton = effectiveProtonPath(accountId);
    if (proton.isEmpty()) {  // blocked — dialog already requested
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_setupAccountId.clear();
        return false;
    }
    QString scriptPath = writeUmuScript(accountId, basePrefix, gw2ExePath,
                                         {}, "umu-1284210", proton, false);
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
    const QString proton = effectiveProtonPath(accountId);
    if (proton.isEmpty()) {  // blocked — dialog already requested
        QFile::remove(localDatPath);
        QFile::rename(backupPath, localDatPath);
        m_updateAccountId.clear();
        return false;
    }
    QString scriptPath = writeUmuScript(accountId, basePrefix, exePath,
                                         {"-image"}, "umu-1284210", proton, false);
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

QString ProcessManager::effectiveProtonPath(const QString &accountId)
{
    // An explicit user choice is always honoured as-is.
    if (!m_protonPath.isEmpty() && m_protonPath != QStringLiteral("GE-Proton"))
        return m_protonPath;

    // "Auto": pick the newest installed Proton whose Steam runtime is present.
    const ProtonResolver::Result res = ProtonResolver::forHost().resolve();
    switch (res.status) {
    case ProtonResolver::Status::Resolved:
        emit instanceOutput(accountId, res.logMessage + "\n");
        return res.protonPath;
    case ProtonResolver::Status::NoProtonInstalled:
        // No GE-Proton found — keep legacy behaviour and let umu fetch one.
        emit instanceOutput(accountId, res.logMessage + "\n");
        return QStringLiteral("GE-Proton");
    case ProtonResolver::Status::AllRuntimesMissing:
        emit launchBlocked(accountId, res.blockTitle, res.blockMessage);
        return QString();  // signal: abort the launch
    }
    return QStringLiteral("GE-Proton");
}

void ProcessManager::stripAddons(const QString &accountId, const QString &gameDir)
{
    emit instanceOutput(accountId, QString("Addon removal — game dir: %1\n").arg(gameDir));

    QDir gameDirObj(gameDir);
    QStringList dlls = gameDirObj.entryList({"*.dll"}, QDir::Files);
    if (!dlls.isEmpty()) {
        emit instanceOutput(accountId, QString("DLLs in game dir: %1\n").arg(dlls.join(", ")));
    }

    int removed = 0;
    for (const auto &f : kAddonFiles) {
        QString path = gameDir + "/" + f;
        if (QFile::exists(path)) {
            QFile::remove(path);
            emit instanceOutput(accountId, QString("  Removed: %1\n").arg(f));
            ++removed;
        }
    }
    for (const auto &d : kAddonDirs) {
        QString path = gameDir + "/" + d;
        if (QDir(path).exists()) {
            QDir(path).removeRecursively();
            emit instanceOutput(accountId, QString("  Removed dir: %1/\n").arg(d));
            ++removed;
        }
    }
    if (removed > 0) {
        emit instanceOutput(accountId,
            QString("Addons disabled — removed %1 addon file(s)/dir(s).\n").arg(removed));
    } else {
        emit instanceOutput(accountId, "Addons disabled — no addon files found to remove.\n");
    }

    QStringList remaining = QDir(gameDir).entryList({"*.dll"}, QDir::Files);
    if (!remaining.isEmpty()) {
        emit instanceOutput(accountId, QString("Remaining DLLs: %1\n").arg(remaining.join(", ")));
    }
}

void ProcessManager::linkGw2Dat(const QString &accountId, const QString &sourceGameDir,
                                 const QString &destGameDir)
{
    if (sourceGameDir == destGameDir) return;

    const QString src = sourceGameDir + "/Gw2.dat";
    const QString dest = destGameDir + "/Gw2.dat";
    if (!QFile::exists(src)) return;

    QFileInfo destInfo(dest);
    if (destInfo.isSymLink() && destInfo.symLinkTarget() == src) return;  // already correct
    if (destInfo.exists() || destInfo.isSymLink()) {
        QFile::remove(dest);
    }
    if (QFile::link(src, dest)) {
        emit instanceOutput(accountId, QString("Symlinked Gw2.dat -> %1\n").arg(src));
    } else {
        emit instanceError(accountId, "Failed to symlink Gw2.dat into " + destGameDir);
    }
}

QString ProcessManager::prepareStrippedGameDir(const QString &accountId,
                                                const QString &sourceGameDir)
{
    const QString destDir = m_overlay->dataDir() + "/" + accountId + "/game";
    if (!QDir().mkpath(destDir)) {
        emit instanceError(accountId, "Could not create addon-free game dir: " + destDir);
        return {};
    }

    emit instanceOutput(accountId,
        QString("Addons disabled — building addon-free game dir at %1 ...\n").arg(destDir));

    // Excluded paths are anchored to the transfer root so a same-named file
    // deeper in the tree is left alone. rsync also protects excluded paths from
    // --delete, which is what keeps the Gw2.dat symlink and the alt's own logs.
    QStringList args = {
        "-a", "--delete",
        "--exclude", "/Gw2.dat",
        "--exclude", "/Gw2.tmp",
        "--exclude", "/Gw2-64.tmp",
        "--exclude", "/debug.log",
        "--exclude", "/Crash.dmp"
    };
    for (const auto &f : kAddonFiles) args << "--exclude" << ("/" + f);
    for (const auto &d : kAddonDirs)  args << "--exclude" << ("/" + d + "/");
    args << sourceGameDir + "/" << destDir + "/";

    QProcess rsync;
    rsync.setProcessChannelMode(QProcess::MergedChannels);
    rsync.start("rsync", args);
    if (!rsync.waitForFinished(600000)) {
        emit instanceError(accountId, "rsync timed out building addon-free game dir");
        return {};
    }
    if (rsync.exitCode() != 0) {
        emit instanceError(accountId,
            "rsync failed building addon-free game dir: " + QString(rsync.readAll()));
        return {};
    }

    linkGw2Dat(accountId, sourceGameDir, destDir);
    emit instanceOutput(accountId, "Addon-free game dir ready.\n");
    return destDir;
}

QString ProcessManager::writeUmuScript(const QString &accountId, const QString &winePrefix,
                                        const QString &exePath, const QStringList &extraArgs,
                                        const QString &gameid, const QString &protonPath,
                                        bool useSetsid) const
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

    // PROTONPATH — resolved by the caller (smart auto-select or explicit path)
    QString proton = protonPath.isEmpty() ? "GE-Proton" : protonPath;
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

QString ProcessManager::windowsToLinuxPath(const QString &winPath, const QString &winePrefix) const
{
    QString path = winPath;
    path.replace('\\', '/');
    // Convert drive letter: C:/foo -> {winePrefix}/drive_c/foo
    if (path.size() >= 3 && path[1] == ':' && path[2] == '/') {
        QString drive = QStringLiteral("drive_") + path[0].toLower();
        path = winePrefix + "/" + drive + "/" + path.mid(3);
    }
    return path;
}

bool ProcessManager::isGw2RunningUnder(qint64 rootPid) const
{
    QDir proc("/proc");
    QMap<qint64, qint64> ppidMap;
    QMap<qint64, QString> commMap;

    for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok;
        qint64 pid = entry.toLongLong(&ok);
        if (!ok) continue;

        QFile statFile("/proc/" + entry + "/stat");
        if (statFile.open(QIODevice::ReadOnly)) {
            QByteArray data = statFile.readAll();
            int closeParen = data.lastIndexOf(')');
            if (closeParen >= 0) {
                QList<QByteArray> parts = data.mid(closeParen + 2).split(' ');
                if (parts.size() >= 2) {
                    qint64 ppid = parts[1].toLongLong(&ok);
                    if (ok) ppidMap[pid] = ppid;
                }
            }
        }

        QFile commFile("/proc/" + entry + "/comm");
        if (commFile.open(QIODevice::ReadOnly))
            commMap[pid] = QString::fromUtf8(commFile.readAll()).trimmed();
    }

    // Build children map and BFS from rootPid
    QMap<qint64, QList<qint64>> children;
    for (auto it = ppidMap.constBegin(); it != ppidMap.constEnd(); ++it)
        children[it.value()].append(it.key());

    QSet<qint64> visited;
    QList<qint64> queue = {rootPid};
    while (!queue.isEmpty()) {
        qint64 pid = queue.takeFirst();
        if (visited.contains(pid)) continue;
        visited.insert(pid);
        if (commMap.value(pid) == QLatin1String("Gw2-64.exe")) return true;
        for (qint64 child : children.value(pid))
            queue.append(child);
    }
    return false;
}

void ProcessManager::startGw2WatchTimer(const QString &accountId, qint64 rootPid)
{
    stopGw2WatchTimer(accountId);
    auto *timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this, accountId, rootPid]() {
        if (!m_instances.contains(accountId) ||
            m_instances[accountId].state != InstanceState::Running ||
            !m_sidecars.contains(accountId) ||
            m_sidecars[accountId].isEmpty()) {
            stopGw2WatchTimer(accountId);
            return;
        }
        if (!isGw2RunningUnder(rootPid)) {
            emit instanceOutput(accountId,
                "GW2 exited — stopping sidecars to release container.\n");
            stopGw2WatchTimer(accountId);
            killSidecars(accountId);
        }
    });
    m_gw2WatchTimers[accountId] = timer;
    timer->start();
}

void ProcessManager::stopGw2WatchTimer(const QString &accountId)
{
    if (m_gw2WatchTimers.contains(accountId)) {
        auto *timer = m_gw2WatchTimers.take(accountId);
        timer->stop();
        timer->deleteLater();
    }
}

void ProcessManager::killSidecars(const QString &accountId)
{
    stopGw2WatchTimer(accountId);
    if (!m_sidecars.contains(accountId)) return;
    const QList<QProcess*> procs = m_sidecars.take(accountId);
    for (auto *proc : procs) {
        proc->disconnect();
        proc->terminate();
        if (!proc->waitForFinished(3000))
            proc->kill();
        proc->deleteLater();
    }
}

QString ProcessManager::findWineBinary() const
{
    if (!m_protonPath.isEmpty() && m_protonPath != QStringLiteral("GE-Proton")) {
        QString wine64 = m_protonPath + "/files/bin/wine64";
        if (QFile::exists(wine64)) return wine64;
    }
    // umu stores GE-Proton under ~/.local/share/umu/compatibilitytools/
    QDir ctDir(QDir::homePath() + "/.local/share/umu/compatibilitytools");
    if (ctDir.exists()) {
        QStringList entries = ctDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const auto &entry : entries) {
            QString wine64 = ctDir.absoluteFilePath(entry) + "/files/bin/wine64";
            if (QFile::exists(wine64)) return wine64;
        }
    }
    QString wine64 = QStandardPaths::findExecutable("wine64");
    if (!wine64.isEmpty()) return wine64;
    return QStandardPaths::findExecutable("wine");
}

QString ProcessManager::findLaunchClient() const
{
    // steam-runtime-launch-client is installed alongside steamrt3 by umu
    QString path = QDir::homePath()
        + "/.local/share/umu/steamrt3/pressure-vessel/bin/steam-runtime-launch-client";
    return QFile::exists(path) ? path : QString();
}

void ProcessManager::launchSidecars(const QString &accountId, const QString &winePrefix,
                                     const QString &pvBusName)
{
    auto acct = m_accounts->account(accountId);
    if (acct.sidecars.isEmpty()) return;

    QString launchClient = findLaunchClient();
    // When injecting inside the container, use bare "wine64" — the container's PATH
    // (set up by umu-run for the correct Proton version) resolves it to the same
    // binary the wineserver was started with, avoiding version mismatches.
    QString wineBin = (!pvBusName.isEmpty() && !launchClient.isEmpty())
        ? QStringLiteral("wine64")
        : findWineBinary();

    if (!pvBusName.isEmpty() && !launchClient.isEmpty()) {
        emit instanceOutput(accountId,
            QString("Sidecar: injecting into pressure-vessel container %1\n").arg(pvBusName));
    } else if (wineBin.isEmpty()) {
        emit instanceOutput(accountId, "Sidecar: cannot find Wine binary, sidecars skipped.\n");
        return;
    }

    for (const auto &sidecar : acct.sidecars) {
        QString linuxPath = windowsToLinuxPath(sidecar.exePath, winePrefix);
        if (!QFile::exists(linuxPath)) {
            emit instanceOutput(accountId,
                QString("Sidecar '%1': not found at %2, skipping.\n")
                    .arg(sidecar.name, linuxPath));
            continue;
        }

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("WINEPREFIX", winePrefix);
        for (auto it = acct.envVars.constBegin(); it != acct.envVars.constEnd(); ++it)
            env.insert(it.key(), it.value());

        auto *proc = new QProcess(this);
        proc->setProcessEnvironment(env);
        proc->setProcessChannelMode(QProcess::MergedChannels);

        QPointer<QProcess> procPtr = proc;
        connect(proc, &QProcess::readyReadStandardOutput, this, [this, procPtr, accountId]() {
            if (procPtr) emit instanceOutput(accountId, procPtr->readAllStandardOutput());
        });

        QString scName = sidecar.name;
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, accountId, procPtr, scName](int exitCode, QProcess::ExitStatus) {
            emit instanceOutput(accountId,
                QString("Sidecar '%1' exited (code %2).\n").arg(scName).arg(exitCode));
            if (m_sidecars.contains(accountId) && procPtr)
                m_sidecars[accountId].removeAll(procPtr.data());
            if (procPtr) procPtr->deleteLater();
        });

        // Run inside GW2's pressure-vessel container if we have the bus name —
        // this shares GW2's wineserver so Windows named pipes are visible to both
        QString exe;
        QStringList args;
        if (!pvBusName.isEmpty() && !launchClient.isEmpty()) {
            exe = launchClient;
            args << "--bus-name=" + pvBusName << "--" << wineBin << linuxPath << sidecar.args;
        } else {
            exe = wineBin;
            args << linuxPath << sidecar.args;
        }
        proc->start(exe, args);

        if (!proc->waitForStarted(5000)) {
            emit instanceOutput(accountId,
                QString("Sidecar '%1': failed to start.\n").arg(scName));
            proc->deleteLater();
            continue;
        }

        m_sidecars[accountId].append(proc);
        emit instanceOutput(accountId,
            QString("Sidecar '%1' started (PID %2).\n")
                .arg(scName).arg(proc->processId()));
    }

    // Watch for GW2 exiting before umu-run does (e.g. closing the launcher before
    // logging in). Sidecars inside the pressure-vessel container keep it alive,
    // preventing umu-run from exiting naturally. When GW2 is gone, kill sidecars
    // so the container closes and umu-run can exit normally.
    if (m_instances.contains(accountId) && !m_sidecars.value(accountId).isEmpty())
        startGw2WatchTimer(accountId, m_instances[accountId].pid);
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

    m_sidecarPendingPrefix.remove(accountId);
    killSidecars(accountId);

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
