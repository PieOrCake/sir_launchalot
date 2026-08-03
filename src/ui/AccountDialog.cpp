#include "ui/AccountDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QMessageBox>
#include <QUuid>
#include <QVBoxLayout>

static bool editSidecar(AccountManager::SidecarProgram &sc, QWidget *parent,
                        const QString &basePrefix = {})
{
    QDialog dlg(parent);
    dlg.setWindowTitle(sc.id.isEmpty() ? "Add Sidecar" : "Edit Sidecar");
    dlg.setMinimumWidth(400);
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;

    auto *nameEdit = new QLineEdit(sc.name);
    nameEdit->setPlaceholderText("e.g. Discord IPC Bridge");
    form->addRow("Name:", nameEdit);

    auto *pathEdit = new QLineEdit(sc.exePath);
    pathEdit->setPlaceholderText(R"(e.g. C:\tools\winediscordipcbridge.exe)");
    auto *browseBtn = new QPushButton("Browse...");
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(pathEdit);
    pathRow->addWidget(browseBtn);
    form->addRow("Exe path:", pathRow);
    QObject::connect(browseBtn, &QPushButton::clicked, &dlg, [&pathEdit, &dlg, basePrefix]() {
        QString driveC = basePrefix + "/drive_c";
        QString start = QDir(driveC).exists() ? driveC : QDir::homePath();
        QString picked = QFileDialog::getOpenFileName(
            &dlg, "Select Exe", start, "Executables (*.exe *.EXE);;All Files (*)");
        if (picked.isEmpty()) return;
        // Auto-convert Linux path inside a Wine prefix to Windows style
        // e.g. /prefix/drive_c/tools/bridge.exe -> C:\tools\bridge.exe
        static const QRegularExpression driveRe("/drive_([a-z])/");
        auto m = driveRe.match(picked);
        if (m.hasMatch()) {
            QString rest = picked.mid(m.capturedEnd());
            rest.replace('/', '\\');
            picked = m.captured(1).toUpper() + ":\\" + rest;
        }
        pathEdit->setText(picked);
    });

    auto *argsEdit = new QLineEdit(sc.args.join(" "));
    argsEdit->setPlaceholderText("Space-separated; quoted args not supported");
    form->addRow("Arguments:", argsEdit);

    layout->addLayout(form);
    auto *label = new QLabel("Use an absolute Windows-style path, e.g. C:\\tools\\bridge.exe");
    label->setWordWrap(true);
    label->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(label);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return false;
    if (nameEdit->text().trimmed().isEmpty() || pathEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(parent, "Sidecar Program", "Name and Exe path are required.");
        return false;
    }

    sc.name = nameEdit->text().trimmed();
    sc.exePath = pathEdit->text().trimmed();
    sc.args = argsEdit->text().split(' ', Qt::SkipEmptyParts);
    return true;
}

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

    m_argsEdit = new QLineEdit;
    m_argsEdit->setPlaceholderText("e.g. -shareArchive -maploadinfo");
    m_argsEdit->setToolTip("Extra command-line arguments passed to Gw2-64.exe");
    form->addRow("Extra arguments:", m_argsEdit);
    layout->addLayout(form);

    m_mainCheck = new QCheckBox;
    m_mainCheck->setVisible(false);

    m_enableAddonsCheck = new QCheckBox("Enable addons");
    m_enableAddonsCheck->setChecked(true);
    layout->addWidget(m_enableAddonsCheck);

    // Launch command (shown for Steam accounts only)
    auto *cmdForm = new QFormLayout;
    m_launchCommandEdit = new QLineEdit;
    m_launchCommandEdit->setMinimumWidth(350);
    cmdForm->addRow("Launch command:", m_launchCommandEdit);
    layout->addLayout(cmdForm);
    m_launchCommandLabel = cmdForm->labelForField(m_launchCommandEdit);
    m_launchCommandEdit->setVisible(false);
    m_launchCommandLabel->setVisible(false);

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

    // Sidecar Programs section
    auto *sidecarGroup = new QGroupBox("Sidecar Programs");
    auto *sidecarLayout = new QVBoxLayout(sidecarGroup);

    m_sidecarTable = new QTableWidget(0, 2);
    m_sidecarTable->setHorizontalHeaderLabels({"Name", "Exe Path"});
    m_sidecarTable->horizontalHeader()->setStretchLastSection(true);
    m_sidecarTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sidecarTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sidecarTable->setMinimumHeight(100);
    sidecarLayout->addWidget(m_sidecarTable);

    auto *scBtnLayout = new QHBoxLayout;
    auto *scAddBtn = new QPushButton("Add");
    auto *scEditBtn = new QPushButton("Edit");
    auto *scRemoveBtn = new QPushButton("Remove");
    scBtnLayout->addWidget(scAddBtn);
    scBtnLayout->addWidget(scEditBtn);
    scBtnLayout->addWidget(scRemoveBtn);
    scBtnLayout->addStretch();
    sidecarLayout->addLayout(scBtnLayout);
    layout->addWidget(sidecarGroup);

    connect(scAddBtn, &QPushButton::clicked, this, [this]() {
        AccountManager::SidecarProgram sc;
        if (editSidecar(sc, this, m_basePrefix)) {
            sc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_sidecars.append(sc);
            refreshSidecarTable();
        }
    });

    connect(scEditBtn, &QPushButton::clicked, this, [this]() {
        int row = m_sidecarTable->currentRow();
        if (row < 0 || row >= m_sidecars.size()) return;
        if (editSidecar(m_sidecars[row], this))
            refreshSidecarTable();
    });

    connect(scRemoveBtn, &QPushButton::clicked, this, [this]() {
        int row = m_sidecarTable->currentRow();
        if (row < 0 || row >= m_sidecars.size()) return;
        m_sidecars.removeAt(row);
        refreshSidecarTable();
    });

    connect(m_sidecarTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex &idx) {
        int row = idx.row();
        if (row >= 0 && row < m_sidecars.size())
            if (editSidecar(m_sidecars[row], this))
                refreshSidecarTable();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void AccountDialog::refreshSidecarTable()
{
    m_sidecarTable->setRowCount(m_sidecars.size());
    for (int i = 0; i < m_sidecars.size(); ++i) {
        m_sidecarTable->setItem(i, 0, new QTableWidgetItem(m_sidecars[i].name));
        m_sidecarTable->setItem(i, 1, new QTableWidgetItem(m_sidecars[i].exePath));
    }
}

void AccountDialog::setBasePrefix(const QString &prefix)
{
    m_basePrefix = prefix;
}

void AccountDialog::setAccount(const AccountManager::Account &account)
{
    m_editMode = true;
    m_accountId = account.id;
    setWindowTitle("Edit Account");

    m_nameEdit->setText(account.displayName);
    m_argsEdit->setText(account.extraArgs.join(' '));

    m_mainCheck->setChecked(account.isMain);

    if (account.isSteam) {
        setSteamMode(account.launchCommand);
    } else {
        m_enableAddonsCheck->setChecked(account.enableAddons);
        m_enableAddonsCheck->setVisible(!account.isMain);
    }

    m_apiKeyEdit->setText(account.apiKey);
    m_showAccountNameCheck->setChecked(account.showAccountName);
    m_showDailyVaultCheck->setChecked(account.showDailyVault);
    m_showWeeklyVaultCheck->setChecked(account.showWeeklyVault);
    m_sidecars = account.sidecars;
    refreshSidecarTable();
}

void AccountDialog::setSteamMode(const QString &defaultCommand)
{
    m_steamMode = true;
    m_enableAddonsCheck->setVisible(false);
    m_launchCommandEdit->setVisible(true);
    m_launchCommandLabel->setVisible(true);
    m_launchCommandEdit->setText(defaultCommand);
}

AccountManager::Account AccountDialog::account() const
{
    AccountManager::Account acct;
    acct.id = m_editMode ? m_accountId
                         : QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    acct.displayName = m_nameEdit->text().trimmed();
    acct.extraArgs = QProcess::splitCommand(m_argsEdit->text());
    acct.isMain = m_mainCheck->isChecked();
    acct.isSteam = m_steamMode;
    acct.launchCommand = m_launchCommandEdit->text().trimmed();
    acct.enableAddons = m_enableAddonsCheck->isChecked();
    acct.apiKey = m_apiKeyEdit->text().trimmed();
    acct.showAccountName = m_showAccountNameCheck->isChecked();
    acct.showDailyVault = m_showDailyVaultCheck->isChecked();
    acct.showWeeklyVault = m_showWeeklyVaultCheck->isChecked();
    acct.sidecars = m_sidecars;

    return acct;
}

