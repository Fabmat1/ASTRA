#include "DetailPanelFactory.h"
#include "SummaryPanel.h"
#include "RVPanel.h"
#include "LCPanel.h"
#include "SpectraPanel.h"

DetailPanel* DetailPanelFactory::create(AppSettings::DetailPanel which,
                                         const DetailPanel::Context& ctx,
                                         QWidget* parent)
{
    using P = AppSettings::DetailPanel;
    switch (which) {
        case P::Summary:        return new SummaryPanel(ctx, parent);
        case P::RadialVelocity: return new RVPanel(ctx, parent);
        case P::LightCurve:     return new LCPanel(ctx, parent);
        case P::Spectra:        return new SpectraPanel(ctx, parent);
        case P::None:           return nullptr;
    }
    return nullptr;
}