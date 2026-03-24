#include "ui/AccountDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QUuid>
#include <QVBoxLayout>

AccountDialog::AccountDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle("Add Alt Account");
    setMinimumWidth(350);
}

void AccountDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    auto *form = new QFormLayout;
    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("e.g. Alt1, PvP Account, Crafter");
    m_nameEdit->setMinimumWidth(250);
    form->addRow("Name:", m_nameEdit);
    layout->addLayout(form);

    m_mainCheck = new QCheckBox;
    m_mainCheck->setVisible(false);

    m_enableAddonsCheck = new QCheckBox("Enable addons");
    m_enableAddonsCheck->setChecked(true);
    layout->addWidget(m_enableAddonsCheck);

    // GW2 API section
    auto *apiGroup = new QGroupBox("GW2 API");
    auto *apiLayout = new QVBoxLayout(apiGroup);
    auto *apiForm = new QFormLayout;
    m_apiKeyEdit = new QLineEdit;
    m_apiKeyEdit->setPlaceholderText("Paste API key from account.arena.net");
    m_apiKeyEdit->setMinimumWidth(350);
    
    apiForm->addRow("API Key:", m_apiKeyEdit);
    apiLayout->addLayout(apiForm);

    auto *showLabel = new QLabel("Show in account list:");
    apiLayout->addWidget(showLabel);
    m_showAccountNameCheck = new QCheckBox("Account name");
    apiLayout->addWidget(m_showAccountNameCheck);
    m_showDailyVaultCheck = new QCheckBox("Daily Wizard's Vault progress");
    apiLayout->addWidget(m_showDailyVaultCheck);
    m_showWeeklyVaultCheck = new QCheckBox("Weekly Wizard's Vault progress");
    apiLayout->addWidget(m_showWeeklyVaultCheck);
    layout->addWidget(apiGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void AccountDialog::setAccount(const AccountManager::Account &account)
{
    m_editMode = true;
    m_accountId = account.id;
    setWindowTitle("Edit Account");

    m_nameEdit->setText(account.displayName);

    m_mainCheck->setChecked(account.isMain);

    m_enableAddonsCheck->setChecked(account.enableAddons);
    m_enableAddonsCheck->setVisible(!account.isMain);

    m_apiKeyEdit->setText(account.apiKey);
    m_showAccountNameCheck->setChecked(account.showAccountName);
    m_showDailyVaultCheck->setChecked(account.showDailyVault);
    m_showWeeklyVaultCheck->setChecked(account.showWeeklyVault);
}

AccountManager::Account AccountDialog::account() const
{
    AccountManager::Account acct;
    acct.id = m_editMode ? m_accountId
                         : QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    acct.displayName = m_nameEdit->text().trimmed();
    acct.isMain = m_mainCheck->isChecked();
    acct.enableAddons = m_enableAddonsCheck->isChecked();
    acct.apiKey = m_apiKeyEdit->text().trimmed();
    acct.showAccountName = m_showAccountNameCheck->isChecked();
    acct.showDailyVault = m_showDailyVaultCheck->isChecked();
    acct.showWeeklyVault = m_showWeeklyVaultCheck->isChecked();

    return acct;
}

