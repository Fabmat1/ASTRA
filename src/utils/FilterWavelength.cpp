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
        // SVO λ_eff for the SDSS system - r and i are distinctly bluer than
        // the Johnson/Cousins R and I below, which they were once given.
        {"g", 464.0},
        {"r", 617.0},
        {"i", 752.0},
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
        // HST (pivot wavelengths; WFC3 values used where ACS shares a name)
        {"F218W", 222.3}, {"F225W", 235.9}, {"F275W", 270.4},
        {"F336W", 335.5}, {"F435W", 431.9}, {"F438W", 432.6},
        {"F475W", 477.3}, {"F555W", 530.8}, {"F606W", 588.7},
        {"F625W", 624.2}, {"F775W", 764.4}, {"F814W", 802.4},
        {"F850LP", 901.7},
        {"F105W", 1055.2}, {"F110W", 1153.4}, {"F125W", 1248.6},
        {"F140W", 1392.3}, {"F160W", 1536.9},
        {"F115LP", 140.6}, {"F125LP", 143.8}, {"F140LP", 152.7},
        {"F150LP", 161.1}, {"F165LP", 175.8},
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