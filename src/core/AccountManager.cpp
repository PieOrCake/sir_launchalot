#include "core/AccountManager.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

AccountManager::AccountManager(QObject *parent)
    : QObject(parent)
{
    m_configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

void AccountManager::setConfigDir(const QString &dir)
{
    m_configDir = dir;
}

QString AccountManager::configDir() const
{
    return m_configDir;
}

void AccountManager::setBasePrefix(const QString &prefix)
{
    m_basePrefix = prefix;
    save();
}

QString AccountManager::basePrefix() const
{
    return m_basePrefix;
}

void AccountManager::setGw2ExePath(const QString &path)
{
    m_gw2ExePath = path;
    save();
}

QString AccountManager::gw2ExePath() const
{
    return m_gw2ExePath;
}

void AccountManager::setWineRunnerPath(const QString &path)
{
    m_wineRunnerPath = path;
    save();
}

QString AccountManager::wineRunnerPath() const
{
    return m_wineRunnerPath;
}

void AccountManager::setProtonPath(const QString &path)
{
    m_protonPath = path;
    save();
}

QString AccountManager::protonPath() const
{
    return m_protonPath;
}

void AccountManager::setApiRefreshInterval(int minutes)
{
    m_apiRefreshInterval = qBound(5, minutes, 60);
    save();
}

int AccountManager::apiRefreshInterval() const
{
    return m_apiRefreshInterval;
}

void AccountManager::setCheckForUpdatesEnabled(bool enabled)
{
    m_checkForUpdatesEnabled = enabled;
    save();
}

bool AccountManager::checkForUpdatesEnabled() const
{
    return m_checkForUpdatesEnabled;
}

QString AccountManager::configFilePath() const
{
    return m_configDir + "/accounts.json";
}

bool AccountManager::load()
{
    QFile file(configFilePath());
    if (!file.exists()) {
        return true; // no config yet, that's fine
    }

    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred("Failed to open accounts config: " + file.errorString());
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        emit errorOccurred("Failed to parse accounts config: " + parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();

    m_accounts.clear();
    QJsonArray arr = root.value("accounts").toArray();
    for (const auto &val : arr) {
        m_accounts.append(accountFromJson(val.toObject()));
    }

    m_externalApps.clear();
    QJsonArray appsArr = root.value("externalApps").toArray();
    for (const auto &val : appsArr) {
        m_externalApps.append(externalAppFromJson(val.toObject()));
    }

    m_itemOrder.clear();
    QJsonArray orderArr = root.value("itemOrder").toArray();
    for (const auto &val : orderArr) {
        m_itemOrder.append(val.toString());
    }

    // Ensure all items are in the order list (handles migration from old configs)
    for (const auto &acct : m_accounts) {
        if (!m_itemOrder.contains(acct.id)) m_itemOrder.append(acct.id);
    }
    for (const auto &app : m_externalApps) {
        if (!m_itemOrder.contains(app.id)) m_itemOrder.append(app.id);
    }

    m_basePrefix = root.value("basePrefix").toString();
    m_gw2ExePath = root.value("gw2ExePath").toString();
    m_wineRunnerPath = root.value("wineRunnerPath").toString();
    m_protonPath = root.value("protonPath").toString();
    m_apiRefreshInterval = root.value("apiRefreshInterval").toInt(15);
    m_checkForUpdatesEnabled = root.value("checkForUpdates").toBool(true);

    return true;
}

bool AccountManager::save() const
{
    QDir dir;
    if (!dir.mkpath(m_configDir)) {
        return false;
    }

    QJsonArray arr;
    for (const auto &acct : m_accounts) {
        arr.append(accountToJson(acct));
    }

    QJsonArray appsArr;
    for (const auto &app : m_externalApps) {
        appsArr.append(externalAppToJson(app));
    }

    QJsonArray orderArr;
    for (const auto &id : m_itemOrder) {
        orderArr.append(id);
    }

    QJsonObject root;
    root["accounts"] = arr;
    root["externalApps"] = appsArr;
    root["itemOrder"] = orderArr;
    root["basePrefix"] = m_basePrefix;
    root["gw2ExePath"] = m_gw2ExePath;
    root["wineRunnerPath"] = m_wineRunnerPath;
    root["protonPath"] = m_protonPath;
    root["apiRefreshInterval"] = m_apiRefreshInterval;
    root["checkForUpdates"] = m_checkForUpdatesEnabled;

    QFile file(configFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

QList<AccountManager::Account> AccountManager::accounts() const
{
    return m_accounts;
}

AccountManager::Account AccountManager::account(const QString &id) const
{
    for (const auto &acct : m_accounts) {
        if (acct.id == id) {
            return acct;
        }
    }
    return {};
}

bool AccountManager::addAccount(const Account &account)
{
    if (hasAccount(account.id)) {
        emit errorOccurred("Account already exists: " + account.id);
        return false;
    }

    // Demote existing main if this account is main
    if (account.isMain) {
        for (auto &a : m_accounts) {
            a.isMain = false;
        }
    }

    m_accounts.append(account);
    if (!m_itemOrder.contains(account.id)) m_itemOrder.append(account.id);
    save();
    emit accountAdded(account.id);
    return true;
}

bool AccountManager::updateAccount(const Account &account)
{
    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].id == account.id) {
            // Demote existing main if this account is becoming main
            if (account.isMain) {
                for (int j = 0; j < m_accounts.size(); ++j) {
                    if (j != i) m_accounts[j].isMain = false;
                }
            }
            m_accounts[i] = account;
            save();
            emit accountUpdated(account.id);
            return true;
        }
    }
    emit errorOccurred("Account not found: " + account.id);
    return false;
}

bool AccountManager::removeAccount(const QString &id)
{
    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].id == id) {
            m_accounts.removeAt(i);
            m_itemOrder.removeAll(id);
            save();
            emit accountRemoved(id);
            return true;
        }
    }
    return false;
}

bool AccountManager::hasAccount(const QString &id) const
{
    for (const auto &acct : m_accounts) {
        if (acct.id == id) {
            return true;
        }
    }
    return false;
}

bool AccountManager::injectAccountFiles(const QString &accountId,
                                         const QString &mergedPrefix,
                                         const QString &upperDir) const
{
    Account acct = account(accountId);
    if (acct.id.isEmpty()) {
        return false;
    }

    // Persistent saved dir for this account (independent of overlay lifecycle)
    QString savedDir = upperDir.isEmpty()
        ? QString()
        : QFileInfo(upperDir).absolutePath() + "/saved";

    // Find GW2 AppData under all Wine user directories (could be any username or "steamuser")
    QString usersDir = mergedPrefix + "/drive_c/users";
    QDir users(usersDir);
    QStringList gw2RelPaths;
    if (users.exists()) {
        for (const auto &user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString relPath = QString("drive_c/users/%1/AppData/Roaming/Guild Wars 2").arg(user);
            if (QDir(mergedPrefix + "/" + relPath).exists()) {
                gw2RelPaths.append(relPath);
            }
        }
    }
    if (gw2RelPaths.isEmpty()) {
        gw2RelPaths.append(QString("drive_c/users/%1/AppData/Roaming/Guild Wars 2")
                           .arg(qEnvironmentVariable("USER")));
    }

    for (const auto &gw2RelPath : gw2RelPaths) {
        // Write directly to the upper dir — overlayfs shows upper over lower,
        // so we don't need to manipulate files through the FUSE merged mount.
        QString upperGw2AppData = upperDir + "/" + gw2RelPath;
        QDir().mkpath(upperGw2AppData);

        QString upperLocalDat = upperGw2AppData + "/Local.dat";
        QString savedLocalDat = savedDir.isEmpty() ? QString()
            : savedDir + "/Local.dat";

        // Local.dat handling:
        //  1. If account has a localDatPath configured → copy it to upper
        //  2. If saved dir has a Local.dat (persisted from previous session) → copy to upper
        //  3. Otherwise → remove from upper so main's lowerdir copy shows (fresh login)
        //     (On fresh login, GW2 will write new Local.dat which goes to upper)
        if (!acct.localDatPath.isEmpty() && QFile::exists(acct.localDatPath)) {
            QFile::remove(upperLocalDat);
            QFile::copy(acct.localDatPath, upperLocalDat);
        } else if (!savedLocalDat.isEmpty() && QFile::exists(savedLocalDat)) {
            QFile::remove(upperLocalDat);
            QFile::copy(savedLocalDat, upperLocalDat);
        } else {
            // No saved credentials — remove from upper so GW2 shows fresh login
            // Note: the lowerdir (main's) copy WILL show through, but GW2 will
            // overwrite it when the user logs in, and the new file goes to upper.
            QFile::remove(upperLocalDat);
        }

        // Copy GFXSettings if we have one
        if (!acct.gfxSettingsPath.isEmpty() && QFile::exists(acct.gfxSettingsPath)) {
            QString dest = upperGw2AppData + "/GFXSettings.Gw2-64.exe.xml";
            QFile::remove(dest);
            QFile::copy(acct.gfxSettingsPath, dest);
        }
    }

    return true;
}

void AccountManager::reorderItems(const QStringList &orderedIds)
{
    m_itemOrder = orderedIds;
    save();
}

QStringList AccountManager::orderedItemIds() const
{
    return m_itemOrder;
}

QList<AccountManager::ExternalApp> AccountManager::externalApps() const
{
    return m_externalApps;
}

bool AccountManager::addExternalApp(const ExternalApp &app)
{
    m_externalApps.append(app);
    if (!m_itemOrder.contains(app.id)) m_itemOrder.append(app.id);
    save();
    return true;
}

bool AccountManager::updateExternalApp(const ExternalApp &app)
{
    for (int i = 0; i < m_externalApps.size(); ++i) {
        if (m_externalApps[i].id == app.id) {
            m_externalApps[i] = app;
            save();
            return true;
        }
    }
    return false;
}

bool AccountManager::removeExternalApp(const QString &id)
{
    for (int i = 0; i < m_externalApps.size(); ++i) {
        if (m_externalApps[i].id == id) {
            m_externalApps.removeAt(i);
            m_itemOrder.removeAll(id);
            save();
            return true;
        }
    }
    return false;
}

QJsonObject AccountManager::externalAppToJson(const ExternalApp &app) const
{
    QJsonObject obj;
    obj["id"] = app.id;
    obj["name"] = app.name;
    obj["command"] = app.command;
    return obj;
}

AccountManager::ExternalApp AccountManager::externalAppFromJson(const QJsonObject &obj) const
{
    ExternalApp app;
    app.id = obj.value("id").toString();
    app.name = obj.value("name").toString();
    app.command = obj.value("command").toString();
    return app;
}

QJsonObject AccountManager::accountToJson(const Account &account) const
{
    QJsonObject obj;
    obj["id"] = account.id;
    obj["displayName"] = account.displayName;
    obj["isMain"] = account.isMain;
    if (account.isSteam) obj["isSteam"] = true;
    if (!account.launchCommand.isEmpty()) obj["launchCommand"] = account.launchCommand;
    obj["email"] = account.email;
    obj["password"] = account.password;
    obj["localDatPath"] = account.localDatPath;
    obj["gfxSettingsPath"] = account.gfxSettingsPath;
    obj["enableAddons"] = account.enableAddons;

    QJsonArray argsArr;
    for (const auto &arg : account.extraArgs) {
        argsArr.append(arg);
    }
    obj["extraArgs"] = argsArr;

    QJsonObject envObj;
    for (auto it = account.envVars.constBegin(); it != account.envVars.constEnd(); ++it) {
        envObj[it.key()] = it.value();
    }
    obj["envVars"] = envObj;

    if (!account.apiKey.isEmpty()) obj["apiKey"] = account.apiKey;
    obj["showAccountName"] = account.showAccountName;
    obj["showDailyVault"] = account.showDailyVault;
    obj["showWeeklyVault"] = account.showWeeklyVault;

    return obj;
}

AccountManager::Account AccountManager::accountFromJson(const QJsonObject &obj) const
{
    Account acct;
    acct.id = obj.value("id").toString();
    acct.displayName = obj.value("displayName").toString();
    acct.isMain = obj.value("isMain").toBool(false);
    acct.isSteam = obj.value("isSteam").toBool(false);
    acct.launchCommand = obj.value("launchCommand").toString();
    acct.email = obj.value("email").toString();
    acct.password = obj.value("password").toString();
    acct.localDatPath = obj.value("localDatPath").toString();
    acct.gfxSettingsPath = obj.value("gfxSettingsPath").toString();
    acct.enableAddons = obj.value("enableAddons").toBool(true);

    QJsonArray argsArr = obj.value("extraArgs").toArray();
    for (const auto &val : argsArr) {
        acct.extraArgs.append(val.toString());
    }

    QJsonObject envObj = obj.value("envVars").toObject();
    for (auto it = envObj.constBegin(); it != envObj.constEnd(); ++it) {
        acct.envVars[it.key()] = it.value().toString();
    }

    acct.apiKey = obj.value("apiKey").toString();
    acct.showAccountName = obj.value("showAccountName").toBool(false);
    acct.showDailyVault = obj.value("showDailyVault").toBool(false);
    acct.showWeeklyVault = obj.value("showWeeklyVault").toBool(false);

    return acct;
}
