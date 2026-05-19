#pragma once

#include <QDialog>

class AppSettings;
class QListWidget;
class QStackedWidget;
class QLineEdit;
class DetailGridEditor;
class QListWidget;
class QPlainTextEdit;
class QLabel;

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

    AppSettings*   _settings;

    QListWidget*   _topicList = nullptr;
    QStackedWidget* _pages    = nullptr;

    // General page
    QLineEdit*     _isisEdit  = nullptr;

    // Star detail page
    DetailGridEditor* _gridEditor = nullptr;

    QListWidget* _gridPathsList = nullptr;

    QLineEdit*       _lcqPythonEdit    = nullptr;
    QLineEdit*       _lcqScriptEdit    = nullptr;
    QLineEdit*       _atlasTokenEdit   = nullptr;
    QLineEdit*       _blackgemEdit     = nullptr;
    QLabel*          _lcqTestResult    = nullptr;
};