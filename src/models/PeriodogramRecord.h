#ifndef PERIODOGRAMRECORD_H
#define PERIODOGRAMRECORD_H

#include <QString>
#include <QDateTime>
#include <memory>
#include <vector>
#include "fitting/Periodogram.h"

class PeriodogramRecord
{
public:
    PeriodogramRecord() = default;

    QString getId() const { return _id; }
    void    setId(const QString& id) { _id = id; }

    void    setDataFile(const QString& f) { _dataFile = f; }
    QString getDataFile() const { return _dataFile; }

    bool saveDataToFile(const QString& filepath);
    bool loadDataFromFile(const QString& filepath);

    QString   source;        // empty for a future "combined" record (unused now)
    QString   filter;        // empty if the series has no filter dimension
    quint64   dataHash = 0;  // hash of (t, y, e)
    quint64   gridHash = 0;  // hash of (f0, df, Nf)
    QDateTime computedAt;

    Periodogram::Result result;

private:
    QString _id;
    QString _dataFile;
};

namespace PeriodogramUtils {
/// Weighted sum over all per-series records for `source`.
Periodogram::Result combineForSource(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
    const QString& source);

/// Geometric-mean product over per-source aggregates, in the source order of
/// first appearance in `records`.
Periodogram::Result combineForStar(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records);
}

#endif