#ifndef INSTALLDETECTOR_H
#define INSTALLDETECTOR_H

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>

class InstallDetector : public QObject
{
    Q_OBJECT

public:
    explicit InstallDetector(QObject *parent = nullptr);

    struct DetectedInstall {
        QString name;
        QString slug;
        QString runner;         // e.g. "wine"
        QString winePrefix;     // WINEPREFIX path
        QString wineBinary;     // path to wine binary used
        QString exePath;        // path to Gw2-64.exe
        QString protonPath;     // PROTONPATH for umu-run (empty = auto "GE-Proton")
        QString source;         // "lutris", "heroic", "faugus", "steam"
        QMap<QString, QString> envVars;
    };

    bool isLutrisInstalled() const;
    static bool isUmuInstalled();
    QList<DetectedInstall> discoverGW2Installs() const;
    DetectedInstall parseGameConfig(const QString &configPath) const;

    static QString gw2ExeInPrefix(const QString &winePrefix);
    static QString deriveProtonPath(const QString &wineBinary);

signals:
    void gw2Found(const DetectedInstall &game);

private:
    QList<DetectedInstall> scanLutrisConfigs() const;
    QList<DetectedInstall> scanHeroicConfigs() const;
    QList<DetectedInstall> scanFaugusConfigs() const;
    QList<DetectedInstall> scanSteamConfigs() const;
    QString lutrisConfigDir() const;
};

#endif // INSTALLDETECTOR_H
