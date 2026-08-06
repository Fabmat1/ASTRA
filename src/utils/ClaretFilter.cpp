#include "ClaretFilter.h"
#include <QHash>

namespace {

struct Mapping {
    QString band;
    // See ClaretFilter::isNative().
    bool native = true;
};

const QHash<QString, Mapping> &mappings() {
    static const QHash<QString, Mapping> table = {
        // TESS
        {"T", {"TESS"}},
        {"TESS", {"TESS"}},
        // Kepler
        {"Kp", {"Kepler"}},
        {"Kepler", {"Kepler"}},
        {"K2", {"Kepler"}},
        // SDSS, and the Sloan-system surveys that share those filters
        {"u", {"SDSS-u"}},
        {"g", {"SDSS-g"}},
        {"zg", {"SDSS-g"}},
        {"r", {"SDSS-r"}},
        {"zr", {"SDSS-r"}},
        {"i", {"SDSS-i"}},
        {"zi", {"SDSS-i"}},
        {"z", {"SDSS-z"}},
        // Johnson-Cousins
        {"U", {"Johnson-U"}},
        {"B", {"Johnson-B"}},
        {"V", {"Johnson-V"}},
        {"Rc", {"Johnson-R"}},
        {"R", {"Johnson-R"}},
        {"Ic", {"Johnson-I"}},
        {"I", {"Johnson-I"}},
        // ATLAS - wide filters with no counterpart in the tables
        {"c", {"SDSS-g", false}},
        {"o", {"SDSS-r", false}},
        // Gaia - no native Claret table
        {"G", {"Johnson-V", false}},
        {"BP", {"Johnson-B", false}},
        {"RP", {"Johnson-R", false}},
    };
    return table;
}

} // namespace

QString ClaretFilter::canonical(const QString &f) {
    return mappings().value(f).band;
}

bool ClaretFilter::isNative(const QString &f) {
    const auto it = mappings().constFind(f);
    return it != mappings().constEnd() && it->native;
}
