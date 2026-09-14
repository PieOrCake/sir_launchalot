#include "ui/AccountDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QThreadPool>
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
    // Minimum rather than fixed: the addon table is the one part of this dialog
    // worth widening, and a fixed size leaves no way to read a clipped column.
    layout->setSizeConstraint(QLayout::SetMinimumSize);

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

    buildAddonSection(layout);

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
    m_showLastSeenCheck = new QCheckBox("Last seen");
    apiLayout->addWidget(m_showLastSeenCheck);
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

// A table that asks for room to show about eight rows, but will shrink to four
// when the dialog has to fit a short screen. QTableWidget's own size hint does
// not grow with its contents, so the dialog would otherwise open cramped.
// No Q_OBJECT, so no moc pass is needed for it.
class AddonTable : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;
    QSize sizeHint() const override
    {
        QSize hint = QTableWidget::sizeHint();
        hint.setHeight(qMax(hint.height(), 300));
        return hint;
    }
};

// Column layout of the addon picker.
enum AddonColumn { ColAddon = 0, ColCopySettings, ColDataDir, ColSize, ColCount };

void AccountDialog::buildAddonSection(QVBoxLayout *parent)
{
    m_addonGroup = new QGroupBox("Addons");
    auto *box = new QVBoxLayout(m_addonGroup);

    m_addonModeCombo = new QComboBox;
    m_addonModeCombo->addItem("No addons", static_cast<int>(AccountManager::AddonMode::Off));
    m_addonModeCombo->addItem("All addons", static_cast<int>(AccountManager::AddonMode::All));
    m_addonModeCombo->addItem("Selected addons...",
                              static_cast<int>(AccountManager::AddonMode::Selected));
    m_addonModeCombo->setCurrentIndex(1);
    box->addWidget(m_addonModeCombo);

    m_addonPicker = new QWidget;
    auto *pick = new QVBoxLayout(m_addonPicker);
    pick->setContentsMargins(0, 0, 0, 0);

    m_addonTable = new AddonTable(0, ColCount);
    m_addonTable->setHorizontalHeaderLabels({"Addon", "Copy settings", "Settings folder", "Size"});
    m_addonTable->verticalHeader()->setVisible(false);
    m_addonTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_addonTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Kept modest so the dialog still fits a laptop screen: it expands into
    // whatever height the window is given, and the window is resizable.
    m_addonTable->setMinimumHeight(150);
    m_addonTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The addon name absorbs the slack; the other three are draggable, starting
    // wide enough for their content — a size reads "785.6 MB", and the longest
    // settings folder name is much wider than the column needs to be.
    const QFontMetrics fm = m_addonTable->fontMetrics();
    const int pad = 28;   // checkbox indicator, sort arrow and cell margins
    auto *header = m_addonTable->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(ColAddon, QHeaderView::Stretch);
    header->setSectionResizeMode(ColCopySettings, QHeaderView::Interactive);
    header->setSectionResizeMode(ColDataDir, QHeaderView::Interactive);
    header->setSectionResizeMode(ColSize, QHeaderView::Interactive);
    header->setMinimumSectionSize(40);
    m_addonTable->setColumnWidth(ColCopySettings, fm.horizontalAdvance("Copy settings") + pad);
    m_addonTable->setColumnWidth(ColDataDir, 170);
    m_addonTable->setColumnWidth(ColSize, fm.horizontalAdvance("8888.8 MB") + pad);

    // Enough width for the name column to be useful once the rest have taken
    // theirs, without forcing the whole dialog wider than it needs to be.
    m_addonTable->setMinimumWidth(620);
    pick->addWidget(m_addonTable, 1);

    m_copyNexusLayoutCheck = new QCheckBox("Copy Nexus window layout and keybinds from main");
    m_copyNexusLayoutCheck->setChecked(true);
    m_copyNexusLayoutCheck->setToolTip(
        "Window positions and keybinds are shared by every addon, so they are copied "
        "as one. Seeded on this alt's first launch and left alone after that.");
    pick->addWidget(m_copyNexusLayoutCheck);

    auto *note = new QLabel(
        "Settings are copied once, on this alt's first launch. After that the alt keeps "
        "its own. The addons themselves are linked, not copied, so they stay up to date "
        "with the main account.");
    note->setWordWrap(true);
    note->setStyleSheet("color: gray; font-size: 11px;");
    pick->addWidget(note);

    auto *btnRow = new QHBoxLayout;
    m_resyncButton = new QPushButton("Resync settings from main");
    m_resyncButton->setToolTip("Replace this alt's addon settings with the main account's.");
    btnRow->addWidget(m_resyncButton);
    btnRow->addStretch();
    pick->addLayout(btnRow);

    box->addWidget(m_addonPicker);
    m_addonPicker->setVisible(false);
    parent->addWidget(m_addonGroup);

    connect(m_addonModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyAddonMode();
    });

    connect(m_resyncButton, &QPushButton::clicked, this, [this]() {
        if (m_accountId.isEmpty()) {
            QMessageBox::information(this, "Resync Addon Settings",
                "Save this account first — settings are copied on its first launch.");
            return;
        }
        auto answer = QMessageBox::question(this, "Resync Addon Settings",
            "Replace this alt's addon settings with the main account's current ones?\n\n"
            "Anything you have changed on this alt will be lost.");
        if (answer == QMessageBox::Yes)
            emit resyncAddonSettingsRequested(m_accountId);
    });
}

void AccountDialog::applyAddonMode()
{
    const bool selected = m_addonModeCombo->currentData().toInt()
                          == static_cast<int>(AccountManager::AddonMode::Selected);
    const bool wasVisible = m_addonPicker->isVisible();
    m_addonPicker->setVisible(selected);
    if (selected && m_addonTable->rowCount() == 0)
        rebuildAddonTable();
    // Shrink back when the table goes away; growing is left to the user so a
    // dialog they have widened stays widened.
    if (wasVisible && !selected)
        adjustSize();
}

void AccountDialog::setAddonsDir(const QString &dir)
{
    m_addonsDir = dir;
    m_addons = AddonScanner::scan(dir);
    m_dataDirChoices = AddonScanner::candidateDataDirs(dir);
    rebuildAddonTable();
    startSizeScan();
}

void AccountDialog::rebuildAddonTable()
{
    if (!m_addonTable) return;

    // What was ticked before: either the account being edited, or whatever the
    // user has already set in this dialog.
    QHash<QString, AccountManager::AddonSelection> chosen;
    if (m_haveAccountSelections) {
        for (const auto &sel : m_pendingSelections)
            chosen.insert(sel.dll, sel);
        m_haveAccountSelections = false;
    } else {
        for (int r = 0; r < m_addonTable->rowCount(); ++r) {
            auto *item = m_addonTable->item(r, ColAddon);
            if (!item || item->checkState() != Qt::Checked) continue;
            AccountManager::AddonSelection sel;
            sel.dll = item->data(Qt::UserRole).toString();
            auto *copyItem = m_addonTable->item(r, ColCopySettings);
            sel.copySettings = copyItem && copyItem->checkState() == Qt::Checked;
            if (auto *combo = qobject_cast<QComboBox *>(m_addonTable->cellWidget(r, ColDataDir)))
                sel.dataDir = combo->currentData().toString();
            chosen.insert(sel.dll, sel);
        }
    }

    // Drop the rows outright rather than repopulating them. clearContents()
    // leaves cell widgets behind, and the table is rebuilt several times as the
    // account and the folder sizes arrive — each pass would strand another
    // drop-down in the viewport.
    m_addonTable->setRowCount(0);
    m_addonTable->setRowCount(m_addons.size());
    for (int r = 0; r < m_addons.size(); ++r) {
        const auto &addon = m_addons[r];
        const bool isChosen = chosen.contains(addon.dll);
        const auto sel = chosen.value(addon.dll);

        auto *nameItem = new QTableWidgetItem(addon.displayName);
        nameItem->setData(Qt::UserRole, addon.dll);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        nameItem->setCheckState(isChosen ? Qt::Checked : Qt::Unchecked);
        nameItem->setToolTip(addon.dll);
        m_addonTable->setItem(r, ColAddon, nameItem);

        auto *copyItem = new QTableWidgetItem;
        copyItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        copyItem->setCheckState((isChosen ? sel.copySettings : true) ? Qt::Checked
                                                                     : Qt::Unchecked);
        m_addonTable->setItem(r, ColCopySettings, copyItem);

        auto *combo = new QComboBox;
        combo->addItem("(none)", QString());
        for (const QString &d : m_dataDirChoices)
            combo->addItem(d, d);
        const QString want = isChosen ? sel.dataDir : addon.dataDir;
        // Keep the longest folder name from dictating the column width; the
        // drop-down list itself is still shown in full.
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(12);
        const int idx = combo->findData(want);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        if (addon.dataDir.isEmpty() && want.isEmpty()) {
            combo->setToolTip("No settings folder detected for this addon. "
                              "Pick one if it keeps settings under a different name.");
        }
        m_addonTable->setCellWidget(r, ColDataDir, combo);

        auto *sizeItem = new QTableWidgetItem;
        sizeItem->setFlags(Qt::ItemIsEnabled);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_addonTable->setItem(r, ColSize, sizeItem);

        connect(combo, &QComboBox::currentIndexChanged, this, [this, r](int) {
            auto *combo = qobject_cast<QComboBox *>(m_addonTable->cellWidget(r, ColDataDir));
            auto *item = m_addonTable->item(r, ColSize);
            if (!combo || !item) return;
            const QString dir = combo->currentData().toString();
            item->setText(dir.isEmpty() ? QString()
                                        : AddonScanner::formatSize(m_dataDirSizes.value(dir, -1)));
        });
    }

    // Fill in whatever sizes are already known.
    for (int r = 0; r < m_addonTable->rowCount(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_addonTable->cellWidget(r, ColDataDir));
        auto *item = m_addonTable->item(r, ColSize);
        if (!combo || !item) continue;
        const QString dir = combo->currentData().toString();
        if (!dir.isEmpty() && m_dataDirSizes.contains(dir))
            item->setText(AddonScanner::formatSize(m_dataDirSizes.value(dir)));
    }
}

void AccountDialog::startSizeScan()
{
    if (m_addonsDir.isEmpty() || m_dataDirChoices.isEmpty()) return;

    const QString dir = m_addonsDir;
    const QStringList names = m_dataDirChoices;
    QPointer<AccountDialog> guard(this);

    // Walking every settings folder touches tens of thousands of files, so it
    // happens off the UI thread and the sizes appear as they are worked out.
    QThreadPool::globalInstance()->start([dir, names, guard]() {
        QHash<QString, qint64> sizes;
        for (const QString &name : names)
            sizes.insert(name, AddonScanner::directorySize(dir + "/" + name));
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, sizes]() {
            if (!guard) return;
            guard->m_dataDirSizes = sizes;
            guard->rebuildAddonTable();
        }, Qt::QueuedConnection);
    });
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
        const int idx = m_addonModeCombo->findData(static_cast<int>(account.addonMode));
        m_addonModeCombo->setCurrentIndex(idx >= 0 ? idx : 1);
        m_copyNexusLayoutCheck->setChecked(account.copyNexusLayout);
        m_pendingSelections = account.addonSelections;
        m_haveAccountSelections = true;
        rebuildAddonTable();
        applyAddonMode();
        m_addonGroup->setVisible(!account.isMain);
    }

    m_apiKeyEdit->setText(account.apiKey);
    m_showAccountNameCheck->setChecked(account.showAccountName);
    m_showDailyVaultCheck->setChecked(account.showDailyVault);
    m_showWeeklyVaultCheck->setChecked(account.showWeeklyVault);
    m_showLastSeenCheck->setChecked(account.showLastSeen);
    m_sidecars = account.sidecars;
    refreshSidecarTable();
}

void AccountDialog::setSteamMode(const QString &defaultCommand)
{
    m_steamMode = true;
    m_addonGroup->setVisible(false);
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
    acct.addonMode = static_cast<AccountManager::AddonMode>(
        m_addonModeCombo->currentData().toInt());
    acct.enableAddons = acct.addonMode != AccountManager::AddonMode::Off;
    acct.copyNexusLayout = m_copyNexusLayoutCheck->isChecked();

    // The ticks are kept even when the mode is not Selected, so switching away
    // and back does not lose them.
    if (m_addonTable->rowCount() > 0) {
        for (int r = 0; r < m_addonTable->rowCount(); ++r) {
            auto *item = m_addonTable->item(r, ColAddon);
            if (!item || item->checkState() != Qt::Checked) continue;
            AccountManager::AddonSelection sel;
            sel.dll = item->data(Qt::UserRole).toString();
            auto *copyItem = m_addonTable->item(r, ColCopySettings);
            sel.copySettings = copyItem && copyItem->checkState() == Qt::Checked;
            if (auto *combo = qobject_cast<QComboBox *>(m_addonTable->cellWidget(r, ColDataDir)))
                sel.dataDir = combo->currentData().toString();
            acct.addonSelections.append(sel);
        }
    } else {
        acct.addonSelections = m_pendingSelections;
    }
    acct.apiKey = m_apiKeyEdit->text().trimmed();
    acct.showAccountName = m_showAccountNameCheck->isChecked();
    acct.showDailyVault = m_showDailyVaultCheck->isChecked();
    acct.showWeeklyVault = m_showWeeklyVaultCheck->isChecked();
    acct.showLastSeen = m_showLastSeenCheck->isChecked();
    acct.sidecars = m_sidecars;

    return acct;
}

