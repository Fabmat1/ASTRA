// src/utils/spectrafetch/MastArchiveClient.h
//
// MAST reduced spectra (HST, IUE, FUSE, ...). Discovery via the MAST CAOM
// TAP service (ivoa.obscore), in chunks of stars per query: the service will
// not plan CONTAINS(POINT, CIRCLE) against its spatial index and 504s on a
// single cone, so the query asks for OR-ed s_ra/s_dec boxes and the circles
// are cut out client-side (the classic archive.stsci.edu SSAP endpoint no
// longer returns rows). Downloads are the archive's calibrated FITS products,
// preferring the VO-normalized "*_vo.fits" spectra where offered.

#ifndef MASTARCHIVECLIENT_H
#define MASTARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class MastArchiveClient : public SpectrumArchiveClient {
public:
    SpecFetch::Archive archive() const override {
        return SpecFetch::Archive::MastSSAP;
    }
    QString displayName() const override {
        return QStringLiteral("MAST (HST/IUE/FUSE)");
    }
    QString hostKey() const override {
        return QStringLiteral("archive.stsci.edu");
    }

    QList<SpecFetch::RemoteSpectrum> discover(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt,
        QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel,
        QString* error) override;

    // Widened past the caller's radius: see kMinRadiusArcsec in the .cpp.
    double searchRadiusArcsec(
        const SpecFetch::ArchiveOptions& opt) const override;

    std::vector<SpecFetch::ParsedSpectrum> parse(
        const QString& localPath,
        const SpecFetch::RemoteSpectrum& r,
        const SpecFetch::ArchiveOptions& opt,
        QString* error) override;

    // UV and HST products are on vacuum wavelengths throughout.
    bool deliversVacuumWavelengths() const override { return true; }
    SpecFetch::Frame declaredFrame(
        const SpecFetch::RemoteSpectrum& r) const override {
        // HST x1d products carry the HELCORR switch, which readFrameInfo()
        // reads directly; this covers the coadds that do not. calstis/calcos
        // and CalFUSE v3 both shift onto a heliocentric scale, and NEWSIPS
        // does the same for IUE high-dispersion images (its low-dispersion
        // ones are ~6 A per resolution element, where 30 km/s is 2% of a
        // pixel). HUT and EUVE are left alone - nothing states their frame.
        if (r.collection.compare(QStringLiteral("HST"),
                                 Qt::CaseInsensitive) == 0 ||
            r.collection.compare(QStringLiteral("FUSE"),
                                 Qt::CaseInsensitive) == 0 ||
            r.collection.compare(QStringLiteral("IUE"),
                                 Qt::CaseInsensitive) == 0)
            return SpecFetch::Frame::Heliocentric;
        return SpecFetch::Frame::Unknown;
    }

    /// Missions offered in the setup UI (obs_collection values).
    static QStringList knownMissions();
};

#endif   // MASTARCHIVECLIENT_H
