#ifndef ALTSETUPWIZARD_H
#define ALTSETUPWIZARD_H

#include <QWizard>
#include <QWizardPage>
#include <QLineEdit>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>

#include "core/AccountManager.h"

class ProcessManager;
class OverlayManager;

class AltSetupWizard : public QWizard
{
    Q_OBJECT

public:
    explicit AltSetupWizard(AccountManager *accounts,
                            ProcessManager *processManager,
                            OverlayManager *overlay,
                            QWidget *parent = nullptr);

    AccountManager::Account createdAccount() const { return m_account; }
    bool setupSucceeded() const { return m_setupSucceeded; }

    enum { Page_Name, Page_Setup, Page_Result };

private:
    friend class SetupPage;

    AccountManager *m_accounts;
    ProcessManager *m_processManager;
    OverlayManager *m_overlay;
    AccountManager::Account m_account;
    bool m_setupSucceeded = false;
};

// Page 1: Name the account
class NamePage : public QWizardPage
{
    Q_OBJECT
public:
    explicit NamePage(QWidget *parent = nullptr);
    bool isComplete() const override;

private:
    QLineEdit *m_nameEdit;
};

// Page 2: Launch GW2 and capture credentials
class SetupPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit SetupPage(ProcessManager *pm, AccountManager *accounts,
                       OverlayManager *overlay, QWidget *parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    int nextId() const override;
    bool validatePage() override;

private slots:
    void onLaunchClicked();
    void onSetupComplete(const QString &accountId, bool success);
    void onInstanceOutput(const QString &accountId, const QString &output);
    void onInstanceStopped(const QString &accountId);

private:
    ProcessManager *m_processManager;
    AccountManager *m_accounts;
    OverlayManager *m_overlay;
    QPushButton *m_launchBtn;
    QTextEdit *m_logView;
    QLabel *m_statusLabel;
    QString m_accountId;
    bool m_setupDone = false;
    bool m_setupSuccess = false;
    bool m_launched = false;
};

// Page 3: Result
class ResultPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit ResultPage(QWidget *parent = nullptr);
    void initializePage() override;

private:
    QLabel *m_resultLabel;
    QLabel *m_detailLabel;
};

#endif // ALTSETUPWIZARD_H
