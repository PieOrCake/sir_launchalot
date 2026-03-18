#ifndef LUTRISINTEGRATION_H
#define LUTRISINTEGRATION_H

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>

class LutrisIntegration : public QObject
{
    Q_OBJECT

public:
    explicit LutrisIntegration(QObject *parent = nullptr);

    struct LutrisGame {
        QString name;
        QString slug;
        QString runner;         // e.g. "wine"
        QString winePrefix;     // WINEPREFIX path
        QString wineBinary;     // path to wine binary used
        QString exePath;        // path to Gw2-64.exe
        QMap<QString, QString> envVars;
        int lutrisGameId = -1;  // numeric ID from `lutris -lo -j`
    };

    bool isLutrisInstalled() const;
    QList<LutrisGame> discoverGW2Installs() const;
    LutrisGame parseGameConfig(const QString &configPath) const;

    static QString gw2ExeInPrefix(const QString &winePrefix);
    int detectLutrisGameId(const QString &winePrefix) const;

signals:
    void gw2Found(const LutrisGame &game);

private:
    QList<LutrisGame> scanLutrisConfigs() const;
    QString lutrisConfigDir() const;
};

#endif // LUTRISINTEGRATION_H
