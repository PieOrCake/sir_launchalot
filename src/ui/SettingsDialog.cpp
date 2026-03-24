#include "ui/SettingsDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
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

    // API group
    auto *apiGroup = new QGroupBox("GW2 API");
    auto *apiForm = new QFormLayout(apiGroup);

    auto *sliderLayout = new QHBoxLayout;
    m_apiRefreshSlider = new QSlider(Qt::Horizontal);
    m_apiRefreshSlider->setRange(5, 60);
    m_apiRefreshSlider->setValue(15);
    m_apiRefreshSlider->setTickInterval(5);
    m_apiRefreshSlider->setTickPosition(QSlider::TicksBelow);
    m_apiRefreshLabel = new QLabel("15 min");
    m_apiRefreshLabel->setFixedWidth(50);
    connect(m_apiRefreshSlider, &QSlider::valueChanged, this, [this](int val) {
        m_apiRefreshLabel->setText(QString("%1 min").arg(val));
    });
    sliderLayout->addWidget(m_apiRefreshSlider);
    sliderLayout->addWidget(m_apiRefreshLabel);
    apiForm->addRow("Auto-refresh interval:", sliderLayout);

    layout->addWidget(apiGroup);

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

void SettingsDialog::setApiRefreshInterval(int minutes)
{
    m_apiRefreshSlider->setValue(qBound(5, minutes, 60));
}

int SettingsDialog::apiRefreshInterval() const
{
    return m_apiRefreshSlider->value();
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

