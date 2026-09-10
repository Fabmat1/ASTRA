#include "app/ui/panels/DetailPanelFactory.h"
#include "app/ui/panels/SummaryPanel.h"
#include "rv/ui/RVPanel.h"
#include "lightcurve/ui/LCPanel.h"
#include "spectra/ui/SpectraPanel.h"

DetailPanel* DetailPanelFactory::create(AppSettings::DetailPanel which,
                                         const DetailPanel::Context& ctx,
                                         QWidget* parent,
                                         bool deferPopulate)
{
    using P = AppSettings::DetailPanel;
    switch (which) {
        case P::Summary:        return new SummaryPanel(ctx, parent, deferPopulate);
        case P::RadialVelocity: return new RVPanel(ctx, parent, deferPopulate);
        case P::LightCurve:     return new LCPanel(ctx, parent, deferPopulate);
        case P::Spectra:        return new SpectraPanel(ctx, parent, deferPopulate);
        case P::None:           return nullptr;
    }
    return nullptr;
}