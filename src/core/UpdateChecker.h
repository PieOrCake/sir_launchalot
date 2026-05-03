#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);
    void check(const QString &currentVersion);

signals:
    void updateAvailable(const QString &latestVersion);

private:
    QNetworkAccessManager *m_nam;
};

#endif // UPDATECHECKER_H
