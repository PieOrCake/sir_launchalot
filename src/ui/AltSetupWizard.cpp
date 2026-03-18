#include "ui/AltSetupWizard.h"
#include "core/ProcessManager.h"
#include "core/OverlayManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QUuid>

// ─── AltSetupWizard ─────────────────────────────────────────────────────────

AltSetupWizard::AltSetupWizard(AccountManager *accounts,
                               ProcessManager *processManager,
                               OverlayManager *overlay,
                               QWidget *parent)
    : QWizard(parent)
    , m_accounts(accounts)
    , m_processManager(processManager)
    , m_overlay(overlay)
{
    setWindowTitle("Add Alt Account");
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoCancelButtonOnLastPage, true);

    setPage(Page_Name, new NamePage(this));
    setPage(Page_Setup, new SetupPage(processManager, accounts, overlay, this));

    setStartId(Page_Name);
    resize(550, 400);
}

// ─── Page 1: Name ───────────────────────────────────────────────────────────

NamePage::NamePage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Add Alt Account");
    setSubTitle("Choose a name, then you'll log in once to save this alt's credentials.");

    auto *layout = new QVBoxLayout(this);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("e.g. Alt1, PvP Account, Crafter, ...");
    layout->addWidget(m_nameEdit);
    layout->addStretch();

    registerField("accountName*", m_nameEdit);
}

bool NamePage::isComplete() const
{
    return !m_nameEdit->text().trimmed().isEmpty();
}

// ─── Page 2: Setup (launch + capture) ───────────────────────────────────────

SetupPage::SetupPage(ProcessManager *pm, AccountManager *accounts,
                     OverlayManager *overlay, QWidget *parent)
    : QWizardPage(parent)
    , m_processManager(pm)
    , m_accounts(accounts)
    , m_overlay(overlay)
{
    setTitle("Log In as Alt");
    setSubTitle("Launch GW2 and enter your alt credentials with 'Remember' checked. "
                "Log in, then close the game.");

    auto *layout = new QVBoxLayout(this);

    m_launchBtn = new QPushButton("Launch GW2");
    m_launchBtn->setMinimumHeight(36);
    QFont btnFont = m_launchBtn->font();
    btnFont.setBold(true);
    m_launchBtn->setFont(btnFont);
    layout->addWidget(m_launchBtn);

    m_statusLabel = new QLabel("Click above to start.");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setFont(QFont("Monospace", 8));
    m_logView->setMaximumHeight(120);
    layout->addWidget(m_logView);

    connect(m_launchBtn, &QPushButton::clicked, this, &SetupPage::onLaunchClicked);
    connect(m_processManager, &ProcessManager::setupComplete,
            this, &SetupPage::onSetupComplete);
    connect(m_processManager, &ProcessManager::instanceOutput,
            this, &SetupPage::onInstanceOutput);
    connect(m_processManager, &ProcessManager::instanceStopped,
            this, &SetupPage::onInstanceStopped);
}

void SetupPage::initializePage()
{
    m_setupDone = false;
    m_setupSuccess = false;
    m_launched = false;
    m_logView->clear();
    m_launchBtn->setEnabled(true);
    m_statusLabel->setText("Click above to start.");

    // Create the account now so setupAccount can find it
    QString name = field("accountName").toString().trimmed();
    m_accountId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

    AccountManager::Account acct;
    acct.id = m_accountId;
    acct.displayName = name;
    acct.isMain = false;
    m_accounts->addAccount(acct);
}

void SetupPage::onLaunchClicked()
{
    m_launchBtn->setEnabled(false);
    m_launched = true;
    m_statusLabel->setText("Waiting for game to close...");

    if (!m_processManager->setupAccount(m_accountId)) {
        m_statusLabel->setText("Failed to launch. Check the log below.");
        m_launchBtn->setEnabled(true);
        m_launched = false;
    }
}

void SetupPage::onSetupComplete(const QString &accountId, bool success)
{
    if (accountId != m_accountId) return;

    m_setupDone = true;
    m_setupSuccess = success;

    // Store result on the wizard
    auto *wiz = qobject_cast<AltSetupWizard *>(wizard());
    if (wiz) {
        wiz->m_setupSucceeded = success;
        wiz->m_account = m_accounts->account(m_accountId);
    }

    if (success) {
        m_statusLabel->setText("<b style='color: #66bb6a; font-size: 12pt;'>"
                               "\u2714 Credentials captured!</b><br>Click 'Finish' to close.");
    } else {
        m_statusLabel->setText("<b style='color: #ef5350; font-size: 12pt;'>"
                               "\u2718 Credentials were not captured.</b><br>"
                               "Make sure you used 'Remember' and logged in. You can try again.");
        m_launchBtn->setEnabled(true);
        m_launched = false;
    }

    emit completeChanged();
}

void SetupPage::onInstanceOutput(const QString &accountId, const QString &output)
{
    if (accountId != m_accountId) return;
    m_logView->append(output.trimmed());
}

void SetupPage::onInstanceStopped(const QString &accountId)
{
    if (accountId != m_accountId) return;
    if (!m_setupDone) {
        m_statusLabel->setText("Game exited. Processing...");
    }
}

bool SetupPage::isComplete() const
{
    return m_setupDone;
}

int SetupPage::nextId() const
{
    return -1;
}

bool SetupPage::validatePage()
{
    if (!m_setupSuccess) {
        // Remove the account if setup failed — user can try the wizard again
        m_accounts->removeAccount(m_accountId);
    }
    return true;
}

// ─── Page 3: Result ─────────────────────────────────────────────────────────

ResultPage::ResultPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Setup Complete");

    auto *layout = new QVBoxLayout(this);

    m_resultLabel = new QLabel;
    m_resultLabel->setWordWrap(true);
    QFont f = m_resultLabel->font();
    f.setPointSize(f.pointSize() + 2);
    f.setBold(true);
    m_resultLabel->setFont(f);
    layout->addWidget(m_resultLabel);

    m_detailLabel = new QLabel;
    m_detailLabel->setWordWrap(true);
    layout->addWidget(m_detailLabel);

    layout->addStretch();
}

void ResultPage::initializePage()
{
    auto *wiz = qobject_cast<AltSetupWizard *>(wizard());
    if (!wiz) return;

    if (wiz->setupSucceeded()) {
        setTitle("Done");
        m_resultLabel->setText(QString("'%1' is ready to launch!")
            .arg(wiz->createdAccount().displayName));
        m_detailLabel->setText("It will auto-login with the saved credentials.");
    } else {
        setTitle("Setup Incomplete");
        m_resultLabel->setText("Credentials were not captured.");
        m_detailLabel->setText(
            "Make sure you entered alt credentials with 'Remember' checked, "
            "logged in, and closed the game normally.\n\n"
            "The account was not created. Run this wizard again to retry.");
    }
}
