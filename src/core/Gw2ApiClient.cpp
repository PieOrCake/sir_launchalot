#include "core/Gw2ApiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

static const QString API_BASE = "https://api.guildwars2.com/v2";

Gw2ApiClient::Gw2ApiClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void Gw2ApiClient::fetchAccountData(const QString &accountId, const QString &apiKey)
{
    if (apiKey.isEmpty()) return;

    m_pendingCount[accountId] = 3;

    doGet(accountId, apiKey, "/account", "account");
    doGet(accountId, apiKey, "/account/wizardsvault/daily", "daily");
    doGet(accountId, apiKey, "/account/wizardsvault/weekly", "weekly");
}

Gw2ApiClient::AccountData Gw2ApiClient::cachedData(const QString &accountId) const
{
    return m_cache.value(accountId);
}

void Gw2ApiClient::doGet(const QString &accountId, const QString &apiKey,
                          const QString &endpoint, const QString &tag)
{
    QNetworkRequest req(QUrl(API_BASE + endpoint));
    req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_nam->get(req);
    m_replyMap[reply] = {accountId, tag};

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleReply(reply);
    });
}

void Gw2ApiClient::handleReply(QNetworkReply *reply)
{
    auto it = m_replyMap.find(reply);
    if (it == m_replyMap.end()) {
        reply->deleteLater();
        return;
    }

    QString accountId = it->first;
    QString tag = it->second;
    m_replyMap.erase(it);

    auto &data = m_cache[accountId];

    if (reply->error() != QNetworkReply::NoError) {
        // Don't emit error for vault endpoints on free accounts (403)
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 403 || tag == "account") {
            emit fetchError(accountId, QString("%1: %2").arg(tag, reply->errorString()));
        }
    } else {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject obj = doc.object();

        if (tag == "account") {
            data.account.name = obj.value("name").toString();
            data.account.world = obj.value("world").toInt();
            data.account.created = obj.value("created").toString();
            data.account.valid = true;
        } else if (tag == "daily") {
            data.dailyVault = parseVault(obj);
        } else if (tag == "weekly") {
            data.weeklyVault = parseVault(obj);
        }
    }

    reply->deleteLater();

    // Decrement pending count and emit when all done
    if (--m_pendingCount[accountId] <= 0) {
        m_pendingCount.remove(accountId);
        emit dataReady(accountId);
    }
}

Gw2ApiClient::VaultStatus Gw2ApiClient::parseVault(const QJsonObject &obj) const
{
    VaultStatus vs;
    vs.metaCurrent = obj.value("meta_progress_current").toInt();
    vs.metaComplete = obj.value("meta_progress_complete").toInt();
    vs.metaClaimed = obj.value("meta_reward_claimed").toBool();

    QJsonArray objectives = obj.value("objectives").toArray();
    vs.objectivesTotal = objectives.size();
    vs.objectivesComplete = 0;
    for (const auto &val : objectives) {
        QJsonObject o = val.toObject();
        if (o.value("claimed").toBool() ||
            o.value("progress_current").toInt() >= o.value("progress_complete").toInt()) {
            vs.objectivesComplete++;
        }
    }
    vs.valid = true;
    return vs;
}
