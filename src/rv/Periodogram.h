#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QList>

#include <atomic>

namespace Periodogram {

/// Cooperative progress / cancellation channel for long computations.
///
/// One instance is shared by every worker of a compute batch: the workers only
/// ever bump `done` (relaxed, once per frequency block, so it costs nothing)
/// and poll `cancel`, while the UI thread samples `done`/`total` on a timer.
struct Progress {
    std::atomic<quint64> done  {0};   ///< frequency bins finished so far
    std::atomic<quint64> total {0};   ///< frequency bins expected in total
    std::atomic<bool>    cancel{false};

    void advance(quint64 n) { done.fetch_add(n, std::memory_order_relaxed); }
    bool cancelled()  const { return cancel.load(std::memory_order_relaxed); }
    void requestCancel()    { cancel.store(true, std::memory_order_relaxed); }
};

struct Grid {
    double f0 = 0.0;
    double df = 0.0;
    int    Nf = 0;
    bool   isValid() const { return Nf > 1 && df > 0.0; }
};

/// Period-finding algorithm used to fill a Result.
enum class Backend {
    LombScargle = 0,   ///< Generalised Lomb-Scargle (sinusoidal model)
    FPW         = 1,   ///< Fast Periodicity Weighting (arbitrary waveform)
};

/// Phase bins for FPW. Finkbeiner et al. (2025) suggest 4-5 for sinusoids,
/// ~10 for general waveforms and ~20 for narrow eclipses.
inline constexpr int kFPWDefaultBins = 10;

/// Floor for a minimum period that had to be *derived from the data* (the
/// caller passed 0 = "auto"). It exists only so that near-duplicate timestamps
/// cannot produce an absurdly fine grid. A minimum period the user typed in is
/// never clamped - asking for a 0.005 d search is legitimate.
inline constexpr double kAutoMinPeriodFloor = 0.01;

// ── Pre-whitening ────────────────────────────────────────────────────

/// Nuisance cycles whose *real* power in the data (diurnal systematics,
/// moonlight, seasonal trends) pre-whitening removes - and with it that
/// power's window-function sidelobes. Aliases of a genuine stellar signal
/// mirrored around the sampling comb are a different beast; those are only
/// flagged (see PeriodogramPanel alias notes), never subtracted.
enum Cycle : quint32 {
    CycleSolarDay     = 1u << 0,
    CycleSiderealDay  = 1u << 1,
    CycleSynodicMonth = 1u << 2,
    CycleYear         = 1u << 3,
};

/// Fundamental periods of the nuisance cycles, in days.
inline constexpr double kSolarDayPeriod     = 1.0;
inline constexpr double kSiderealDayPeriod  = 0.99726957;
inline constexpr double kSynodicMonthPeriod = 29.530589;
inline constexpr double kYearPeriod         = 365.25636;

struct PreWhitenConfig {
    /// Harmonic: one weighted linear least-squares fit of a truncated Fourier
    /// series (`harmonics` sin/cos pairs per selected cycle) plus a constant,
    /// subtracted in a single pass. Only removes power exactly on the comb.
    ///
    /// Template: for each selected cycle, fold at its fundamental period and
    /// subtract the inverse-variance-weighted per-bin mean profile
    /// (`templateBins` uniform phase bins). Captures strongly non-sinusoidal
    /// systematics (airmass curves) including all their harmonics at once.
    enum class Mode { Harmonic = 0, Template = 1 };

    bool    enabled      = false;
    quint32 cycles       = CycleSolarDay | CycleSiderealDay;
    int     harmonics    = 2;    ///< per cycle; Harmonic mode only
    Mode    mode         = Mode::Harmonic;
    int     templateBins = 10;   ///< Template mode only

    /// Also whiten integer multiples of the *daily* periods, up to
    /// `subharmonics` x P (1 = fundamentals only). Phase-fold statistics (FPW)
    /// respond to a daily systematic at every fold period k·P_day - the 2 d,
    /// 3 d, 4 d... folds all contain the repeated daily profile - so removing
    /// the 1 d line alone leaves its residual scatter standing there. Month /
    /// year cycles are not multiplied.
    int     subharmonics = 1;
};

/// The frequency comb (1/day) the config would try to remove: fundamentals of
/// the selected cycles, expanded to `harmonics` multiples in Harmonic mode.
/// Data-independent - prewhiten() may still drop unresolvable lines.
QVector<double> preWhitenFrequencies(const PreWhitenConfig& cfg);

/// Subtract the configured nuisance model from `y` and return the residuals.
/// Returns `y` unchanged when disabled or when the fit is impossible (short
/// baseline, degenerate comb, ...). Comb lines the time baseline cannot
/// resolve are dropped rather than fitted; every such decision is appended to
/// `notes` if given. Pure and thread-safe.
QVector<double> prewhiten(const QVector<double>& t,
                          const QVector<double>& y,
                          const QVector<double>& e,
                          const PreWhitenConfig& cfg,
                          QStringList* notes = nullptr);

/// Hash of everything in a PreWhitenConfig that changes prewhiten()'s output.
quint64 hashPreWhiten(const PreWhitenConfig& cfg);

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

/// Compute an FPW (Fast Periodicity Weighting) periodogram - Finkbeiner,
/// Prince & Whitebook (2025), arXiv:2502.00243.
///
/// For every trial frequency the series is phase-folded into `nBins` uniform
/// bins and the inverse-variance-weighted Δχ² between a piecewise-constant
/// (per-bin) model and a constant model is accumulated. Unlike GLS this makes
/// no assumption about the folded waveform, so eclipses and other sharp
/// features are picked up far better; the price is O(Nf · N) work with no FFT
/// shortcut. Power is returned on the same Δχ²/2 scale as computeGLS().
///
/// If `progress` is given it is advanced as frequency blocks complete and
/// polled for cancellation; a cancelled call returns an invalid Result.
Result computeFPW(const QVector<double>& t,
                   const QVector<double>& y,
                   const QVector<double>& dy,
                   const Grid& grid,
                   int nBins = kFPWDefaultBins,
                   Progress* progress = nullptr);

/// Dispatch to computeGLS() / computeFPW(). `nBins` is ignored by GLS, which
/// also reports no intermediate progress (being FFT-based it is comparatively
/// fast); `progress` is still polled for cancellation before it starts.
Result compute(Backend backend,
                const QVector<double>& t,
                const QVector<double>& y,
                const QVector<double>& dy,
                const Grid& grid,
                int nBins = kFPWDefaultBins,
                Progress* progress = nullptr);

/// Frequency bins one series will report via Progress::advance() - `grid.Nf`
/// for FPW, 0 for backends that only report on completion.
quint64 progressUnits(Backend backend, const Grid& grid);

/// Weighted sum of periodograms sharing the same grid.
/// Weight per part = max(1, part.nPoints).
Result weightedSum(const QList<Result>& parts, const QString& label = {});

/// Geometric-mean product across periodograms on a common interpolated grid.
Result multiplied(const QList<Result>& parts, const QString& label = {});

bool resolveAutoBounds(const QVector<double>& t,
                       double& minPeriod, double& maxPeriod);

quint64 hashData(const QVector<double>& t,
                 const QVector<double>& y,
                 const QVector<double>& e);
/// Hash of everything that invalidates a cached Result besides the input data:
/// the frequency grid plus the backend and its parameters. The LombScargle /
/// nBins=0 case reproduces the pre-backend hash so existing caches stay valid.
quint64 hashGrid(const Grid& g,
                 Backend backend = Backend::LombScargle,
                 int     nBins   = 0);

} // namespace Periodogram