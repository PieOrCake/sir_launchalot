#ifndef WINEMANAGER_H
#define WINEMANAGER_H

#include <QObject>
#include <QString>
#include <QList>

class WineManager : public QObject
{
    Q_OBJECT

public:
    explicit WineManager(QObject *parent = nullptr);

    struct WineRunner {
        QString name;       // display name, e.g. "wine-ge-8-26-x86_64"
        QString path;       // full path to wine binary
        QString version;    // version string if known
        QString source;     // "system", "lutris", "proton", "custom"
    };

    QList<WineRunner> discoverRunners() const;
    WineRunner findRunner(const QString &name) const;

    void setSelectedRunner(const WineRunner &runner);
    WineRunner selectedRunner() const;

    static QString wineVersion(const QString &wineBinary);
    static QString prefixWineVersion(const QString &prefixPath);
    WineRunner bestRunnerForPrefix(const QList<WineRunner> &runners,
                                    const QString &prefixPath) const;

signals:
    void runnersDiscovered(const QList<WineRunner> &runners);

private:
    QList<WineRunner> discoverSystemWine() const;
    QList<WineRunner> discoverLutrisRunners() const;
    QList<WineRunner> discoverProtonRunners() const;

    WineRunner m_selectedRunner;
};

Q_DECLARE_METATYPE(WineManager::WineRunner)

#endif // WINEMANAGER_H
