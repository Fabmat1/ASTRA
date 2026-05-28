#include "ClaretFilter.h"
#include <QHash>

QString ClaretFilter::canonical(const QString &f) {
    static const QHash<QString, QString> table = {
        // TESS
        {"T", "TESS"},
        {"TESS", "TESS"},
        // Kepler
        {"Kp", "Kepler"},
        {"Kepler", "Kepler"},
        {"K2", "Kepler"},
        // SDSS / ZTF / Pan-STARRS (all close enough to use SDSS tables)
        {"u", "SDSS-u"},
        {"g", "SDSS-g"},
        {"zg", "SDSS-g"},
        {"r", "SDSS-r"},
        {"zr", "SDSS-r"},
        {"i", "SDSS-i"},
        {"zi", "SDSS-i"},
        {"z", "SDSS-z"},
        // Johnson-Cousins
        {"U", "Johnson-U"},
        {"B", "Johnson-B"},
        {"V", "Johnson-V"},
        {"Rc", "Johnson-R"},
        {"R", "Johnson-R"},
        {"Ic", "Johnson-I"},
        {"I", "Johnson-I"},
        // ATLAS (closest match)
        {"c", "SDSS-g"},
        {"o", "SDSS-r"},
        // Gaia (no native Claret table — fall back to broadband V)
        {"G", "Johnson-V"},
        {"BP", "Johnson-B"},
        {"RP", "Johnson-R"},
    };
    auto it = table.constFind(f);
    return (it != table.constEnd()) ? it.value() : QString();
}