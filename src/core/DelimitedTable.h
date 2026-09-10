#pragma once

// Reading the delimited text files people actually hand to ASTRA.
//
// Radial-velocity tables, photometry exports and hand-edited catalogues arrive
// as CSV, TSV, semicolon-separated or whitespace-aligned text, usually without
// saying which. Two import paths had byte-identical copies of the sniffing and
// splitting code, which is how one of them could gain quote handling without
// the other.
//
// This is deliberately not a general CSV library: it handles the one dialect
// that shows up here, which is quoted fields with no escaped quotes inside.

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace DelimitedTable {

/// Guesses the delimiter of a line by counting candidates: comma, tab,
/// semicolon, or runs of whitespace. Ties go to the comma, then tab, then
/// semicolon, and whitespace only wins outright, which keeps a whitespace-
/// aligned file with a comma in a name from being read as CSV.
QChar detectDelimiter(const QString& line);

/// Splits one line on `delimiter`, honouring double-quoted fields and trimming
/// each result. A delimiter of ' ' means "any run of whitespace", so repeated
/// spaces do not produce empty columns.
QStringList splitLine(const QString& line, QChar delimiter);

/// A parsed comma-separated table: a header index and the rows under it.
struct Csv {
    QMap<QString, int> columns;      // lower-cased header name -> column index
    QList<QStringList> rows;

    bool isEmpty() const { return rows.isEmpty(); }

    int col(const QString& name) const {
        return columns.value(name.toLower(), -1);
    }

    /// Cell as text, or an empty string when the column or row is absent.
    QString value(int row, const QString& name) const {
        const int c = col(name);
        if (c < 0 || row < 0 || row >= rows.size()) return QString();
        const QStringList& r = rows.at(row);
        return c < r.size() ? r.at(c) : QString();
    }

    /// Cell as a number, or NaN when it is absent, blank or not a number.
    double number(int row, const QString& name) const;
};

/// Parses a comma-separated body with a header row, honouring quoted fields
/// and skipping blank and '#'-commented lines. Embedded newlines inside a
/// quoted field are not supported; no service ASTRA queries emits them.
Csv parseCsv(const QByteArray& body);

}   // namespace DelimitedTable
