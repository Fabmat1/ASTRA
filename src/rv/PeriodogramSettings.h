#ifndef PERIODOGRAMSETTINGS_H
#define PERIODOGRAMSETTINGS_H

#include "rv/Periodogram.h"

#include <QHash>
#include <QString>
#include <optional>

/// What a periodogram was computed with, as the user entered it (0 = auto).
/// The periodogram store keeps these after it deletes the periodogram itself,
/// so computing it again needs no re-entry.
///
/// Light-curve periodograms use every field; RV periodograms only the grid
/// (period range, N, oversample).
struct PeriodogramSettings {
    double minPeriod  = 0.0;
    double maxPeriod  = 0.0;
    int    nSamples   = 0;
    double oversample = 20.0;

    Periodogram::Backend backend = Periodogram::Backend::LombScargle;
    int                  fpwBins = Periodogram::kFPWDefaultBins;

    int minPoints = -1;   ///< series with fewer points are skipped; -1 = not recorded

    /// The shared pre-whitening options (`enabled` unused); the per-series
    /// toggles are in `series`.
    std::optional<Periodogram::PreWhitenConfig> preWhiten;

    struct SeriesFlags {
        bool enabled   = true;
        bool preWhiten = false;
    };
    QHash<QString, SeriesFlags> series;   ///< by "source::filter"

    QString toJson() const;
    static std::optional<PeriodogramSettings> fromJson(const QString& json);

    /// Settings that reproduce `grid`'s period range and size: all that is
    /// known of a periodogram stored before its settings were recorded.
    static PeriodogramSettings fromGrid(const Periodogram::Grid& grid);
};

#endif
