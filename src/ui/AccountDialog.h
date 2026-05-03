#ifndef ACCOUNTDIALOG_H
#define ACCOUNTDIALOG_H

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>

#include "core/AccountManager.h"

class AccountDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AccountDialog(QWidget *parent = nullptr);

    void setAccount(const AccountManager::Account &account);
    void setSteamMode(const QString &defaultCommand);
    AccountManager::Account account() const;

private:
    void setupUi();

    bool m_editMode = false;
    bool m_steamMode = false;
    QString m_accountId;
    QLineEdit *m_nameEdit;
    QCheckBox *m_mainCheck;
    QCheckBox *m_enableAddonsCheck;
    QLineEdit *m_launchCommandEdit;
    QWidget *m_launchCommandLabel;
    QLineEdit *m_apiKeyEdit;
    QCheckBox *m_showAccountNameCheck;
    QCheckBox *m_showDailyVaultCheck;
    QCheckBox *m_showWeeklyVaultCheck;
};

#endif // ACCOUNTDIALOG_H
