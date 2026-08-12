#include "ElementAbundances.h"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace astra::elements {

namespace {

// Solar reference values are the "fractional particle number relative to all
// particles" table in the grids' abundances.dat (Nieva & Przybilla 2012 /
// Asplund et al. 2009 / Jeffery et al. 2015). Ordered by atomic number.
const QVector<ElementInfo>& table()
{
    static const QVector<ElementInfo> t = {
        { "C",  "C",    6,  12.011, -4.84, "c"  },
        { "N",  "N",    7,  14.007, -4.44, "n"  },
        { "O",  "O",    8,  15.999, -4.34, "o"  },
        { "NE", "Ne",  10,  20.180, -5.04, "ne" },
        { "NA", "Na",  11,  22.990, -5.80, "na" },
        { "MG", "Mg",  12,  24.305, -5.24, "mg" },
        { "AL", "Al",  13,  26.982, -6.34, "al" },
        { "SI", "Si",  14,  28.085, -5.54, "si" },
        { "P",  "P",   15,  30.974, -6.63, "p"  },
        { "S",  "S",   16,  32.060, -5.54, "s"  },
        { "AR", "Ar",  18,  39.948, -5.64, "ar" },
        { "CA", "Ca",  20,  40.078, -5.70, "ca" },
        { "TI", "Ti",  22,  47.867, -6.09, "ti" },
        { "V",  "V",   23,  50.942, -7.11, "v"  },
        { "CR", "Cr",  24,  51.996, -5.40, "cr" },
        { "MN", "Mn",  25,  54.938, -5.61, "mn" },
        { "FE", "Fe",  26,  55.845, -4.54, "fe" },
        { "CO", "Co",  27,  58.933, -6.05, "co" },
        { "NI", "Ni",  28,  58.693, -4.82, "ni" },
        { "GE", "Ge",  32,  72.630, -7.39, "ge" },
        { "SR", "Sr",  38,  87.620, -7.17, "sr" },
        { "Y",  "Y",   39,  88.906, -7.83, "y"  },
        { "ZR", "Zr",  40,  91.224, -7.46, "zr" },
        { "SN", "Sn",  50, 118.710, -8.00, "sn" },
    };
    return t;
}

const std::unordered_map<QString, int>& symbolIndex()
{
    static const std::unordered_map<QString, int> m = [] {
        std::unordered_map<QString, int> out;
        const auto& t = table();
        for (int i = 0; i < t.size(); ++i) out.emplace(t[i].symbol, i);
        return out;
    }();
    return m;
}

const std::unordered_map<QString, int>& suffixIndex()
{
    static const std::unordered_map<QString, int> m = [] {
        std::unordered_map<QString, int> out;
        const auto& t = table();
        for (int i = 0; i < t.size(); ++i) out.emplace(t[i].dbSuffix, i);
        return out;
    }();
    return m;
}

} // namespace

const QVector<ElementInfo>& all() { return table(); }

int count() { return static_cast<int>(table().size()); }

int indexOfSymbol(const QString& symbol)
{
    const auto& m = symbolIndex();
    auto it = m.find(symbol.trimmed().toUpper());
    return it == m.end() ? -1 : it->second;
}

const ElementInfo* bySymbol(const QString& symbol)
{
    const int i = indexOfSymbol(symbol);
    return i < 0 ? nullptr : &table()[i];
}

int indexOfDbSuffix(const QString& suffix)
{
    const auto& m = suffixIndex();
    auto it = m.find(suffix.trimmed().toLower());
    return it == m.end() ? -1 : it->second;
}

double toSolarRelative(int elementIndex, double value)
{
    if (elementIndex < 0 || elementIndex >= count() || std::isnan(value))
        return std::numeric_limits<double>::quiet_NaN();
    return value - table()[elementIndex].solarLogN;
}

bool isSwitchedOff(double value)
{
    return !std::isnan(value) && value >= 10.0;
}

} // namespace astra::elements
