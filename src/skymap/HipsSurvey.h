#pragma once

// The image surveys the sky map can show, and the HiPS metadata they publish.
//
// Every survey lists all the servers known to carry it. CDS (alasky) is the
// canonical host for most of them but is not the only one: ESA, IRSA, MAST,
// CASDA and others mirror the popular surveys, and several X-ray and radio
// surveys live only at their own institute. The tile loader probes every
// mirror and uses whichever answers first, so one host being down costs
// nothing as long as another carries the survey.
//
// The mirror lists were taken from the CDS MocServer registry
// (hips_service_url*) and each non-CDS one was checked on 2026-11-19.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace SkyMap {

enum class Regime { Gamma, XRay, UV, Optical, Infrared, Radio };

QString regimeName(Regime r);

struct HipsSurvey {
    QString     id;           // IVOA / CDS identifier, e.g. "CDS/P/DSS2/color"
    QString     title;        // short name for the selector
    Regime      regime;
    QString     band;         // wavelength / energy range, shown as a hint
    QStringList mirrors;      // base URLs, no trailing slash
};

/// All surveys in display order, high energy to radio.
const QList<HipsSurvey>& hipsSurveys();
/// The survey with this id, or nullptr.
const HipsSurvey* findHipsSurvey(const QString& id);

/// The subset of a HiPS `properties` file the renderer needs.
struct HipsProperties {
    bool    valid     = false;
    bool    galactic  = false;   // hips_frame = galactic (else equatorial)
    int     maxOrder  = -1;      // hips_order: deepest tile order
    int     minOrder  = 3;       // hips_order_min, never below 3 (Allsky order)
    int     tileWidth = 512;     // pixels per tile side, a power of two
    QString tileExt;             // "jpg" or "png": the format we will request
    QString title;               // obs_title
    QString copyright;           // obs_copyright
};

/// Parse a `properties` file. `valid` is false when it lacks an order or a
/// tile format the image loader can decode (FITS-only surveys are skipped).
HipsProperties parseHipsProperties(const QByteArray& text);

/// Path of a tile below the survey root: "Norder<k>/Dir<d>/Npix<n>.<ext>".
QString hipsTilePath(int order, qint64 npix, const QString& ext);

} // namespace SkyMap
