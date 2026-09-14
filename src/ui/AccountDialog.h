#ifndef ACCOUNTDIALOG_H
#define ACCOUNTDIALOG_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QHash>
#include <QLineEdit>
#include <QList>
#include <QMap>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/AccountManager.h"
#include "core/AddonScanner.h"

class AccountDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AccountDialog(QWidget *parent = nullptr);

    void setAccount(const AccountManager::Account &account);
    void setSteamMode(const QString &defaultCommand);
    void setBasePrefix(const QString &prefix);
    // Main account's addons folder; enables the per-addon picker.
    void setAddonsDir(const QString &dir);
    AccountManager::Account account() const;

signals:
    // Asks the owner to re-copy this alt's addon settings from the main install.
    void resyncAddonSettingsRequested(const QString &accountId);

protected:
    // Sizes the window once, on first show, when every section has been filled in.
    void showEvent(QShowEvent *event) override;

private:
    void setupUi();
    void buildAddonSection(QVBoxLayout *parent);
    void buildEnvSection(QVBoxLayout *parent);
    void refreshEnvTable();
    void rebuildAddonTable();
    void applyAddonMode();
    void startSizeScan();

    bool m_sized = false;
    bool m_editMode = false;
    bool m_steamMode = false;
    QString m_accountId;
    QString m_basePrefix;
    QWidget *m_content = nullptr;
    QLineEdit *m_nameEdit;
    QLineEdit *m_argsEdit;
    QCheckBox *m_mainCheck;

    // Addons
    QString m_addonsDir;
    QGroupBox *m_addonGroup = nullptr;
    QComboBox *m_addonModeCombo = nullptr;
    QWidget *m_addonPicker = nullptr;
    QTableWidget *m_addonTable = nullptr;
    QCheckBox *m_copyNexusLayoutCheck = nullptr;
    QPushButton *m_resyncButton = nullptr;
    QList<AddonScanner::AddonInfo> m_addons;
    QStringList m_dataDirChoices;
    QHash<QString, qint64> m_dataDirSizes;   // folder name -> bytes
    // Selections carried in from setAccount() before the folder has been scanned.
    QList<AccountManager::AddonSelection> m_pendingSelections;
    bool m_haveAccountSelections = false;
    QLineEdit *m_launchCommandEdit;
    QWidget *m_launchCommandLabel;
    QLineEdit *m_apiKeyEdit;
    QCheckBox *m_showAccountNameCheck;
    QCheckBox *m_showDailyVaultCheck;
    QCheckBox *m_showWeeklyVaultCheck;
    QCheckBox *m_showLastSeenCheck;
    QTableWidget *m_envTable = nullptr;
    QMap<QString, QString> m_envVars;
    QTableWidget *m_sidecarTable;
    QList<AccountManager::SidecarProgram> m_sidecars;
    void refreshSidecarTable();
};

#endif // ACCOUNTDIALOG_H
