#pragma once

#include "core/InstrumentMode.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <utility>
#include <vector>

// The spectrum panel's "Panels" view: an overview of the whole spectrum with
// cut-outs below it. This is the part that does not need a widget: which
// cut-outs a spectrum gets, the straight-line continuum for spectra without
// a fitted one, and each cut-out's automatic flux range.

namespace astra::spectra {

/// ASTRA's own panel set, used for every instrument mode that has none of its
/// own: the Balmer jump (in flux), the Paschen region and eight line
/// cut-outs (normalized).
QVector<SpectrumViewPanel> builtInViewPanels();

/// The panels a spectrum of this mode shows: the mode's own list when it has
/// one, the built-in set otherwise.
QVector<SpectrumViewPanel> viewPanelsFor(const InstrumentMode* mode);

/// A spectral line (air wavelength, Angstrom). Within a few Angstrom of a
/// line core the data always count for a cut-out's flux range, so a deep
/// core is never clipped even where the fit masked it.
struct SpectralLine {
    QString label;
    double  wavelength = 0.0;
};
const QVector<SpectralLine>& knownLines();

/// Windows wider than this are drawn two columns wide and are shown even
/// where the fit used none of their pixels; narrower ones are line cut-outs.
constexpr double kWidePanelAngstrom = 300.0;

/// One cut-out this spectrum actually gets, with its pixel range [i0, i1).
struct PlannedPanel {
    int     source = -1;          ///< index into the panel list
    QString label;
    double  wlLow  = 0.0;
    double  wlHigh = 0.0;
    SpectrumViewPanel::Scale scale = SpectrumViewPanel::Scale::Auto;
    int     i0 = 0;
    int     i1 = 0;
    bool    wide = false;
};

/// The cut-outs of `panels` that this spectrum can show, in list order. A
/// window needs at least 8 pixels spanning 60 % of it; a line cut-out the fit
/// left out entirely (`used` all zero there) is skipped too, as a He I line in
/// a dichroic gap would be. `wl` must be ascending; `used` may be empty (no
/// fit mask: every pixel counts as used).
QVector<PlannedPanel> planPanels(const std::vector<double>& wl,
                                 const std::vector<uint8_t>& used,
                                 const QVector<SpectrumViewPanel>& panels);

/// Straight line through the medians of the outer 15 % of [i0, i1) on both
/// sides: a stand-in continuum for normalizing a cut-out of a spectrum that
/// has no fitted one. Returns i1 - i0 values (NaN when a side has no data).
std::vector<double> edgeLine(const std::vector<double>& wl,
                             const std::vector<double>& flux, int i0, int i1);

/// A cut-out's automatic flux range: the 0.5 to 99.5 percentiles of the used
/// data, widened to the whole model and to every data point within `half`
/// Angstrom of a line core, plus a 6 % margin. Spikes in excluded pixels set
/// nothing. `model` and `used` may be empty. {NaN, NaN} without data.
std::pair<double, double> panelYRange(const std::vector<double>& x,
                                      const std::vector<double>& flux,
                                      const std::vector<uint8_t>& used,
                                      const std::vector<double>& model,
                                      const std::vector<double>& cores,
                                      double half);

}   // namespace astra::spectra
