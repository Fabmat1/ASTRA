#include "skymap/HipsSurvey.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace SkyMap {

namespace {

const QString kAlasky    = QStringLiteral("https://alasky.cds.unistra.fr");
const QString kAlaskyBis = QStringLiteral("https://alaskybis.cds.unistra.fr");

// Both CDS hosts for a path below their root.
QStringList cds(const QString& path)
{
    return { kAlasky + '/' + path, kAlaskyBis + '/' + path };
}

} // namespace

QString regimeName(Regime r)
{
    switch (r) {
    case Regime::Gamma:    return QStringLiteral("Gamma ray");
    case Regime::XRay:     return QStringLiteral("X-ray");
    case Regime::UV:       return QStringLiteral("Ultraviolet");
    case Regime::Optical:  return QStringLiteral("Optical");
    case Regime::Infrared: return QStringLiteral("Infrared");
    case Regime::Radio:    return QStringLiteral("Sub-mm and radio");
    }
    return {};
}

const QList<HipsSurvey>& hipsSurveys()
{
    static const QList<HipsSurvey> surveys = {
        // ── Gamma ray ──────────────────────────────────────────────────────
        { "CDS/P/Fermi/color", "Fermi LAT color", Regime::Gamma, "0.1-300 GeV",
          QStringList{ "https://skies.esac.esa.int/FermiColor" } + cds("Fermi/Color") },
        { "ESAVO/P/INTEGRAL/color", "INTEGRAL IBIS color", Regime::Gamma, "20-100 keV",
          { "https://skies.esac.esa.int/Integral/color" } },

        // ── X-ray ──────────────────────────────────────────────────────────
        { "JAXA/P/SWIFT/BAT/Image", "Swift BAT 70-month", Regime::XRay, "14-195 keV",
          { "https://data.darts.isas.jaxa.jp/pub/HiPS/SWIFT-BAT_Image" } },
        { "cxc.harvard.edu/P/cda/hips/allsky/rgb", "Chandra ACIS color", Regime::XRay, "0.5-7 keV",
          QStringList{ "https://cdaftp.cfa.harvard.edu/cxc-hips" }
              + cds("CHANDRA/cxc.harvard.edu_P_cda_hips_allsky_rgb") },
        { "ESDC/P/XMM/EPIC-RGB", "XMM-Newton EPIC color", Regime::XRay, "0.2-12 keV",
          { "https://esdchips.esac.esa.int/XMM-Newton/EPIC-RGB" } },
        { "erosita/dr1/rate/rgb", "eROSITA-DE DR1 color", Regime::XRay, "0.2-2.3 keV",
          QStringList{ "https://erosita.mpe.mpg.de/erodat/static/hips/eRASS1_RGB_Rate_c010" }
              + cds("erosita/erosita_P_dr1_rate_rgb") },
        { "CDS/P/RASS", "ROSAT All-Sky Survey", Regime::XRay, "0.1-2.4 keV",
          cds("RASS") + QStringList{ "https://cade.irap.omp.eu/documents/Ancillary/4Aladin/RASS" } },

        // ── Ultraviolet ────────────────────────────────────────────────────
        { "CDS/P/GALEXGR6_7/color", "GALEX GR6/7 color", Regime::UV, "FUV + NUV",
          cds("GALEX/GALEXGR6_7_color")
              + QStringList{ "https://stpubdata.s3.us-east-1.amazonaws.com/mast/skybackgrounds/GALEXColor" } },
        { "CDS/P/GALEXGR6/AIS/color", "GALEX AIS color", Regime::UV, "FUV + NUV",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/GALEX/GR6-03-2014/AIS-Color",
                       "https://hips.china-vo.org/m/CDS_P_GALEXGR6_AIS_color" }
              + cds("GALEX/GR6-03-2014/AIS-Color") },
        { "nasa.heasarc/P/Swift/UVOT/int_U_UVW1_UVW2", "Swift UVOT color", Regime::UV, "U, UVW1, UVW2",
          { "https://skyview.gsfc.nasa.gov/surveys/uvot/int/color2" } },
        { "ESDC/P/XMM/OM-UV", "XMM-Newton OM UV", Regime::UV, "UVW2, UVM2, UVW1",
          { "https://esdchips.esac.esa.int/XMM-Newton/OM-UV" } },

        // ── Optical ────────────────────────────────────────────────────────
        { "CDS/P/DSS2/color", "DSS2 color", Regime::Optical, "B + R plates",
          QStringList{ "https://skies.esac.esa.int/DSSColor",
                       "https://irsa.ipac.caltech.edu/data/hips/CDS/DSS2/color",
                       "https://casda.csiro.au/hips/DSS2/color",
                       "https://stpubdata.s3.us-east-1.amazonaws.com/mast/skybackgrounds/DSSColor" }
              + cds("DSS/DSSColor") },
        { "CDS/P/DSS2/red", "DSS2 red", Regime::Optical, "R plates",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/DSS2/red" }
              + cds("DSS/DSS2Merged") },
        { "CDS/P/DSS2/blue", "DSS2 blue", Regime::Optical, "B plates",
          cds("DSS/DSS2-blue-XJ-S") },
        { "CDS/P/PanSTARRS/DR1/color-i-r-g", "Pan-STARRS DR1 color (i, r, g)", Regime::Optical, "g, r, i",
          QStringList{ "https://stpubdata.s3.us-east-1.amazonaws.com/mast/skybackgrounds/PanSTARRS" }
              + cds("Pan-STARRS/DR1/color-i-r-g") },
        { "CDS/P/PanSTARRS/DR1/color-z-zg-g", "Pan-STARRS DR1 color (z, g)", Regime::Optical, "g, z",
          cds("Pan-STARRS/DR1/color-z-zg-g") },
        { "CDS/P/SDSS9/color", "SDSS DR9 color", Regime::Optical, "g, r, i",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/SDSS/DR9/color",
                       "https://skies.esac.esa.int/SDSS9Color" }
              + cds("SDSS/DR9/color") },
        { "CDS/P/DESI-Legacy-Surveys/DR10/color", "DESI Legacy Surveys DR10", Regime::Optical, "g, r, i, z",
          cds("DESI-legacy-surveys/DR10/CDS_P_DESI-Legacy-Surveys_DR10_color") },
        { "CDS/P/DES-DR2/ColorIRG", "DES DR2 color", Regime::Optical, "g, r, i",
          cds("DES/DR2/CDS_P_DES-DR2_ColorIRG") },
        { "NOAO/P/DES/DR1/LIneA-color", "DES DR1 color (NOIRLab)", Regime::Optical, "g, r, i",
          { "https://datalab.noirlab.edu/HiPS/des_dr1" } },
        { "CDS/P/Skymapper/DR4/color", "SkyMapper DR4 color", Regime::Optical, "g, r, i",
          cds("Skymapper/DR4/CDS_P_Skymapper_DR4_color") },
        { "CDS/P/ZTF/DR7/color", "ZTF DR7 color", Regime::Optical, "g, r, i",
          cds("ZTF/DR7/CDS_P_ZTF_DR7_color") },
        { "CDS/P/DECaPS/DR2/color", "DECaPS DR2 color", Regime::Optical, "g, r, z",
          cds("DECaPS/DR2/CDS_P_DECaPS_DR2_color") },

        // ── Infrared ───────────────────────────────────────────────────────
        { "CDS/P/2MASS/color", "2MASS color", Regime::Infrared, "J, H, Ks",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/2MASS/Color" }
              + cds("2MASS/Color") },
        { "VOXASTRO/P/UKIDSS/JHK/color", "UKIDSS color", Regime::Infrared, "J, H, K",
          { "https://padc-web.obspm.fr/planeto/hips/VOXASTRO_P_UKIDSS_COLOR" } },
        { "VOXASTRO/P/VHS/JKs/color", "VISTA VHS color", Regime::Infrared, "J, Ks",
          { "https://padc-web.obspm.fr/planeto/hips/VOXASTRO_P_VHS_COLOR" } },
        { "CDS/P/allWISE/color", "AllWISE color", Regime::Infrared, "W1, W2, W4",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/AllWISE/RGB-W4-W2-W1",
                       "https://skies.esac.esa.int/AllWISEColor",
                       "https://hips.china-vo.org/m/CDS_P_allWISE_color" }
              + cds("AllWISE/RGB-W4-W2-W1") },
        { "CDS/P/unWISE/color-W2-W1W2-W1", "unWISE color", Regime::Infrared, "W1, W2",
          cds("unWISE/color-W2-W1W2-W1") },
        { "CDS/P/SPITZER/color", "Spitzer IRAC color", Regime::Infrared, "3.6, 4.5, 8.0 um",
          QStringList{ "https://casda.csiro.au/hips/SpitzerI1I2I4color" }
              + cds("Spitzer/SpitzerI1I2I4color") },
        { "CDS/P/AKARI/FIS/Color", "AKARI FIS color", Regime::Infrared, "65-160 um",
          cds("AKARI-FIS/ColorLSN60") },
        { "CDS/P/IRIS/color", "IRAS IRIS color", Regime::Infrared, "12-100 um",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/IRIS/color",
                       "https://skies.esac.esa.int/IRISColor" }
              + cds("IRISColor") },

        // ── Sub-mm and radio ───────────────────────────────────────────────
        { "CDS/P/PLANCK/R2/HFI/color", "Planck HFI color", Regime::Radio, "353-857 GHz",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/PLANCK/R2/HFI/color" }
              + cds("PLANCK/R2/HFI_Color_353_545_857") },
        { "CDS/P/PLANCK/R2/LFI/color", "Planck LFI color", Regime::Radio, "30-70 GHz",
          QStringList{ "https://irsa.ipac.caltech.edu/data/hips/CDS/PLANCK/R2/LFI/color",
                       "https://hips.china-vo.org/m/CDS_P_PLANCK_R2_LFI_color" }
              + cds("PLANCK/R2/LFI_Color_30_44_70") },
        { "CDS/P/NVSS", "NVSS", Regime::Radio, "1.4 GHz",
          QStringList{ "https://casda.csiro.au/hips/NVSS",
                       "https://hips.china-vo.org/m/CDS_P_NVSS" }
              + cds("NVSS/intensity") },
        { "CSIRO/P/RACS/low/I", "RACS-low", Regime::Radio, "888 MHz",
          QStringList{ "https://casda.csiro.au/hips/RACS/low/I" }
              + cds("CASDA/CSIRO_P_RACS_low_I") },
        { "CDS/P/SUMSS", "SUMSS", Regime::Radio, "843 MHz",
          QStringList{ "https://casda.csiro.au/hips/SUMSS" } + cds("SUMSS") },
        { "CDS/P/WENSS", "WENSS", Regime::Radio, "325 MHz",
          cds("WENSS") + QStringList{ "https://skies.esac.esa.int/WENSS" } },
        { "astron.nl/P/tgssadr", "TGSS ADR", Regime::Radio, "150 MHz",
          { "https://hips.astron.nl/ASTRON/P/tgssadr",
            "https://tgssadr.strw.leidenuniv.nl/hips" } },
        { "astron.nl/P/lotss_dr2_high", "LoTSS DR2", Regime::Radio, "144 MHz",
          { "https://hips.astron.nl/ASTRON/P/lotss_dr2_high" } },
        { "ov-gso/P/GLEAM/170-231", "GLEAM", Regime::Radio, "170-231 MHz",
          { "https://cade.irap.omp.eu/documents/Ancillary/4Aladin/GLEAM_170_231" } },
        { "ESAVO/P/Haslam408", "Haslam", Regime::Radio, "408 MHz",
          { "https://skies.esac.esa.int/Haslam/Haslam408_dsds_Remazeilles2014_mod_HiPS" } },
    };
    return surveys;
}

const HipsSurvey* findHipsSurvey(const QString& id)
{
    for (const HipsSurvey& s : hipsSurveys())
        if (s.id == id) return &s;
    return nullptr;
}

HipsProperties parseHipsProperties(const QByteArray& text)
{
    QHash<QString, QString> kv;
    for (const QByteArray& raw : text.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        if (!kv.contains(key))   // first value wins for repeated keys
            kv.insert(key, line.mid(eq + 1).trimmed());
    }

    HipsProperties p;
    bool ok = false;
    p.maxOrder = kv.value("hips_order").toInt(&ok);
    if (!ok) p.maxOrder = -1;
    const int minOrder = kv.value("hips_order_min").toInt(&ok);
    p.minOrder = ok ? std::max(3, minOrder) : 3;
    const int width = kv.value("hips_tile_width").toInt(&ok);
    p.tileWidth = (ok && width >= 8 && (width & (width - 1)) == 0) ? width : 512;
    p.galactic = kv.value("hips_frame").startsWith("gal", Qt::CaseInsensitive);
    p.title = kv.value("obs_title");
    p.copyright = kv.value("obs_copyright");

    // Prefer JPEG (smaller), then PNG. Some services write "jpg".
    const QStringList formats =
        kv.value("hips_tile_format").toLower().split(QRegularExpression("[\\s,]+"),
                                                       Qt::SkipEmptyParts);
    if (formats.contains("jpeg") || formats.contains("jpg")) p.tileExt = "jpg";
    else if (formats.contains("png"))                        p.tileExt = "png";

    p.valid = p.maxOrder >= 0 && !p.tileExt.isEmpty();
    if (p.valid) p.minOrder = std::min(p.minOrder, p.maxOrder);
    return p;
}

QString hipsTilePath(int order, qint64 npix, const QString& ext)
{
    const qint64 dir = (npix / 10000) * 10000;
    return QStringLiteral("Norder%1/Dir%2/Npix%3.%4").arg(order).arg(dir).arg(npix).arg(ext);
}

} // namespace SkyMap
