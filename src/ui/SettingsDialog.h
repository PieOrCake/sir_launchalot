#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    void setBasePrefix(const QString &prefix);
    QString basePrefix() const;

    void setGw2ExePath(const QString &path);
    QString gw2ExePath() const;

    void setApiRefreshInterval(int minutes);
    int apiRefreshInterval() const;

    void setCheckForUpdatesEnabled(bool enabled);
    bool checkForUpdatesEnabled() const;

    void setMultiLaunchDelay(int seconds);
    int multiLaunchDelay() const;

    void setConfirmMultiLaunchEnabled(bool enabled);
    bool confirmMultiLaunchEnabled() const;

private slots:
    void onBrowsePrefix();
    void onBrowseExe();

private:
    void setupUi();

    QLineEdit *m_prefixEdit;
    QLineEdit *m_exeEdit;
    QSlider *m_apiRefreshSlider;
    QLabel *m_apiRefreshLabel;
    QCheckBox *m_checkUpdatesBox;
    QSpinBox *m_multiLaunchDelaySpin;
    QCheckBox *m_confirmMultiLaunchBox;
};

#endif // SETTINGSDIALOG_H
