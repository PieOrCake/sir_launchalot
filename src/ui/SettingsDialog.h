#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>

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

private slots:
    void onBrowsePrefix();
    void onBrowseExe();

private:
    void setupUi();

    QLineEdit *m_prefixEdit;
    QLineEdit *m_exeEdit;
    QSlider *m_apiRefreshSlider;
    QLabel *m_apiRefreshLabel;
};

#endif // SETTINGSDIALOG_H
