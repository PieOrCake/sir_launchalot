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

#include "core/InstallDetector.h"

class WineManager;

class SetupWizard : public QWizard
{
    Q_OBJECT

public:
    explicit SetupWizard(InstallDetector *detector,
                         WineManager *wine,
                         QWidget *parent = nullptr);

    QString accountName() const;
    QString winePrefix() const;
    QString gw2ExePath() const;
    QString wineBinary() const;
    QString protonPath() const;
    QString source() const;

    void setDetectedInstall(const QString &prefix, const QString &exe,
                            const QString &wine, const QString &proton,
                            const QString &source = {});

    enum PageId {
        Page_Welcome,
        Page_Detect,
        Page_Manual,
        Page_Name
    };

private:
    InstallDetector *m_detector;
    WineManager *m_wine;
    QString m_detectedPrefix;
    QString m_detectedExe;
    QString m_detectedWineBinary;
    QString m_detectedProtonPath;
    QString m_detectedSource;
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
    explicit DetectPage(InstallDetector *detector, QWidget *parent = nullptr);

    void initializePage() override;
    int nextId() const override;
    bool isComplete() const override;

private slots:
    void onSelectionChanged();

private:
    InstallDetector *m_detector;
    QLabel *m_statusLabel;
    QListWidget *m_installList;
    QLabel *m_detailLabel;
    QList<InstallDetector::DetectedInstall> m_installs;
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
