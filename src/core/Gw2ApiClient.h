#ifndef GW2APICLIENT_H
#define GW2APICLIENT_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QJsonObject>

class QNetworkAccessManager;
class QNetworkReply;

class Gw2ApiClient : public QObject
{
    Q_OBJECT

public:
    explicit Gw2ApiClient(QObject *parent = nullptr);

    struct AccountInfo {
        QString name;
        int world = 0;
        QString created;
        bool valid = false;
    };

    struct VaultStatus {
        int metaCurrent = 0;
        int metaComplete = 0;
        bool metaClaimed = false;
        int objectivesComplete = 0;
        int objectivesTotal = 0;
        bool valid = false;
    };

    struct AccountData {
        AccountInfo account;
        VaultStatus dailyVault;
        VaultStatus weeklyVault;
    };

    void fetchAccountData(const QString &accountId, const QString &apiKey);
    AccountData cachedData(const QString &accountId) const;

signals:
    void dataReady(const QString &accountId);
    void fetchError(const QString &accountId, const QString &error);

private:
    void doGet(const QString &accountId, const QString &apiKey,
               const QString &endpoint, const QString &tag);
    void handleReply(QNetworkReply *reply);
    VaultStatus parseVault(const QJsonObject &obj) const;

    QNetworkAccessManager *m_nam;
    QMap<QString, AccountData> m_cache;

    // Track pending requests per account: tag -> done
    struct PendingRequest {
        QString accountId;
        QString apiKey;
        int remaining = 0;
    };
    QMap<QNetworkReply *, QPair<QString, QString>> m_replyMap; // reply -> {accountId, tag}
    QMap<QString, int> m_pendingCount; // accountId -> remaining requests
};

#endif // GW2APICLIENT_H
