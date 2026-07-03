#pragma once

#include <QSqlQuery>
#include <QVariant>
#include <cmath>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers for round-tripping "unset" doubles (NaN sentinel) through
// SQLite. Unset values are stored as NULL; NULL (or a missing column) reads
// back as NaN instead of QVariant's default 0.0, which would otherwise turn
// "no error stored" into "error of exactly zero".
// ─────────────────────────────────────────────────────────────────────────────
namespace SqlValue {

// Bind value: NaN → NULL, everything else as-is.
inline QVariant fromDouble(double v)
{
    return std::isnan(v) ? QVariant() : QVariant(v);
}

// Read by column name: NULL / missing column → NaN.
inline double toDoubleOrNaN(const QSqlQuery& q, const QString& col)
{
    const QVariant v = q.value(col);
    if (!v.isValid() || v.isNull())
        return std::numeric_limits<double>::quiet_NaN();
    return v.toDouble();
}

// Read by column index (from QSqlRecord::indexOf): −1 or NULL → NaN.
inline double toDoubleOrNaN(const QSqlQuery& q, int idx)
{
    if (idx < 0)
        return std::numeric_limits<double>::quiet_NaN();
    const QVariant v = q.value(idx);
    if (!v.isValid() || v.isNull())
        return std::numeric_limits<double>::quiet_NaN();
    return v.toDouble();
}

} // namespace SqlValue
