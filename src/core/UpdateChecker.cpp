#include "core/UpdateChecker.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVersionNumber>
#include <QUrl>

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void UpdateChecker::check(const QString &currentVersion)
{
    QNetworkRequest req(QUrl("https://api.github.com/repos/PieOrCake/sir_launchalot/releases/latest"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "sir-launchalot");

    auto *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, currentVersion]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        auto doc = QJsonDocument::fromJson(reply->readAll());
        QString tag = doc.object().value("tag_name").toString();
        if (tag.startsWith('v'))
            tag.remove(0, 1);

        auto latest = QVersionNumber::fromString(tag);
        auto current = QVersionNumber::fromString(currentVersion);
        if (!latest.isNull() && latest > current)
            emit updateAvailable(tag);
    });
}
