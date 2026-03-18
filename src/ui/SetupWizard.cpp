#include "ui/SetupWizard.h"
#include "core/LutrisIntegration.h"
#include "core/WineManager.h"

#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>

// ============================================================================
// SetupWizard
// ============================================================================

SetupWizard::SetupWizard(LutrisIntegration *lutris,
                         WineManager *wine,
                         QWidget *parent)
    : QWizard(parent)
    , m_lutris(lutris)
    , m_wine(wine)
{
    setWindowTitle("Sir Launchalot - First Time Setup");
    setWizardStyle(QWizard::ModernStyle);
    setMinimumSize(550, 400);

    setPage(Page_Welcome, new WelcomePage);
    setPage(Page_Detect, new DetectPage(lutris));
    setPage(Page_Manual, new ManualConfigPage);
    setPage(Page_Name, new AccountNamePage);

    setStartId(Page_Welcome);
}

QString SetupWizard::accountName() const
{
    return field("accountName").toString().trimmed();
}

QString SetupWizard::winePrefix() const
{
    if (!m_detectedPrefix.isEmpty()) return m_detectedPrefix;
    return field("manualPrefix").toString().trimmed();
}

QString SetupWizard::gw2ExePath() const
{
    if (!m_detectedExe.isEmpty()) return m_detectedExe;
    return field("manualExe").toString().trimmed();
}

QString SetupWizard::wineBinary() const
{
    return m_detectedWineBinary;
}

void SetupWizard::setDetectedInstall(const QString &prefix, const QString &exe, const QString &wine)
{
    m_detectedPrefix = prefix;
    m_detectedExe = exe;
    m_detectedWineBinary = wine;
}

// ============================================================================
// WelcomePage
// ============================================================================

WelcomePage::WelcomePage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Welcome to Sir Launchalot");
    setSubTitle("This wizard will help you set up your main Guild Wars 2 account.");

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(
        "Sir Launchalot lets you run multiple Guild Wars 2 accounts "
        "simultaneously on Linux using lightweight copy-on-write overlays."
        "<br><br>"
        "First, we need to locate your existing GW2 installation. This will "
        "become your <b>main account</b>, which runs directly from your "
        "existing WINEPREFIX."
        "<br><br>"
        "Alt accounts will be created later as overlay clones of the main "
        "account's prefix, so they share the game data without duplicating "
        "the ~50 GB install.");
    intro->setTextFormat(Qt::RichText);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addStretch();
}

// ============================================================================
// DetectPage
// ============================================================================

DetectPage::DetectPage(LutrisIntegration *lutris, QWidget *parent)
    : QWizardPage(parent)
    , m_lutris(lutris)
{
    setTitle("Detecting GW2 Installation");
    setSubTitle("Searching for existing Guild Wars 2 installations...");

    auto *layout = new QVBoxLayout(this);

    m_statusLabel = new QLabel;
    m_statusLabel->setWordWrap(true);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSize(statusFont.pointSize() + 1);
    statusFont.setBold(true);
    m_statusLabel->setFont(statusFont);
    layout->addWidget(m_statusLabel);

    m_installList = new QListWidget;
    m_installList->setVisible(false);
    connect(m_installList, &QListWidget::currentRowChanged,
            this, &DetectPage::onSelectionChanged);
    layout->addWidget(m_installList);

    m_detailLabel = new QLabel;
    m_detailLabel->setTextFormat(Qt::RichText);
    m_detailLabel->setWordWrap(true);
    layout->addWidget(m_detailLabel);

    layout->addStretch();
}

void DetectPage::initializePage()
{
    m_installs = m_lutris->discoverGW2Installs();
    m_installList->clear();
    m_selectedIndex = -1;

    if (!m_installs.isEmpty()) {
        m_statusLabel->setText(
            QString("\u2705  Found %1 GW2 installation%2")
                .arg(m_installs.size())
                .arg(m_installs.size() > 1 ? "s!" : "!"));
    } else {
        m_statusLabel->setText("\u274C  No GW2 installation detected automatically.");
    }

    // Always show the list with detected installs + manual browse option
    m_installList->setVisible(true);
    for (const auto &game : m_installs) {
        QString label = QString("%1\n    %2").arg(game.name, game.winePrefix);
        m_installList->addItem(label);
    }
    m_installList->addItem("Browse manually...\n    Locate a GW2 install not listed above");

    m_installList->setCurrentRow(0);

    m_detailLabel->setText("Select the installation to use as your <b>main account</b>, "
                           "then click <b>Next</b>.");

    emit completeChanged();
}

void DetectPage::onSelectionChanged()
{
    int row = m_installList->currentRow();
    auto *wiz = qobject_cast<SetupWizard *>(wizard());
    if (row >= 0 && row < m_installs.size()) {
        // A detected install
        m_selectedIndex = row;
        auto &gw2 = m_installs[row];
        if (wiz) wiz->setDetectedInstall(gw2.winePrefix, gw2.exePath, gw2.wineBinary);
    } else {
        // "Browse manually..." selected (or nothing)
        m_selectedIndex = -1;
        if (wiz) wiz->setDetectedInstall({}, {}, {});
    }
    emit completeChanged();
}

int DetectPage::nextId() const
{
    if (m_selectedIndex >= 0) {
        return SetupWizard::Page_Name;
    }
    return SetupWizard::Page_Manual;
}

bool DetectPage::isComplete() const
{
    return m_installList->currentRow() >= 0;
}

// ============================================================================
// ManualConfigPage
// ============================================================================

ManualConfigPage::ManualConfigPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Locate GW2 Installation");
    setSubTitle("Please provide the path to your WINEPREFIX and GW2 executable.");

    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(
        "The <b>WINEPREFIX</b> is the directory that contains your Wine "
        "environment (it has a <code>drive_c</code> folder inside it).\n\n"
        "The <b>executable</b> is typically at:<br>"
        "<code>drive_c/Program Files/Guild Wars 2/Gw2-64.exe</code>");
    info->setWordWrap(true);
    layout->addWidget(info);

    layout->addSpacing(10);

    // Prefix
    auto *prefixLayout = new QHBoxLayout;
    m_prefixEdit = new QLineEdit;
    m_prefixEdit->setPlaceholderText("e.g. /home/user/.local/share/games/guild-wars-2");
    auto *browsePrefix = new QPushButton("Browse...");
    connect(browsePrefix, &QPushButton::clicked, this, &ManualConfigPage::onBrowsePrefix);
    connect(m_prefixEdit, &QLineEdit::textChanged, this, &ManualConfigPage::completeChanged);
    prefixLayout->addWidget(m_prefixEdit);
    prefixLayout->addWidget(browsePrefix);

    auto *prefixLabel = new QLabel("WINEPREFIX:");
    layout->addWidget(prefixLabel);
    layout->addLayout(prefixLayout);

    layout->addSpacing(10);

    // Exe
    auto *exeLayout = new QHBoxLayout;
    m_exeEdit = new QLineEdit;
    m_exeEdit->setPlaceholderText("e.g. .../drive_c/Program Files/Guild Wars 2/Gw2-64.exe");
    auto *browseExe = new QPushButton("Browse...");
    connect(browseExe, &QPushButton::clicked, this, &ManualConfigPage::onBrowseExe);
    connect(m_exeEdit, &QLineEdit::textChanged, this, &ManualConfigPage::completeChanged);
    exeLayout->addWidget(m_exeEdit);
    exeLayout->addWidget(browseExe);

    auto *exeLabel = new QLabel("GW2 Executable:");
    layout->addWidget(exeLabel);
    layout->addLayout(exeLayout);

    layout->addStretch();

    registerField("manualPrefix", m_prefixEdit);
    registerField("manualExe", m_exeEdit);
}

bool ManualConfigPage::isComplete() const
{
    return !m_prefixEdit->text().trimmed().isEmpty() &&
           !m_exeEdit->text().trimmed().isEmpty();
}

void ManualConfigPage::onBrowsePrefix()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select WINEPREFIX",
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) {
        m_prefixEdit->setText(dir);

        // Auto-fill exe if found in prefix
        QString exe = LutrisIntegration::gw2ExeInPrefix(dir);
        if (!exe.isEmpty() && m_exeEdit->text().trimmed().isEmpty()) {
            m_exeEdit->setText(exe);
        }
    }
}

void ManualConfigPage::onBrowseExe()
{
    QString startDir = m_prefixEdit->text().trimmed().isEmpty()
                           ? QDir::homePath()
                           : m_prefixEdit->text().trimmed();
    QString path = QFileDialog::getOpenFileName(this, "Select Gw2-64.exe",
        startDir, "Executables (*.exe);;All files (*)", nullptr,
        QFileDialog::DontUseNativeDialog);
    if (!path.isEmpty()) {
        m_exeEdit->setText(path);
    }
}

// ============================================================================
// AccountNamePage
// ============================================================================

AccountNamePage::AccountNamePage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle("Name Your Main Account");
    setSubTitle("Give your main GW2 account a display name.");

    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(
        "This name is shown in the account list to help you tell your "
        "accounts apart. You can use your character name, account name, "
        "or anything you like.");
    info->setWordWrap(true);
    layout->addWidget(info);

    layout->addSpacing(20);

    auto *nameLabel = new QLabel("Display Name:");
    layout->addWidget(nameLabel);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("e.g. Main Account, My.1234, etc.");
    connect(m_nameEdit, &QLineEdit::textChanged, this, &AccountNamePage::completeChanged);
    layout->addWidget(m_nameEdit);

    layout->addStretch();

    registerField("accountName*", m_nameEdit);
}

bool AccountNamePage::isComplete() const
{
    return !m_nameEdit->text().trimmed().isEmpty();
}
