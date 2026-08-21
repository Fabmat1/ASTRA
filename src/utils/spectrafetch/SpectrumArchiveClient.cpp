// src/utils/spectrafetch/SpectrumArchiveClient.cpp

#include "SpectrumArchiveClient.h"

namespace SpecFetch {

QString archiveKey(Archive a) {
    switch (a) {
    case Archive::LamostLRS:   return QStringLiteral("lamost-lrs");
    case Archive::LamostMRS:   return QStringLiteral("lamost-mrs");
    case Archive::SdssOptical: return QStringLiteral("sdss");
    case Archive::EsoPhase3:   return QStringLiteral("eso");
    case Archive::MastSSAP:    return QStringLiteral("mast");
    case Archive::Apogee:      return QStringLiteral("apogee");
    }
    return QStringLiteral("unknown");
}

QString archiveDisplayName(Archive a) {
    switch (a) {
    case Archive::LamostLRS:   return QStringLiteral("LAMOST LRS");
    case Archive::LamostMRS:   return QStringLiteral("LAMOST MRS");
    case Archive::SdssOptical: return QStringLiteral("SDSS optical");
    case Archive::EsoPhase3:   return QStringLiteral("ESO Phase 3");
    case Archive::MastSSAP:    return QStringLiteral("MAST (HST/IUE/FUSE)");
    case Archive::Apogee:      return QStringLiteral("SDSS APOGEE");
    }
    return QStringLiteral("Unknown");
}

}   // namespace SpecFetch

QUrl SpectrumArchiveClient::resolveDownloadUrl(
    const SpecFetch::RemoteSpectrum& r, QNetworkAccessManager* nam,
    QString* error) {
    Q_UNUSED(nam);
    if (error) error->clear();
    return r.downloadUrl;
}
