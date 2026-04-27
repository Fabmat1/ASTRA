#pragma once

#include "DetailPanel.h"

class QPushButton;
class QVBoxLayout;
class QCustomPlot;
class QCheckBox;
class RVFit;

class RVPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit RVPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;
    
    void setDisplayedFit(std::shared_ptr<RVFit> fit);

private slots:
    void onToggleFolded();

private:
    void setupUi();
    void populate();

    QPushButton* _toggleButton  = nullptr;
    QWidget*     _content       = nullptr;
    QVBoxLayout* _contentLayout = nullptr;
    bool         _folded        = false;
    
    std::shared_ptr<RVFit> _displayedFit;

    QCheckBox* _showFlaggedCheck = nullptr;
    bool _showFlagged = false;
};