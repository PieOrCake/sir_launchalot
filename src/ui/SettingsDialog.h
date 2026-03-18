#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

class WineManager;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(WineManager *wineManager, QWidget *parent = nullptr);

    void setBasePrefix(const QString &prefix);
    QString basePrefix() const;

    void setGw2ExePath(const QString &path);
    QString gw2ExePath() const;

private slots:
    void onBrowsePrefix();
    void onBrowseExe();

private:
    void setupUi();

    WineManager *m_wineManager;

    QLineEdit *m_prefixEdit;
    QLineEdit *m_exeEdit;
    QLabel *m_runnerLabel;
    QSpinBox *m_delaySpin;
};

#endif // SETTINGSDIALOG_H
