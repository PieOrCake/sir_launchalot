#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QTextEdit>
#include <QSplitter>
#include <QProcess>

class OverlayManager;
class AccountManager;
class WineManager;
class InstallDetector;
class ProcessManager;
class Gw2ApiClient;
class UpdateChecker;
class QSystemTrayIcon;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(bool devMode = false, QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onAddAccount();
    void onEditAccount();
    void onRemoveAccount();
    void onSetupAccount();
    void onSetupComplete(const QString &accountId, bool success);
    void onSettings();
    void onToggleLog();
    void onAccountContextMenu(const QPoint &pos);
    void onInstanceStarted(const QString &accountId);
    void onInstanceStopped(const QString &accountId);
    void onInstanceError(const QString &accountId, const QString &error);
    void onInstanceOutput(const QString &accountId, const QString &output);
    void onAddExternalApp();
    void onEditExternalApp(const QString &appId);
    void onRemoveExternalApp(const QString &appId);
    void onLaunchExternalApp(const QString &appId);
    void onUpdateAlts();

private:
    void setupUi();
    void setupMenuBar();
    void refreshAccountList();
    void refreshExternalAppList();
    void updateButtonStates();
    void appendLog(const QString &message);
    void detectGW2Installation();
    void runSetupWizard();

    void fetchApiData();
    void addSteamAlt();
    void launchSteamAccount(const QString &accountId);

    // Core managers
    OverlayManager *m_overlayManager;
    AccountManager *m_accountManager;
    WineManager *m_wineManager;
    InstallDetector *m_detector;
    ProcessManager *m_processManager;
    Gw2ApiClient *m_apiClient;
    UpdateChecker *m_updateChecker;
    QSystemTrayIcon *m_trayIcon;

    // UI elements
    QListWidget *m_accountList;
    QAction *m_addAltAction;
    QWidget *m_logWindow;
    QTextEdit *m_logView;
    QLabel *m_statusLabel;
    QTimer *m_apiRefreshTimer;

    // State
    QString m_basePrefix;
    QString m_gw2ExePath;
    bool m_wizardActive = false;
    bool m_devMode = false;
};

#endif // MAINWINDOW_H
