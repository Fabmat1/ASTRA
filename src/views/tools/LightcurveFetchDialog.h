#pragma once

#include <QDialog>
#include <memory>

class QTabWidget;

class Star;
class DatabaseManager;
class ApplicationController;
class LCPanel;
class PeriodogramPanel;

class LightcurveFetchDialog : public QDialog
{
    Q_OBJECT

public:
    LightcurveFetchDialog(std::shared_ptr<Star> star,
                          DatabaseManager*       dbm,
                          ApplicationController* controller,
                          const QString&         projectId,
                          QWidget*               parent = nullptr);
    ~LightcurveFetchDialog() override;

    LCPanel* lcPanel() const { return _lcPanel; }

private:
    void setupUi();
    QWidget* buildViewerTab();
    QWidget* buildPeriodogramTab();   // populated in Task 2
    QWidget* buildFetchTab();         // populated in Task 3
    QWidget* buildFitTab();           // populated in Task 4

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm        = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;

    QTabWidget* _tabs    = nullptr;
    LCPanel*    _lcPanel = nullptr;
    PeriodogramPanel* _periodogramPanel = nullptr;
};