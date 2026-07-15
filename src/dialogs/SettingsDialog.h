#pragma once

#include <QDialog>

class AppSettings;
class UpdateManager;
struct UpdateInfo;
class QListWidget;
class QStackedWidget;
class QLineEdit;
class DetailGridEditor;
class QListWidget;
class QPlainTextEdit;
class QLabel;
class QCheckBox;
class QPushButton;

class SettingsDialog : public QDialog
{
public:
    explicit SettingsDialog(AppSettings* settings, QWidget* parent = nullptr);

private slots:
    void apply();

private:
    void setupUi();
    QWidget* createGeneralPage();
    QWidget* createStarDetailPage();
    QWidget* createGridPathsPage();
    QWidget* createLightcurveFetchPage();
    QWidget *createLightcurveFitPage();
    QWidget* createUpdatesPage();

    QLineEdit *_lcurveDirEdit = nullptr;
    QLabel  *_lcurveStatusLbl = nullptr;
    AppSettings*   _settings;

    QListWidget*   _topicList = nullptr;
    QStackedWidget* _pages    = nullptr;

    // General page
    QLineEdit*     _isisEdit   = nullptr;
    QLineEdit*     _sedFitEdit = nullptr;

    // Star detail page
    DetailGridEditor* _gridEditor = nullptr;

    QListWidget* _gridPathsList = nullptr;

    QLineEdit*       _lcqPythonEdit    = nullptr;
    QLineEdit*       _lcqScriptEdit    = nullptr;
    QLineEdit*       _atlasTokenEdit   = nullptr;
    QLineEdit*       _adsTokenEdit     = nullptr;
    QLineEdit*       _blackgemEdit     = nullptr;
    QLabel*          _lcqTestResult    = nullptr;

    // Updates page
    QCheckBox*       _updateOnStartup  = nullptr;
    QLabel*          _updateStatus     = nullptr;
    QPushButton*     _updateCheckBtn   = nullptr;
    QPushButton*     _updateInstallBtn = nullptr;
    UpdateManager*   _updater          = nullptr;
    void startUpdateInstall(const UpdateInfo& info);
};