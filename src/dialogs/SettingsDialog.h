#pragma once

#include <QDialog>

class AppSettings;
class QListWidget;
class QStackedWidget;
class QLineEdit;
class DetailGridEditor;
class QListWidget;

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

    AppSettings*   _settings;

    QListWidget*   _topicList = nullptr;
    QStackedWidget* _pages    = nullptr;

    // General page
    QLineEdit*     _isisEdit  = nullptr;

    // Star detail page
    DetailGridEditor* _gridEditor = nullptr;

    QListWidget* _gridPathsList = nullptr;
};