#pragma once

// Reading the shape of an RV table the user hands to ASTRA: which line is the
// header, which column is which, and which time scale the epochs are on.
//
// RV tables arrive from papers, colleagues' notebooks and pipeline dumps, with
// no fixed layout. A pandas export has a blank first header cell, a VizieR one
// a '#'-commented header, a hand-typed one none at all, and column names
// routinely contradict the values under them (a column called "mjd" holding
// full barycentric Julian dates was the case that motivated this). Every guess
// here is only a starting point the import dialog shows and lets the user
// override; the point is that a plain table imports without touching anything.

#include "core/Time.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace RVTableGuess {

/// Parses a numeric cell. Accepts a decimal comma ("12,5") when the cell has
/// no other separator, so semicolon-separated European exports read too.
/// Blank cells, "nan", "--" and the like report !ok.
double parseNumber(const QString& cell, bool* ok);

/// Parses an ISO 8601 date or date-time ("2023-05-01", "2023-05-01T03:22:11",
/// optional fractional seconds and trailing 'Z') as UTC and returns its MJD.
/// Returns false when the cell is not such a date.
bool parseIsoDateMjd(const QString& cell, double* mjd);

/// True when `first` is a header line for the data rows that follow it: it
/// carries a non-numeric text cell in a column that holds numbers below.
/// Comparing against the data (rather than asking "is anything non-numeric")
/// keeps a data row whose first cell is a file name from reading as a header.
bool looksLikeHeader(const QStringList& first,
                     const std::vector<QStringList>& following);

/// Column indices chosen for each role, -1 when none.
struct ColumnGuess {
    int time      = -1;
    int rv        = -1;
    int error     = -1;
    int sysError  = -1;
    int instrument= -1;
    int component = -1;
};

/// Maps columns to roles from their names first, then from their values:
/// a table whose names say nothing still gets an epoch column (the first
/// numeric column with dates in it) and an RV column (the next numeric one),
/// and a headerless one an error column when the column after the RV holds
/// only positive numbers. No column is given two roles.
/// `hasHeader` false means the names are placeholders and say nothing.
ColumnGuess guessColumns(const QStringList& names,
                         const std::vector<QStringList>& rows,
                         bool hasHeader);

/// The scale and epoch offset a time column is read with.
struct ScaleGuess {
    TimeScale scale  = TimeScale::MJD;
    double    offset = 0.0;    ///< added to every value before reading it
    bool      iso    = false;  ///< values are ISO dates, read as UTC MJD
    /// Set when the values overruled the column name or an offset was
    /// inferred rather than stated, so the dialog can say so.
    QString   note;
};

/// Guesses the scale of a time column from its name, the file's '#' lines
/// and the values themselves. The name wins when its scale is plausible for
/// the values; otherwise the magnitude decides: full Julian dates are read as
/// BJD, 3e4..1e6 as MJD (or a name's JD scale reduced by 2400000), and
/// 1000..3e4 as BJD reduced by 2450000.
ScaleGuess guessTimeScale(const QString& columnName,
                          const QStringList& metadataLines,
                          const std::vector<QStringList>& rows,
                          int column);

/// The scale a column name states outright, or Unknown.
TimeScale scaleFromName(const QString& columnName);

}   // namespace RVTableGuess
