#ifndef ACCOUNTMANAGER_H
#define ACCOUNTMANAGER_H

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>

class AccountManager : public QObject
{
    Q_OBJECT

public:
    explicit AccountManager(QObject *parent = nullptr);

    struct ExternalApp {
        QString id;
        QString name;
        QString command;
    };

    struct Account {
        QString id;
        QString displayName;
        bool isMain = false;        // main account uses base WINEPREFIX directly
        QString email;              // GW2 login email (passed via -email flag)
        QString password;           // GW2 login password (passed via -password flag)
        QString localDatPath;       // stored copy of Local.dat
        QString gfxSettingsPath;    // stored copy of GFXSettings
        bool enableAddons = true;    // include addon DLLs when cloning prefix
        QStringList extraArgs;      // additional GW2 command-line args
        QMap<QString, QString> envVars; // additional environment variables
    };

    bool load();
    bool save() const;

    QList<Account> accounts() const;
    Account account(const QString &id) const;
    bool addAccount(const Account &account);
    bool updateAccount(const Account &account);
    bool removeAccount(const QString &id);
    bool hasAccount(const QString &id) const;

    void reorderItems(const QStringList &orderedIds);
    QStringList orderedItemIds() const;

    QList<ExternalApp> externalApps() const;
    bool addExternalApp(const ExternalApp &app);
    bool updateExternalApp(const ExternalApp &app);
    bool removeExternalApp(const QString &id);

    bool injectAccountFiles(const QString &accountId, const QString &mergedPrefix,
                            const QString &upperDir = QString()) const;

    void setConfigDir(const QString &dir);
    QString configDir() const;

    void setBasePrefix(const QString &prefix);
    QString basePrefix() const;
    void setGw2ExePath(const QString &path);
    QString gw2ExePath() const;
    void setWineRunnerPath(const QString &path);
    QString wineRunnerPath() const;

signals:
    void accountAdded(const QString &id);
    void accountRemoved(const QString &id);
    void accountUpdated(const QString &id);
    void errorOccurred(const QString &error);

private:
    QString configFilePath() const;
    QJsonObject externalAppToJson(const ExternalApp &app) const;
    ExternalApp externalAppFromJson(const QJsonObject &obj) const;

    QJsonObject accountToJson(const Account &account) const;
    Account accountFromJson(const QJsonObject &obj) const;

    QString m_configDir;
    QList<Account> m_accounts;
    QList<ExternalApp> m_externalApps;
    QStringList m_itemOrder;
    QString m_basePrefix;
    QString m_gw2ExePath;
    QString m_wineRunnerPath;
};

#endif // ACCOUNTMANAGER_H
