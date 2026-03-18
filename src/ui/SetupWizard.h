#ifndef SETUPWIZARD_H
#define SETUPWIZARD_H

#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QGroupBox>
#include <QVBoxLayout>

#include "core/LutrisIntegration.h"

class WineManager;

class SetupWizard : public QWizard
{
    Q_OBJECT

public:
    explicit SetupWizard(LutrisIntegration *lutris,
                         WineManager *wine,
                         QWidget *parent = nullptr);

    QString accountName() const;
    QString winePrefix() const;
    QString gw2ExePath() const;
    QString wineBinary() const;

    void setDetectedInstall(const QString &prefix, const QString &exe, const QString &wine);

    enum PageId {
        Page_Welcome,
        Page_Detect,
        Page_Manual,
        Page_Name
    };

private:
    LutrisIntegration *m_lutris;
    WineManager *m_wine;
    QString m_detectedPrefix;
    QString m_detectedExe;
    QString m_detectedWineBinary;
};

// ---- Welcome Page ----
class WelcomePage : public QWizardPage
{
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);
};

// ---- Detection Page ----
class DetectPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit DetectPage(LutrisIntegration *lutris, QWidget *parent = nullptr);

    void initializePage() override;
    int nextId() const override;
    bool isComplete() const override;

private slots:
    void onSelectionChanged();

private:
    LutrisIntegration *m_lutris;
    QLabel *m_statusLabel;
    QListWidget *m_installList;
    QLabel *m_detailLabel;
    QList<LutrisIntegration::LutrisGame> m_installs;
    int m_selectedIndex = -1;
};

// ---- Manual Config Page ----
class ManualConfigPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit ManualConfigPage(QWidget *parent = nullptr);
    bool isComplete() const override;

private slots:
    void onBrowsePrefix();
    void onBrowseExe();

private:
    QLineEdit *m_prefixEdit;
    QLineEdit *m_exeEdit;
};

// ---- Account Name Page ----
class AccountNamePage : public QWizardPage
{
    Q_OBJECT
public:
    explicit AccountNamePage(QWidget *parent = nullptr);
    bool isComplete() const override;

private:
    QLineEdit *m_nameEdit;
};

#endif // SETUPWIZARD_H
