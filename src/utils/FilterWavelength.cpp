#include "FilterWavelength.h"
#include <QHash>

double FilterWavelength::lookupNm(const QString &filter) {
    static const QHash<QString, double> table = {
        // Gaia
        {"G", 621.7},
        {"BP", 518.6},
        {"RP", 783.0},
        // TESS
        {"T", 786.5},
        {"TESS", 786.5},
        // ZTF / PS1 / SDSS
        {"g", 464.0},
        {"r", 658.0},
        {"i", 806.0},
        {"z", 900.0},
        {"y", 962.0},
        {"zg", 464.0},
        {"zr", 648.0},
        {"zi", 806.0},
        {"u", 354.0},
        // ATLAS
        {"c", 533.0},
        {"o", 679.0},
        // Johnson
        {"U", 365.0},
        {"B", 445.0},
        {"V", 551.0},
        {"R", 658.0},
        {"I", 806.0},
        // 2MASS
        {"J", 1220.0},
        {"H", 1630.0},
        {"K", 2190.0},
        {"Ks", 2150.0},
        // BlackGEM (filter labels)
        {"q", 590.0},
    };
    auto it = table.constFind(filter);
    if (it != table.constEnd())
        return it.value();
    // try case-insensitive fallback for short keys
    for (auto j = table.constBegin(); j != table.constEnd(); ++j)
        if (j.key().compare(filter, Qt::CaseInsensitive) == 0)
            return j.value();
    return 0.0;
}