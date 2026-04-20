#pragma once

#include "DetailPanel.h"

class QPushButton;
class QVBoxLayout;
class QCustomPlot;

class RVPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit RVPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

private slots:
    void onToggleFolded();

private:
    void setupUi();
    void populate();

    QPushButton* _toggleButton  = nullptr;
    QWidget*     _content       = nullptr;
    QVBoxLayout* _contentLayout = nullptr;
    bool         _folded        = false;
};