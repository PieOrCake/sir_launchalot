#include "ui/MainWindow.h"
#include "ui/AccountDialog.h"
#include "ui/AltSetupWizard.h"
#include "ui/SettingsDialog.h"
#include "ui/SetupWizard.h"
#include "core/OverlayManager.h"
#include "core/AccountManager.h"
#include "core/WineManager.h"
#include "core/LutrisIntegration.h"
#include "core/ProcessManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QMenu>
#include <QMenuBar>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // Create core managers
    m_overlayManager = new OverlayManager(this);
    m_accountManager = new AccountManager(this);
    m_wineManager = new WineManager(this);
    m_lutris = new LutrisIntegration(this);
    m_processManager = new ProcessManager(m_overlayManager, m_accountManager,
                                          m_wineManager, this);

    setupUi();
    setupMenuBar();

    // Connect process manager signals
    connect(m_processManager, &ProcessManager::instanceStarted,
            this, &MainWindow::onInstanceStarted);
    connect(m_processManager, &ProcessManager::instanceStopped,
            this, &MainWindow::onInstanceStopped);
    connect(m_processManager, &ProcessManager::instanceError,
            this, &MainWindow::onInstanceError);
    connect(m_processManager, &ProcessManager::instanceOutput,
            this, &MainWindow::onInstanceOutput);
    connect(m_processManager, &ProcessManager::setupComplete,
            this, &MainWindow::onSetupComplete);
    connect(m_processManager, &ProcessManager::patchDetected,
            this, [this]() {
        // Delay slightly so the instance stopped state is fully processed
        QTimer::singleShot(500, this, [this]() {
            auto reply = QMessageBox::question(this, "Game Patch Detected",
                "GW2 was patched during this session.\n\n"
                "Would you like to update alt accounts now?\n"
                "(This updates each alt's Local.dat so they can connect.)",
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::Yes) {
                onUpdateAlts();
            }
        });
    });

    // Load accounts
    m_accountManager->load();
    refreshAccountList();
    refreshExternalAppList();

    // First run: show setup wizard; otherwise load saved settings
    if (m_accountManager->accounts().isEmpty()) {
        QTimer::singleShot(100, this, &MainWindow::runSetupWizard);
    } else {
        // Restore persisted prefix/exe
        m_basePrefix = m_accountManager->basePrefix();
        m_gw2ExePath = m_accountManager->gw2ExePath();

        if (m_basePrefix.isEmpty() || m_gw2ExePath.isEmpty()) {
            // Settings lost — try auto-detect as fallback
            QTimer::singleShot(100, this, &MainWindow::detectGW2Installation);
        } else {
            appendLog(QString("Loaded config — Prefix: %1").arg(m_basePrefix));
            appendLog(QString("  Exe: %1").arg(m_gw2ExePath));
            // Restore persisted wine runner, or discover
            auto runners = m_wineManager->discoverRunners();
            QString savedRunner = m_accountManager->wineRunnerPath();
            bool matched = false;
            if (!savedRunner.isEmpty()) {
                for (const auto &r : runners) {
                    if (r.path == savedRunner) {
                        m_wineManager->setSelectedRunner(r);
                        appendLog(QString("  Wine runner: %1 (%2)").arg(r.name, r.version));
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && !runners.isEmpty()) {
                // Prefer a runner matching the install source
                QString preferredSource;
                if (m_basePrefix.contains("/Games/", Qt::CaseInsensitive) ||
                    m_basePrefix.contains("/lutris", Qt::CaseInsensitive)) {
                    preferredSource = "lutris";
                } else if (m_basePrefix.contains("/compatdata/", Qt::CaseInsensitive)) {
                    preferredSource = "proton";
                }
                if (!preferredSource.isEmpty()) {
                    for (const auto &r : runners) {
                        if (r.source == preferredSource) {
                            m_wineManager->setSelectedRunner(r);
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched) {
                    m_wineManager->setSelectedRunner(runners.first());
                }
                auto sel = m_wineManager->selectedRunner();
                m_accountManager->setWineRunnerPath(sel.path);
                appendLog(QString("  Wine runner: %1 (%2)")
                          .arg(sel.name, sel.version));
            }
            // Detect Lutris game ID for CLI-based launching
            int lutrisId = m_lutris->detectLutrisGameId(m_basePrefix);
            if (lutrisId > 0) {
                m_processManager->setLutrisGameId(lutrisId);
                appendLog(QString("  Lutris game ID: %1 (will launch via Lutris CLI)").arg(lutrisId));
            }

            refreshAccountList();
        }
    }

    setWindowTitle("Sir Launchalot");
    resize(320, 280);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_logWindow && event->type() == QEvent::Close) {
        m_logToggleBtn->setChecked(false);
        m_logToggleBtn->setText("Show Log");
    }
    if (obj == m_accountList->viewport() && event->type() == QEvent::Drop) {
        // Let Qt process the drop first, then read the new order
        QTimer::singleShot(0, this, [this]() {
            QStringList orderedIds;
            for (int i = 0; i < m_accountList->count(); ++i) {
                QString id = m_accountList->item(i)->data(Qt::UserRole).toString();
                if (!id.isEmpty()) orderedIds.append(id);
            }
            m_accountManager->reorderItems(orderedIds);
            refreshAccountList();
        });
    }
    return QMainWindow::eventFilter(obj, event);
}

MainWindow::~MainWindow()
{
    m_processManager->stopAll();
    delete m_logWindow;
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *outerLayout = new QHBoxLayout(central);
    outerLayout->setSpacing(0);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // Left side: accounts + toolbar + status
    auto *leftWidget = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setSpacing(4);
    leftLayout->setContentsMargins(8, 8, 8, 8);

    // Account list
    m_accountList = new QListWidget;
    m_accountList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_accountList->setStyleSheet("QListWidget::item:selected { background: transparent; }");
    m_accountList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_accountList->setDragDropMode(QAbstractItemView::InternalMove);
    m_accountList->setDefaultDropAction(Qt::MoveAction);
    connect(m_accountList, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onAccountContextMenu);
    connect(m_accountList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { onEditAccount(); });
    m_accountList->viewport()->installEventFilter(this);
    leftLayout->addWidget(m_accountList, 1);

    // Toolbar
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);

    m_addBtn = new QPushButton("+ Add Alt");
    toolbar->addWidget(m_addBtn);

    m_addAppBtn = new QPushButton("+ Add App");
    toolbar->addWidget(m_addAppBtn);
    toolbar->addStretch();

    m_logToggleBtn = new QPushButton("Show Log");
    m_logToggleBtn->setCheckable(true);
    m_logToggleBtn->setChecked(false);
    toolbar->addWidget(m_logToggleBtn);

    connect(m_addBtn, &QPushButton::clicked, this, &MainWindow::onAddAccount);
    connect(m_addAppBtn, &QPushButton::clicked, this, &MainWindow::onAddExternalApp);
    connect(m_logToggleBtn, &QPushButton::clicked, this, &MainWindow::onToggleLog);

    leftLayout->addLayout(toolbar);

    outerLayout->addWidget(leftWidget);

    // Log window (separate floating window)
    m_logWindow = new QWidget(nullptr, Qt::Window);
    m_logWindow->setWindowTitle("Sir Launchalot \u2014 Log");
    m_logWindow->resize(600, 350);
    m_logWindow->installEventFilter(this);
    auto *logLayout = new QVBoxLayout(m_logWindow);
    logLayout->setContentsMargins(8, 8, 8, 8);

    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setFont(QFont("Monospace", 8));
    logLayout->addWidget(m_logView);

    auto *logBtnLayout = new QHBoxLayout;
    auto *copyLogBtn = new QPushButton("Copy");
    auto *clearLogBtn = new QPushButton("Clear");
    copyLogBtn->setFixedWidth(60);
    clearLogBtn->setFixedWidth(60);
    logBtnLayout->addStretch();
    logBtnLayout->addWidget(copyLogBtn);
    logBtnLayout->addWidget(clearLogBtn);
    logLayout->addLayout(logBtnLayout);

    connect(copyLogBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_logView->toPlainText());
    });
    connect(clearLogBtn, &QPushButton::clicked, this, [this]() {
        m_logView->clear();
    });

    resize(320, 280);
    updateButtonStates();
}

void MainWindow::setupMenuBar()
{
    auto *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("Setup &Wizard...", this, &MainWindow::runSetupWizard);
    fileMenu->addAction("&Settings...", this, &MainWindow::onSettings);
    fileMenu->addAction("&Update Alts (after game patch)...", this, &MainWindow::onUpdateAlts);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", qApp, &QApplication::quit);

    auto *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About", this, [this]() {
        QMessageBox about(this);
        about.setWindowTitle("About Sir Launchalot");
        about.setIconPixmap(windowIcon().pixmap(64, 64));
        about.setText("<h2>Sir Launchalot v0.1.0</h2>");
        about.setInformativeText(
            "Guild Wars 2 multibox launcher for Linux.\n\n"
            "Features:\n"
            "  • Launch multiple GW2 accounts simultaneously\n"
            "  • Automatic Wine prefix cloning via rsync\n"
            "  • Per-account credentials and graphics settings\n"
            "  • Batch alt updates after game patches\n"
            "  • External app launcher integration\n"
            "  • Lutris and umu-run/Proton compatibility\n\n"
            "Requires: rsync, Wine or Proton");
        about.setMinimumWidth(500);
        about.exec();
    });
}

void MainWindow::refreshAccountList()
{
    m_accountList->clear();

    // Build lookup maps
    QMap<QString, AccountManager::Account> acctMap;
    for (const auto &acct : m_accountManager->accounts()) {
        acctMap[acct.id] = acct;
    }
    QMap<QString, AccountManager::ExternalApp> appMap;
    for (const auto &app : m_accountManager->externalApps()) {
        appMap[app.id] = app;
    }

    // Render in interleaved order
    QStringList order = m_accountManager->orderedItemIds();
    for (const auto &id : order) {
        if (acctMap.contains(id)) {
            const auto &acct = acctMap[id];
            auto *item = new QListWidgetItem(m_accountList);
            item->setData(Qt::UserRole, acct.id);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);

            auto *rowWidget = new QWidget;
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(6, 4, 6, 4);

            QString name = acct.displayName.isEmpty() ? acct.id : acct.displayName;
            auto *nameLabel = new QLabel(name);
            QFont nameFont = nameLabel->font();
            nameFont.setPointSize(nameFont.pointSize() + 1);
            nameLabel->setFont(nameFont);
            rowLayout->addWidget(nameLabel);

            auto state = m_processManager->instanceState(acct.id);
            bool isRunning = (state == ProcessManager::InstanceState::Running ||
                              state == ProcessManager::InstanceState::Starting);

            auto *statusLabel = new QLabel;
            statusLabel->setFont(QFont(statusLabel->font().family(), statusLabel->font().pointSize() - 1));

            if (isRunning) {
                statusLabel->setText(state == ProcessManager::InstanceState::Starting
                                     ? "\u23F3 Starting..." : "\u25B6 Running");
                statusLabel->setStyleSheet("color: #66bb6a;");
            } else if (state == ProcessManager::InstanceState::Stopping) {
                statusLabel->setText("\u23F9 Stopping...");
                statusLabel->setStyleSheet("color: #ffa726;");
            } else if (acct.isMain) {
                statusLabel->setText("\u2605 Main");
                statusLabel->setStyleSheet("color: #4fc3f7;");
            } else {
                QString savedLocalDat = m_overlayManager->dataDir() + "/" + acct.id + "/saved/Local.dat";
                if (QFile::exists(savedLocalDat)) {
                    statusLabel->setText("Ready");
                    statusLabel->setStyleSheet("color: #9e9e9e;");
                } else {
                    statusLabel->setText("Needs Setup");
                    statusLabel->setStyleSheet("color: #ef5350;");
                }
            }
            rowLayout->addWidget(statusLabel);
            rowLayout->addStretch();

            QString accountId = acct.id;
            bool needsSetup = !acct.isMain &&
                !QFile::exists(m_overlayManager->dataDir() + "/" + acct.id + "/saved/Local.dat");

            auto *actionBtn = new QPushButton(isRunning ? "Stop" : "Launch");
            actionBtn->setFixedWidth(70);
            if (isRunning) {
                actionBtn->setStyleSheet("background-color: #c62828; color: white;");
            } else if (needsSetup) {
                actionBtn->setEnabled(false);
                actionBtn->setToolTip("Run Setup first (right-click \u2192 Setup Account)");
            } else {
                actionBtn->setStyleSheet("background-color: #2e7d32; color: white;");
                actionBtn->setEnabled(!m_basePrefix.isEmpty());
            }

            connect(actionBtn, &QPushButton::clicked, this, [this, accountId, isRunning]() {
                if (isRunning) {
                    m_processManager->stopAccount(accountId);
                } else {
                    appendLog(QString("Launching: %1").arg(accountId));
                    m_processManager->launchAccount(accountId, m_basePrefix, m_gw2ExePath);
                }
                refreshAccountList();
            });
            rowLayout->addWidget(actionBtn);

            item->setSizeHint(rowWidget->sizeHint());
            m_accountList->setItemWidget(item, rowWidget);

        } else if (appMap.contains(id)) {
            const auto &app = appMap[id];
            auto *item = new QListWidgetItem(m_accountList);
            item->setData(Qt::UserRole, app.id);
            item->setData(Qt::UserRole + 1, "externalApp");
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);

            auto *rowWidget = new QWidget;
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(6, 4, 6, 4);

            auto *nameLabel = new QLabel(app.name);
            QFont nameFont = nameLabel->font();
            nameFont.setPointSize(nameFont.pointSize() + 1);
            nameLabel->setFont(nameFont);
            rowLayout->addWidget(nameLabel);
            rowLayout->addStretch();

            QString appId = app.id;
            auto *launchBtn = new QPushButton("Launch");
            launchBtn->setFixedWidth(70);
            launchBtn->setStyleSheet("background-color: #1565c0; color: white;");
            connect(launchBtn, &QPushButton::clicked, this, [this, appId]() {
                onLaunchExternalApp(appId);
            });
            rowLayout->addWidget(launchBtn);

            item->setSizeHint(rowWidget->sizeHint());
            m_accountList->setItemWidget(item, rowWidget);
        }
    }

    updateButtonStates();
}

void MainWindow::updateButtonStates()
{
    // Need a base prefix and at least one main account before adding alts
    bool hasMain = false;
    for (const auto &acct : m_accountManager->accounts()) {
        if (acct.isMain) { hasMain = true; break; }
    }
    m_addBtn->setEnabled(!m_basePrefix.isEmpty() && hasMain);
}

void MainWindow::appendLog(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    m_logView->append(QString("[%1] %2").arg(timestamp, message));
}

void MainWindow::detectGW2Installation()
{
    appendLog("Searching for GW2 installations...");

    auto installs = m_lutris->discoverGW2Installs();
    if (installs.isEmpty()) {
        appendLog("No GW2 installation found. Please configure in Settings.");
        return;
    }

    auto &gw2 = installs.first();
    m_basePrefix = gw2.winePrefix;
    m_gw2ExePath = gw2.exePath;
    m_accountManager->setBasePrefix(m_basePrefix);
    m_accountManager->setGw2ExePath(m_gw2ExePath);

    appendLog(QString("Found GW2: %1").arg(gw2.name));
    appendLog(QString("  Prefix: %1").arg(m_basePrefix));
    appendLog(QString("  Exe: %1").arg(m_gw2ExePath));

    // Try to auto-select a wine runner
    auto runners = m_wineManager->discoverRunners();
    if (!runners.isEmpty()) {
        // Prefer the runner matching the Lutris config, otherwise first available
        if (!gw2.wineBinary.isEmpty()) {
            for (const auto &r : runners) {
                if (r.path == gw2.wineBinary) {
                    m_wineManager->setSelectedRunner(r);
                    appendLog(QString("  Wine runner: %1 (%2)")
                              .arg(r.name, r.version));
                    break;
                }
            }
        }
        if (m_wineManager->selectedRunner().path.isEmpty()) {
            m_wineManager->setSelectedRunner(runners.first());
            appendLog(QString("  Wine runner: %1 (%2)")
                      .arg(runners.first().name, runners.first().version));
        }
    } else {
        appendLog("WARNING: No Wine runners found.");
    }

    // Detect Lutris game ID for CLI-based launching
    int lutrisId = m_lutris->detectLutrisGameId(m_basePrefix);
    if (lutrisId > 0) {
        m_processManager->setLutrisGameId(lutrisId);
        appendLog(QString("  Lutris game ID: %1 (will launch via Lutris CLI)").arg(lutrisId));
    }

    updateButtonStates();
}

void MainWindow::runSetupWizard()
{
    if (!m_accountManager->accounts().isEmpty()) {
        QMessageBox::information(this, "Setup Wizard",
            "The setup wizard is only available when no accounts exist.\n\n"
            "Use File \u2192 Settings to change configuration, or remove all accounts first.");
        return;
    }

    SetupWizard wizard(m_lutris, m_wineManager, this);
    if (wizard.exec() == QDialog::Accepted) {
        // Set and persist the base prefix and exe path
        m_basePrefix = wizard.winePrefix();
        m_gw2ExePath = wizard.gw2ExePath();
        m_accountManager->setBasePrefix(m_basePrefix);
        m_accountManager->setGw2ExePath(m_gw2ExePath);

        // Create the main account (or update if re-running wizard)
        AccountManager::Account mainAcct;
        mainAcct.id = "main";
        mainAcct.displayName = wizard.accountName();
        mainAcct.isMain = true;
        m_accountManager->addAccount(mainAcct);

        appendLog(QString("Main account created: %1").arg(mainAcct.displayName));
        appendLog(QString("  Prefix: %1").arg(m_basePrefix));
        appendLog(QString("  Exe: %1").arg(m_gw2ExePath));

        // Discover and select a Wine runner
        auto runners = m_wineManager->discoverRunners();
        QString detectedBinary = wizard.wineBinary();
        bool matched = false;

        // First: try using the binary detected from the Lutris config
        if (!detectedBinary.isEmpty() && QFile::exists(detectedBinary)) {
            for (const auto &r : runners) {
                if (r.path == detectedBinary) {
                    m_wineManager->setSelectedRunner(r);
                    matched = true;
                    break;
                }
            }
            // If the Lutris binary isn't in our runner list, use it directly
            if (!matched) {
                WineManager::WineRunner r;
                r.name = QFileInfo(detectedBinary).dir().dirName();
                r.path = detectedBinary;
                r.version = WineManager::wineVersion(detectedBinary);
                r.source = "lutris";
                m_wineManager->setSelectedRunner(r);
                matched = true;
            }
        }

        // If no specific binary matched, prefer a Lutris runner
        if (!matched && !runners.isEmpty()) {
            for (const auto &r : runners) {
                if (r.source == "lutris") {
                    m_wineManager->setSelectedRunner(r);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                m_wineManager->setSelectedRunner(runners.first());
            }
        }

        auto selected = m_wineManager->selectedRunner();
        if (!selected.path.isEmpty()) {
            m_accountManager->setWineRunnerPath(selected.path);
            appendLog(QString("  Wine runner: %1 (%2)")
                      .arg(selected.name, selected.version));
        } else {
            appendLog("WARNING: No Wine runners found.");
        }

        refreshAccountList();
        updateButtonStates();
    } else {
        appendLog("Setup wizard cancelled. Use File > Settings to configure manually.");
    }
}

void MainWindow::onAddAccount()
{
    if (m_basePrefix.isEmpty()) {
        QMessageBox::warning(this, "Not Ready",
            "Please run the Setup Wizard first (File menu) to detect your GW2 installation.");
        return;
    }

    if (!m_processManager->runningAccounts().isEmpty()) {
        QMessageBox::warning(this, "Accounts Running",
            "Stop all running accounts before adding a new alt.\n\n"
            "Setup mode needs to launch GW2 via Lutris to capture credentials.");
        return;
    }

    m_wizardActive = true;
    AltSetupWizard wiz(m_accountManager, m_processManager, m_overlayManager, this);
    wiz.exec();
    m_wizardActive = false;

    if (wiz.setupSucceeded()) {
        auto acct = wiz.createdAccount();
        appendLog(QString("Added and set up account: %1").arg(acct.displayName));
    }
    refreshAccountList();
    updateButtonStates();
}

void MainWindow::onEditAccount()
{
    auto items = m_accountList->selectedItems();
    if (items.isEmpty()) return;

    QString id = items.first()->data(Qt::UserRole).toString();
    auto acct = m_accountManager->account(id);

    AccountDialog dlg(this);
    dlg.setAccount(acct);
    if (dlg.exec() == QDialog::Accepted) {
        auto updated = dlg.account();
        updated.id = acct.id; // preserve the original ID
        if (m_accountManager->updateAccount(updated)) {
            appendLog(QString("Updated account: %1").arg(updated.displayName));
            refreshAccountList();
        }
    }
}

void MainWindow::onRemoveAccount()
{
    auto items = m_accountList->selectedItems();
    if (items.isEmpty()) return;

    QString id = items.first()->data(Qt::UserRole).toString();
    auto acct = m_accountManager->account(id);

    if (acct.isMain && m_accountManager->accounts().size() > 1) {
        QMessageBox::warning(this, "Cannot Remove",
            "Remove all alt accounts before removing the main account.");
        return;
    }

    auto result = QMessageBox::question(this, "Remove Account",
        QString("Remove account '%1'?\n\n"
                "This will delete the overlay data for this account.")
            .arg(acct.displayName));

    if (result == QMessageBox::Yes) {
        m_processManager->stopAccount(id);
        m_accountManager->removeAccount(id);
        appendLog(QString("Removed account: %1").arg(acct.displayName));
        refreshAccountList();
    }
}

void MainWindow::onSetupAccount()
{
    auto items = m_accountList->selectedItems();
    if (items.isEmpty()) return;

    QString id = items.first()->data(Qt::UserRole).toString();
    auto acct = m_accountManager->account(id);

    if (acct.isMain) {
        QMessageBox::information(this, "Setup Account",
            "The main account doesn't need setup — it uses the base prefix directly.");
        return;
    }

    if (!m_processManager->runningAccounts().isEmpty()) {
        QMessageBox::warning(this, "Accounts Running",
            "Stop all running accounts before running setup.");
        return;
    }

    auto result = QMessageBox::information(this, "Setup Account",
        QString("This will launch GW2 to capture credentials for '%1'.\n\n"
                "Steps:\n"
                "1. Enter your alt account credentials on the GW2 launcher\n"
                "2. Check 'Remember Account Name' and 'Remember Password'\n"
                "3. Log in and reach character select\n"
                "4. Close the game\n\n"
                "Your main account's credentials will be backed up and restored automatically.\n\n"
                "Continue?").arg(acct.displayName),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);

    if (result != QMessageBox::Ok) return;

    appendLog(QString("Starting setup for: %1").arg(acct.displayName));
    m_processManager->setupAccount(id);
    refreshAccountList();
}

void MainWindow::onSetupComplete(const QString &accountId, bool success)
{
    auto acct = m_accountManager->account(accountId);
    if (success) {
        appendLog(QString("Setup complete for '%1' \u2014 credentials captured!")
            .arg(acct.displayName));
    } else {
        appendLog(QString("Setup for '%1' did not capture credentials.")
            .arg(acct.displayName));
    }

    // Only show popups when setup was triggered outside the wizard (via context menu)
    if (!m_wizardActive) {
        if (success) {
            QMessageBox::information(this, "Setup Complete",
                QString("Credentials captured for '%1'!\n\n"
                        "You can now launch this account and it will auto-login.")
                    .arg(acct.displayName));
        } else {
            QMessageBox::warning(this, "Setup Incomplete",
                QString("Credentials were not captured for '%1'.\n\n"
                        "Make sure you entered alt credentials with 'Remember' checked, "
                        "logged in, and closed the game normally.")
                    .arg(acct.displayName));
        }
    }
    refreshAccountList();
}

void MainWindow::onSettings()
{
    SettingsDialog dlg(m_wineManager, this);
    dlg.setBasePrefix(m_basePrefix);
    dlg.setGw2ExePath(m_gw2ExePath);

    if (dlg.exec() == QDialog::Accepted) {
        m_basePrefix = dlg.basePrefix();
        m_gw2ExePath = dlg.gw2ExePath();
        m_accountManager->setBasePrefix(m_basePrefix);
        m_accountManager->setGw2ExePath(m_gw2ExePath);
        appendLog("Settings updated.");
        refreshAccountList();
    }
}

void MainWindow::onToggleLog()
{
    bool show = m_logToggleBtn->isChecked();

    if (show) {
        m_logWindow->show();
        m_logWindow->raise();
        m_logWindow->activateWindow();
    } else {
        m_logWindow->hide();
    }

    m_logToggleBtn->setText(show ? "Hide Log" : "Show Log");
}

void MainWindow::onAccountContextMenu(const QPoint &pos)
{
    auto *item = m_accountList->itemAt(pos);
    if (!item) return;

    QString id = item->data(Qt::UserRole).toString();
    QString itemType = item->data(Qt::UserRole + 1).toString();

    QMenu menu(this);

    if (itemType == "externalApp") {
        auto *editAction = menu.addAction("Edit");
        connect(editAction, &QAction::triggered, this, [this, id]() {
            onEditExternalApp(id);
        });
        auto *removeAction = menu.addAction("Remove");
        connect(removeAction, &QAction::triggered, this, [this, id]() {
            onRemoveExternalApp(id);
        });
    } else {
        auto acct = m_accountManager->account(id);

        if (!acct.isMain) {
            bool hasRunning = !m_processManager->runningAccounts().isEmpty();
            auto *setupAction = menu.addAction("Setup Account (re-capture credentials)");
            setupAction->setEnabled(!hasRunning);
            connect(setupAction, &QAction::triggered, this, [this, item]() {
                m_accountList->setCurrentItem(item);
                onSetupAccount();
            });
            menu.addSeparator();
        }

        auto *editAction = menu.addAction("Edit");
        connect(editAction, &QAction::triggered, this, [this, item]() {
            m_accountList->setCurrentItem(item);
            onEditAccount();
        });

        auto *removeAction = menu.addAction("Remove");
        connect(removeAction, &QAction::triggered, this, [this, item]() {
            m_accountList->setCurrentItem(item);
            onRemoveAccount();
        });
    }

    menu.exec(m_accountList->viewport()->mapToGlobal(pos));
}

void MainWindow::refreshExternalAppList()
{
    refreshAccountList();
}

void MainWindow::onAddExternalApp()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Add External App");
    dlg.setMinimumWidth(450);
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    auto *nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("e.g. Discord, TeamSpeak, ...");
    form->addRow("App name:", nameEdit);
    auto *cmdEdit = new QLineEdit;
    cmdEdit->setPlaceholderText("e.g. discord, /usr/bin/teamspeak");
    form->addRow("Command:", cmdEdit);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;
    if (nameEdit->text().trimmed().isEmpty() || cmdEdit->text().trimmed().isEmpty()) return;

    AccountManager::ExternalApp app;
    app.id = QString("app_%1").arg(QDateTime::currentMSecsSinceEpoch());
    app.name = nameEdit->text().trimmed();
    app.command = cmdEdit->text().trimmed();
    m_accountManager->addExternalApp(app);
    appendLog(QString("Added external app: %1").arg(app.name));
    refreshExternalAppList();
}

void MainWindow::onEditExternalApp(const QString &appId)
{
    auto apps = m_accountManager->externalApps();
    for (const auto &app : apps) {
        if (app.id == appId) {
            QDialog dlg(this);
            dlg.setWindowTitle("Edit External App");
            dlg.setMinimumWidth(450);
            auto *layout = new QVBoxLayout(&dlg);
            auto *form = new QFormLayout;
            auto *nameEdit = new QLineEdit(app.name);
            form->addRow("App name:", nameEdit);
            auto *cmdEdit = new QLineEdit(app.command);
            form->addRow("Command:", cmdEdit);
            layout->addLayout(form);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            layout->addWidget(buttons);

            if (dlg.exec() != QDialog::Accepted) return;
            if (nameEdit->text().trimmed().isEmpty() || cmdEdit->text().trimmed().isEmpty()) return;

            AccountManager::ExternalApp updated;
            updated.id = app.id;
            updated.name = nameEdit->text().trimmed();
            updated.command = cmdEdit->text().trimmed();
            m_accountManager->updateExternalApp(updated);
            appendLog(QString("Updated external app: %1").arg(updated.name));
            refreshAccountList();
            return;
        }
    }
}

void MainWindow::onRemoveExternalApp(const QString &appId)
{
    m_accountManager->removeExternalApp(appId);
    appendLog(QString("Removed external app: %1").arg(appId));
    refreshExternalAppList();
}

void MainWindow::onLaunchExternalApp(const QString &appId)
{
    auto apps = m_accountManager->externalApps();
    for (const auto &app : apps) {
        if (app.id == appId) {
            appendLog(QString("Launching: %1 (%2)").arg(app.name, app.command));
            QStringList args = QProcess::splitCommand(app.command);
            if (args.isEmpty()) {
                appendLog(QString("ERROR: Empty command for '%1'").arg(app.name));
                return;
            }
            // Clear desktop file hints so KDE doesn't show Sir Launchalot's
            // icon for the launched app, and use setsid for session detach
            QString program = args.takeFirst();
            QStringList wrappedArgs;
            wrappedArgs << "-u" << "GIO_LAUNCHED_DESKTOP_FILE"
                        << "-u" << "GIO_LAUNCHED_DESKTOP_FILE_PID"
                        << "-u" << "DESKTOP_STARTUP_ID"
                        << "-u" << "XDG_ACTIVATION_TOKEN"
                        << "setsid" << "--wait" << program << args;
            QProcess::startDetached("env", wrappedArgs);
            return;
        }
    }
}

void MainWindow::onUpdateAlts()
{
    if (m_basePrefix.isEmpty() || m_gw2ExePath.isEmpty()) {
        QMessageBox::warning(this, "Not Ready",
            "Please run the Setup Wizard first to configure your GW2 installation.");
        return;
    }

    if (!m_processManager->runningAccounts().isEmpty()) {
        QMessageBox::warning(this, "Accounts Running",
            "Stop all running accounts before updating alts.");
        return;
    }

    // Collect all alt account IDs that have saved credentials
    QStringList altIds;
    for (const auto &acct : m_accountManager->accounts()) {
        if (!acct.isMain) {
            QString savedLocalDat = m_overlayManager->dataDir() + "/" + acct.id + "/saved/Local.dat";
            if (QFile::exists(savedLocalDat)) {
                altIds.append(acct.id);
            } else {
                appendLog(QString("Skipping %1 — no saved credentials").arg(
                    acct.displayName.isEmpty() ? acct.id : acct.displayName));
            }
        }
    }

    if (altIds.isEmpty()) {
        QMessageBox::information(this, "No Alts",
            "No alt accounts with saved credentials found.\n"
            "Use 'Setup Account' on each alt first.");
        return;
    }

    auto reply = QMessageBox::question(this, "Update Alts",
        QString("This will update Local.dat for %1 alt account(s) by launching "
                "GW2 with -image in the base prefix.\n\n"
                "Make sure the main account is already updated.\n\n"
                "Continue?").arg(altIds.size()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    appendLog(QString("=== Updating %1 alt(s) ===").arg(altIds.size()));

    connect(m_processManager, &ProcessManager::updateComplete,
            this, [this](const QString &accountId, bool success) {
        auto acct = m_accountManager->account(accountId);
        QString name = acct.displayName.isEmpty() ? accountId : acct.displayName;
        appendLog(QString("Update %1: %2").arg(name, success ? "OK" : "FAILED"));
    }, Qt::UniqueConnection);

    connect(m_processManager, &ProcessManager::allUpdatesComplete,
            this, [this]() {
        appendLog("=== All alt updates complete ===");
        QMessageBox::information(this, "Update Complete",
            "All alt accounts have been updated.\n"
            "You can now launch them normally.");
    }, Qt::UniqueConnection);

    m_processManager->updateAllAlts(altIds, m_basePrefix, m_gw2ExePath);
}

void MainWindow::onInstanceStarted(const QString &accountId)
{
    appendLog(QString("Instance started: %1").arg(accountId));
    refreshAccountList();
}

void MainWindow::onInstanceStopped(const QString &accountId)
{
    appendLog(QString("Instance stopped: %1").arg(accountId));
    refreshAccountList();
}

void MainWindow::onInstanceError(const QString &accountId, const QString &error)
{
    appendLog(QString("ERROR [%1]: %2").arg(accountId, error));
    refreshAccountList();
}

void MainWindow::onInstanceOutput(const QString &accountId, const QString &output)
{
    appendLog(QString("[%1] %2").arg(accountId, output.trimmed()));
}
