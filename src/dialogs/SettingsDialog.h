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
class QSpinBox;
class QComboBox;
class QuantityLabel;

class SettingsDialog : public QDialog
{
public:
    /// `page` selects the topic to open on, e.g. "Numbers & Copying".
    explicit SettingsDialog(AppSettings* settings, QWidget* parent = nullptr,
                            const QString& page = QString());

private slots:
    void apply();

private:
    void setupUi();
    QWidget* createGeneralPage();
    QWidget* createStarDetailPage();
    QWidget* createNumberFormatPage();
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

    // Numbers & copying page
    QComboBox*     _copyContentCombo = nullptr;
    QComboBox*     _copyStyleCombo   = nullptr;
    QCheckBox*     _copyWrapMath     = nullptr;
    QCheckBox*     _copyIncludeName  = nullptr;
    QCheckBox*     _copyRound        = nullptr;
    QuantityLabel* _copyPreviewValue = nullptr;
    QLabel*        _copyPreviewText  = nullptr;
    void updateCopyPreview();

    QListWidget* _gridPathsList = nullptr;

    QLineEdit*       _lcqPythonEdit    = nullptr;
    QLineEdit*       _lcqScriptEdit    = nullptr;
    QLineEdit*       _atlasTokenEdit   = nullptr;
    QLineEdit*       _adsTokenEdit     = nullptr;
    QSpinBox*        _fitWorkersSpin   = nullptr;
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