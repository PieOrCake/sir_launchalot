#ifndef OVERLAYMANAGER_H
#define OVERLAYMANAGER_H

#include <QObject>
#include <QString>

class OverlayManager : public QObject
{
    Q_OBJECT

public:
    explicit OverlayManager(QObject *parent = nullptr);
    ~OverlayManager() = default;

    void setDataDir(const QString &dataDir);
    QString dataDir() const;

signals:
    void errorOccurred(const QString &accountId, const QString &error);

private:
    QString m_dataDir;
};

#endif // OVERLAYMANAGER_H
