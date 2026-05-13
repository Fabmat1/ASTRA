#pragma once

#include <QString>
#include <QVector>
#include <QList>

namespace Periodogram {

struct Grid {
    double f0 = 0.0;
    double df = 0.0;
    int    Nf = 0;
    bool   isValid() const { return Nf > 1 && df > 0.0; }
};

struct Result {
    QVector<double> frequency;   ///< 1/day if t is in days
    QVector<double> power;       ///< same length as frequency
    Grid            grid;
    int             nPoints = 0; ///< input series size
    QString         label;
    bool isValid() const { return power.size() == grid.Nf && grid.isValid(); }
};

/// Build a frequency grid. Any zero parameter is auto-filled.
/// Wraps the (later-supplied) `gen_optimal_samples` external.
Grid generateOptimalGrid(const QVector<double>& t,
                          double oversampling = 5.0,
                          double minPeriod = 0.0,
                          double maxPeriod = 0.0,
                          int    nSamples  = 0);

/// Compute a Generalised Lomb-Scargle periodogram.
/// Wraps the (later-supplied) `gls_fast_extern` external.
Result computeGLS(const QVector<double>& t,
                   const QVector<double>& y,
                   const QVector<double>& dy,
                   const Grid& grid,
                   int  normalization = 0,
                   bool fitMean       = true,
                   bool centerData    = true,
                   int  nterms        = 1);

/// Weighted sum of periodograms sharing the same grid.
/// Weight per part = max(1, part.nPoints).
Result weightedSum(const QList<Result>& parts, const QString& label = {});

/// Geometric-mean product across periodograms on a common interpolated grid.
Result multiplied(const QList<Result>& parts, const QString& label = {});

bool resolveAutoBounds(const QVector<double>& t,
                       double& minPeriod, double& maxPeriod);
} // namespace Periodogram