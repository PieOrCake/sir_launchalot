#include "ui/SettingsDialog.h"
#include "core/WineManager.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(WineManager *wineManager, QWidget *parent)
    : QDialog(parent)
    , m_wineManager(wineManager)
{
    setupUi();
    setWindowTitle("Settings");
    resize(500, 300);
}

void SettingsDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);

    // GW2 Installation group
    auto *gw2Group = new QGroupBox("GW2 Installation");
    auto *gw2Form = new QFormLayout(gw2Group);

    auto *prefixLayout = new QHBoxLayout;
    m_prefixEdit = new QLineEdit;
    m_prefixEdit->setPlaceholderText("Path to base WINEPREFIX");
    auto *browsePrefixBtn = new QPushButton("Browse...");
    connect(browsePrefixBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowsePrefix);
    prefixLayout->addWidget(m_prefixEdit);
    prefixLayout->addWidget(browsePrefixBtn);
    gw2Form->addRow("Base WINEPREFIX:", prefixLayout);

    auto *exeLayout = new QHBoxLayout;
    m_exeEdit = new QLineEdit;
    m_exeEdit->setPlaceholderText("Path to Gw2-64.exe");
    auto *browseExeBtn = new QPushButton("Browse...");
    connect(browseExeBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseExe);
    exeLayout->addWidget(m_exeEdit);
    exeLayout->addWidget(browseExeBtn);
    gw2Form->addRow("GW2 Executable:", exeLayout);

    layout->addWidget(gw2Group);

    // Wine Runner group (read-only)
    auto *wineGroup = new QGroupBox("Wine Runner (detected)");
    auto *wineForm = new QFormLayout(wineGroup);

    auto selected = m_wineManager->selectedRunner();
    m_runnerLabel = new QLabel;
    if (!selected.path.isEmpty()) {
        m_runnerLabel->setText(QString("%1 (%2)").arg(selected.name, selected.version));
        m_runnerLabel->setToolTip(selected.path);
    } else {
        m_runnerLabel->setText("Not detected — run Setup Wizard");
    }
    wineForm->addRow("Runner:", m_runnerLabel);

    layout->addWidget(wineGroup);

    // Launch group
    auto *launchGroup = new QGroupBox("Launch Options");
    auto *launchForm = new QFormLayout(launchGroup);

    m_delaySpin = new QSpinBox;
    m_delaySpin->setRange(0, 60);
    m_delaySpin->setValue(5);
    m_delaySpin->setSuffix(" seconds");
    launchForm->addRow("Delay between launches:", m_delaySpin);

    layout->addWidget(launchGroup);

    // Buttons
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}


void SettingsDialog::setBasePrefix(const QString &prefix)
{
    m_prefixEdit->setText(prefix);
}

QString SettingsDialog::basePrefix() const
{
    return m_prefixEdit->text().trimmed();
}

void SettingsDialog::setGw2ExePath(const QString &path)
{
    m_exeEdit->setText(path);
}

QString SettingsDialog::gw2ExePath() const
{
    return m_exeEdit->text().trimmed();
}

void SettingsDialog::onBrowsePrefix()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select Base WINEPREFIX",
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) {
        m_prefixEdit->setText(dir);
    }
}

void SettingsDialog::onBrowseExe()
{
    QString path = QFileDialog::getOpenFileName(this, "Select Gw2-64.exe",
        m_prefixEdit->text(), "Executables (*.exe);;All files (*)", nullptr,
        QFileDialog::DontUseNativeDialog);
    if (!path.isEmpty()) {
        m_exeEdit->setText(path);
    }
}

