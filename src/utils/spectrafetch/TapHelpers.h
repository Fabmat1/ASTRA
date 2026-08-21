// src/utils/spectrafetch/TapHelpers.h
//
// Shared helpers for the archive clients: TAP_UPLOAD positional crossmatch
// payloads, synchronous upload queries against an explicit TAP endpoint, and
// a small CSV response parser.

#ifndef SPECFETCH_TAPHELPERS_H
#define SPECFETCH_TAPHELPERS_H

#include "SpectrumArchiveTypes.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

class QNetworkAccessManager;

namespace SpecFetch {

// VOTable 1.3 with columns idx (int), ra, dec (double, deg) - the payload
// for a TAP_UPLOAD positional crossmatch. idx refers into `stars`.
QByteArray buildPositionVOTable(const std::vector<StarQuery>& stars);

// Synchronous TAP query with a VOTable upload against an explicit endpoint.
// The uploaded table is visible to the ADQL as TAP_UPLOAD.<uploadName>.
// Returns the raw body (`format` picks csv or votable); empty + *error set
// on failure.
QByteArray tapUploadQuery(QNetworkAccessManager* nam, const QString& url,
                          const QString& adql, const QByteArray& votable,
                          const QString& uploadName, const QString& format,
                          int timeoutMs, QString* error);

// Plain synchronous TAP form query (no upload) against an explicit endpoint.
QByteArray tapQuery(QNetworkAccessManager* nam, const QString& url,
                    const QString& adql, const QString& format, int timeoutMs,
                    QString* error);

// Minimal CSV parse: header row -> lower-cased name->column map, then rows.
// Handles quoted fields and embedded commas; good enough for TAP/SkyServer
// CSV output.
struct Csv {
    QMap<QString, int> columns;      // lower-cased header -> index
    QList<QStringList> rows;

    int col(const QString& name) const {
        return columns.value(name.toLower(), -1);
    }
    QString value(int row, const QString& name) const {
        const int c = col(name);
        if (c < 0 || row < 0 || row >= rows.size()) return QString();
        const QStringList& r = rows.at(row);
        return c < r.size() ? r.at(c) : QString();
    }
};

Csv parseCsv(const QByteArray& body);

// Split a work list into chunks of at most `chunkSize`.
template <typename T>
std::vector<std::vector<T>> chunked(const std::vector<T>& items,
                                    size_t chunkSize) {
    std::vector<std::vector<T>> out;
    for (size_t i = 0; i < items.size(); i += chunkSize) {
        const size_t end = std::min(items.size(), i + chunkSize);
        out.emplace_back(items.begin() + i, items.begin() + end);
    }
    return out;
}

}   // namespace SpecFetch

#endif   // SPECFETCH_TAPHELPERS_H
