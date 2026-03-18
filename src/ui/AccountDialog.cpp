#include "ui/AccountDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
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
}

AccountManager::Account AccountDialog::account() const
{
    AccountManager::Account acct;
    acct.id = m_editMode ? m_accountId
                         : QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    acct.displayName = m_nameEdit->text().trimmed();
    acct.isMain = m_mainCheck->isChecked();
    acct.enableAddons = m_enableAddonsCheck->isChecked();

    return acct;
}

