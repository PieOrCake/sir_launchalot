#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

class OverlayManager;
class AccountManager;
class WineManager;

class ProcessManager : public QObject
{
    Q_OBJECT

public:
    explicit ProcessManager(OverlayManager *overlay,
                            AccountManager *accounts,
                            WineManager *wine,
                            QObject *parent = nullptr);
    ~ProcessManager();

    enum class InstanceState {
        Stopped,
        Starting,
        Running,
        Stopping
    };

    struct InstanceInfo {
        QString accountId;
        InstanceState state = InstanceState::Stopped;
        qint64 pid = 0;
        QProcess *process = nullptr;
        QElapsedTimer launchTimer;  // tracks time since launch for grace period
    };

    bool launchAccount(const QString &accountId, const QString &basePrefix,
                       const QString &exePath);
    bool setupAccount(const QString &accountId);
    bool updateAlt(const QString &accountId, const QString &basePrefix,
                   const QString &exePath);
    void updateAllAlts(const QStringList &altIds, const QString &basePrefix,
                       const QString &exePath);
    bool stopAccount(const QString &accountId);
    void stopAll();

    InstanceState instanceState(const QString &accountId) const;
    QStringList runningAccounts() const;

    void setLaunchDelay(int msec);
    int launchDelay() const;

    void setLutrisGameId(int id);
    int lutrisGameId() const;

    QProcessEnvironment buildEnvironment(const QString &accountId,
                                          const QString &mergedPrefix) const;

signals:
    void instanceStarted(const QString &accountId);
    void instanceStopped(const QString &accountId);
    void instanceError(const QString &accountId, const QString &error);
    void instanceOutput(const QString &accountId, const QString &output);
    void setupComplete(const QString &accountId, bool success);
    void updateComplete(const QString &accountId, bool success);
    void allUpdatesComplete();
    void patchDetected();

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void pollDetachedInstances();
    void captureLutrisEnvironment();

private:
    QStringList buildLaunchArgs(const QString &exePath,
                                 const QString &accountId) const;
    void saveCapturedEnv() const;
    void loadCapturedEnv();
    QString envFilePath() const;

    OverlayManager *m_overlay;
    AccountManager *m_accounts;
    WineManager *m_wine;
    QMap<QString, InstanceInfo> m_instances;
    int m_launchDelay = 5000; // ms between sequential launches
    int m_lutrisGameId = -1;
    QString m_basePrefix;  // for polling wineserver
    QTimer *m_pollTimer = nullptr;
    QProcessEnvironment m_capturedEnv;  // env captured from Lutris Wine process
    bool m_envCaptured = false;

    // Setup Account mode: captures alt's Local.dat from base prefix
    QString m_setupAccountId;       // non-empty = setup mode active
    QString m_localDatPath;         // path to Local.dat in base prefix
    QString m_localDatBackupPath;   // backup of main's Local.dat

    // Update Alt mode: updates alt's Local.dat via -image in base prefix
    QString m_updateAccountId;      // non-empty = update mode active
    QString m_updateLocalDatPath;   // path to Local.dat in base prefix
    QString m_updateBackupPath;     // backup of main's Local.dat
    QString m_updateSavedDir;       // alt's saved dir for Local.dat
    QStringList m_updateQueue;      // remaining alts to update
    QString m_updateBasePrefix;     // stored for queue processing
    QString m_updateExePath;        // stored for queue processing

    // Patch detection: track Gw2.dat modification time across main launch
    QDateTime m_gw2DatMtimeBefore;
    QString m_gw2DatPath;           // path to Gw2.dat (derived from exePath)
};

#endif // PROCESSMANAGER_H
