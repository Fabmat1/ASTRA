#pragma once

// Ids for rows that exist at most once per parent.
//
// A star has one photometry row and (when first touched) one RV curve; a
// photometry row has one lightcurve per source; a curve has one point per
// spectrum and component. Rows like these are created on demand, so two
// ASTRA installs working on copies of one database each create "the" row for
// the same parent. With random ids those are two rows that a merge then has
// to reconcile. Derived from the parent's id they are the same row, and a
// merge is a no-op.
//
// Only rows created from here on get such ids. Rows that already exist keep
// the id they have: every caller looks for an existing row first.

#include <QString>
#include <QUuid>

namespace astra::ids {

namespace detail {
// Namespace for every id below. Never change it: the ids are stored.
inline const QUuid kNamespace{QStringLiteral("5b1f6c1e-9a3d-4e0b-8c57-2f4a7d9e6b10")};

inline QString derive(const QString& kind, const QString& key)
{
    return QUuid::createUuidV5(kNamespace, kind + QLatin1Char('|') + key)
        .toString(QUuid::WithoutBraces);
}
}   // namespace detail

/// The photometry row of a star.
inline QString photometryIdFor(const QString& starId)
{
    return detail::derive(QStringLiteral("photometry"), starId);
}

/// The RV curve created for a star that has none.
inline QString rvCurveIdFor(const QString& starId)
{
    return detail::derive(QStringLiteral("rv_curve"), starId);
}

/// The lightcurve row of one source ("TESS", "ZTF", ...) under a photometry row.
inline QString lightcurveIdFor(const QString& photometryId, const QString& source)
{
    return detail::derive(QStringLiteral("lightcurve"),
                          photometryId + QLatin1Char('|') + source);
}

/// A photometric point, keyed by its content (the caller's dedup key).
inline QString photometricPointIdFor(const QString& photometryId, const QString& contentKey)
{
    return detail::derive(QStringLiteral("photometric_point"),
                          photometryId + QLatin1Char('|') + contentKey);
}

/// The RV point a curve holds for one spectrum and component.
inline QString rvPointIdFor(const QString& curveId, const QString& spectrumId, int component)
{
    return detail::derive(QStringLiteral("rv_point"),
                          curveId + QLatin1Char('|') + spectrumId + QLatin1Char('|') +
                              QString::number(component));
}

}   // namespace astra::ids
