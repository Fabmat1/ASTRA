#pragma once

#include "DetailPanel.h"      
#include "utils/AppSettings.h"

class DetailPanelFactory {
public:
    static DetailPanel* create(AppSettings::DetailPanel which,
                               const DetailPanel::Context& ctx,
                               QWidget* parent = nullptr);
};