#pragma once

#include "app/ui/panels/DetailPanel.h"      
#include "app/AppSettings.h"

class DetailPanelFactory {
public:
    static DetailPanel* create(AppSettings::DetailPanel which,
                               const DetailPanel::Context& ctx,
                               QWidget* parent = nullptr,
                               bool deferPopulate = false);
};