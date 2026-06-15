#include "DetailPanelFactory.h"
#include "SummaryPanel.h"
#include "RVPanel.h"
#include "LCPanel.h"
#include "SpectraPanel.h"

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