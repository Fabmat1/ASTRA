#include "ObservabilityDialog.h"

#include "models/Star.h"
#include "models/Instrument.h"
#include "models/RadialVelocity.h"
#include "models/AsymmetricErrors.h"
#include "db/DatabaseManager.h"
#include "utils/ObservabilityCalculator.h"
#include "views/panels/PanelUtils.h"
#include "plotting/qcustomplot.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <random>

namespace {
constexpr double MJD_UNIX_EPOCH = 40587.0;

// Returns a "fake UTC" unix time that, when formatted with a UTC ticker,
// displays the wall-clock value in the requested timezone (offsetHours from UTC).
inline double displayUnix(double mjd, double offsetHours)
{
    return (mjd - MJD_UNIX_EPOCH) * 86400.0 + offsetHours * 3600.0;
}

inline double dateToUnix(const QDate& d)
{
    return QDateTime(d, QTime(0, 0), QTimeZone::utc()).toSecsSinceEpoch();
}

inline double dateTimeToMjd(const QDateTime& dt)
{
    return dt.toUTC().toMSecsSinceEpoch() / 86400000.0 + MJD_UNIX_EPOCH;
}

inline QDateTime mjdToDateTime(double mjd)
{
    return QDateTime::fromMSecsSinceEpoch(
        qint64(std::llround((mjd - MJD_UNIX_EPOCH) * 86400000.0)), QTimeZone::utc());
}

inline QString formatTime(double mjd, double offsetHours, const QString& fmt)
{
    const qint64 secs = qint64(displayUnix(mjd, offsetHours));
    return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc()).toString(fmt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Calendar ticker
//
// QCPAxisTickerDateTime picks month-sized steps for multi-month ranges, but it
// snaps every tick to the *day of month of the range start* and does that
// snapping in local time, so a range that begins on the 28th gets labelled
// "Aug / Sep / Oct" on ticks that do not sit on month boundaries at all - and a
// non-UTC machine shifts them across the boundary entirely. This ticker places
// ticks on real UTC calendar boundaries and prints the year only where it
// actually changes, which is what makes a year of months readable: a row of
// "Sep Oct Nov Dec Jan/2027 Feb ..." instead of twelve repetitions of "2026-09".
// ─────────────────────────────────────────────────────────────────────────────
class CalendarTicker : public QCPAxisTicker
{
public:
    CalendarTicker() { setTickCount(12); }

protected:
    enum Unit { Day, Month, Year };

    double getTickStep(const QCPRange& range) override
    {
        // Only used to seed the sub-tick count; createTickVector does the real
        // work on calendar units, which are not expressible as a fixed step.
        return range.size() / double(tickCount() + 1e-10);
    }

    int getSubTickCount(double) override { return _subTicks; }

    QVector<double> createTickVector(double, const QCPRange& range) override
    {
        const double spanDays = range.size() / 86400.0;
        const int    want     = std::max(3, tickCount());

        QVector<double> ticks;
        _yearTicks.clear();
        _subTicks = 0;

        const QDate lo = QDateTime::fromSecsSinceEpoch(
                             qint64(std::floor(range.lower)), QTimeZone::utc()).date();
        const QDate hi = QDateTime::fromSecsSinceEpoch(
                             qint64(std::ceil(range.upper)), QTimeZone::utc()).date().addDays(1);
        if (!lo.isValid() || !hi.isValid()) return ticks;

        auto keyOf = [](const QDate& d) { return dateToUnix(d); };

        if (spanDays <= 70.0) {
            _unit = Day;
            const double ideal = spanDays / want;
            _step = 14;
            for (int c : {1, 2, 5, 7, 14}) { if (c >= ideal) { _step = c; break; } }
            _subTicks = (_step == 7 || _step == 14) ? 6 : (_step >= 2 ? _step - 1 : 0);

            QDate d = lo;
            while (d.toJulianDay() % _step != 0) d = d.addDays(1);
            for (; d <= hi; d = d.addDays(_step)) ticks.append(keyOf(d));
        } else if (spanDays <= 3.5 * 365.25) {
            _unit = Month;
            const double idealMonths = spanDays / 30.4375 / want;
            _step = 6;
            for (int c : {1, 2, 3, 6}) { if (c >= idealMonths) { _step = c; break; } }
            _subTicks = (_step == 1) ? 0 : _step - 1;

            QDate d(lo.year(), lo.month(), 1);
            while ((d.year() * 12 + d.month() - 1) % _step != 0) d = d.addMonths(1);
            for (; d <= hi; d = d.addMonths(_step)) ticks.append(keyOf(d));
        } else {
            _unit = Year;
            const double idealYears = spanDays / 365.25 / want;
            _step = 100;
            for (int c : {1, 2, 5, 10, 20, 50, 100}) { if (c >= idealYears) { _step = c; break; } }
            _subTicks = 0;

            int y = lo.year();
            while (y % _step != 0) ++y;
            for (; y <= hi.year(); y += _step) ticks.append(keyOf(QDate(y, 1, 1)));
        }

        // Mark the ticks that should carry a year: the first one, and every
        // tick that opens a new year. Everything else prints bare.
        int prevYear = INT_MIN;
        for (double t : ticks) {
            // Ticks below the range are trimmed before labelling, so marking one
            // of those as "the first" would lose the leading year entirely.
            if (t < range.lower) continue;
            const int y = QDateTime::fromSecsSinceEpoch(qint64(t), QTimeZone::utc()).date().year();
            if (y != prevYear) { _yearTicks.insert(qint64(t)); prevYear = y; }
        }
        return ticks;
    }

    QString getTickLabel(double tick, const QLocale& locale, QChar, int) override
    {
        const QDate d = QDateTime::fromSecsSinceEpoch(qint64(tick), QTimeZone::utc()).date();
        const bool  withYear = _yearTicks.contains(qint64(tick));
        switch (_unit) {
        case Day:
            return withYear ? QString("%1 %2\n%3").arg(d.day())
                                  .arg(locale.monthName(d.month(), QLocale::ShortFormat))
                                  .arg(d.year())
                            : QString("%1 %2").arg(d.day())
                                  .arg(locale.monthName(d.month(), QLocale::ShortFormat));
        case Month:
            return withYear ? QString("%1\n%2")
                                  .arg(locale.monthName(d.month(), QLocale::ShortFormat))
                                  .arg(d.year())
                            : locale.monthName(d.month(), QLocale::ShortFormat);
        case Year:
        default:
            return QString::number(d.year());
        }
    }

private:
    Unit         _unit     = Month;
    int          _step     = 1;
    int          _subTicks = 0;
    QSet<qint64> _yearTicks;
};

// Picks the ticker that suits the span: clock time for a night, calendar units
// for anything longer. Keys are always "display unix", so the datetime ticker
// runs in UTC even when the axis shows observatory local mean time.
void applyTimeTicker(QCPAxis* axis, double spanSeconds)
{
    if (spanSeconds <= 3.0 * 86400.0) {
        auto t = QSharedPointer<QCPAxisTickerDateTime>::create();
        t->setDateTimeSpec(Qt::UTC);
        t->setDateTimeFormat(spanSeconds > 1.2 * 86400.0 ? "d MMM\nHH:mm" : "HH:mm");
        axis->setTicker(t);
    } else {
        axis->setTicker(QSharedPointer<CalendarTicker>::create());
    }
}

// Twelve months only read as twelve months if there is room for twelve labels.
// The ticker cannot see the axis rect, so the plot feeds it the current width
// before every replot; that also keeps the density right while the user drags.
void syncCalendarTickDensity(QCustomPlot* plot)
{
    auto ticker = qSharedPointerDynamicCast<CalendarTicker>(plot->xAxis->ticker());
    if (!ticker) return;
    const int w = plot->axisRect()->width();
    ticker->setTickCount(std::clamp(w / 70, 4, 13));
}

// ─────────────────────────────────────────────────────────────────────────────
// Monte-Carlo job
// ─────────────────────────────────────────────────────────────────────────────

// One parameter's uncertainty. Kept two-sided so a fit stored with asymmetric
// errors (the solver's own posterior percentiles) is sampled from the two-piece
// normal it actually describes, instead of a mean that walks the quoted edge
// past a physical bound.
struct Sigma
{
    double up = 0.0, down = 0.0;
    bool any() const { return up > 0.0 || down > 0.0; }
};

Sigma makeSigma(double sym, double up, double down)
{
    Sigma s;
    const double u = AsymErr::upOr(up, sym);
    const double d = AsymErr::downOr(down, sym);
    s.up   = (std::isfinite(u) && u > 0.0) ? u : 0.0;
    s.down = (std::isfinite(d) && d > 0.0) ? d : 0.0;
    return s;
}

inline double drawAsym(double mu, const Sigma& s, double z)
{
    return mu + (z >= 0.0 ? s.up * z : s.down * z);
}

struct RvJob
{
    std::shared_ptr<Instrument> inst;
    double raDeg = 0.0, decDeg = 0.0;
    double mjdStart = 0.0, mjdEnd = 0.0;
    double offsetHours = 0.0;
    int    nGrid = 200;
    int    nMc   = 500;

    bool   eccentric = false, hasK2 = false;
    double K = 0, K2 = 0, gamma = 0, period = 1, T0 = 0, ecc = 0, omega = 0;
    Sigma  sK, sK2, sGamma, sPeriod, sT0, sEcc, sOmega;

    bool   shade  = true;
    double minAlt = 30.0, sunAlt = -18.0;
};

// Five quantiles from one sample buffer. Each nth_element leaves everything
// below its pivot to the left, so the next (larger) quantile only has to search
// the remaining tail - five linear passes over shrinking ranges rather than a
// full sort per grid point.
void quantiles5(std::vector<double>& v, double* out)
{
    const size_t n = v.size();
    if (n == 0) { for (int i = 0; i < 5; ++i) out[i] = 0.0; return; }
    static const double qs[5] = {0.025, 0.16, 0.50, 0.84, 0.975};
    size_t lo = 0;
    for (int j = 0; j < 5; ++j) {
        size_t k = size_t(qs[j] * double(n - 1));
        if (k < lo) k = lo;
        std::nth_element(v.begin() + lo, v.begin() + k, v.end());
        out[j] = v[k];
        lo = k;
    }
}

ObservabilityDialog::RvResult
runRvJob(const RvJob& job,
         const std::shared_ptr<std::atomic_bool>& cancel,
         const std::function<void(int)>& progress)
{
    ObservabilityDialog::RvResult res;
    const int Nt  = job.nGrid;
    const int Nmc = job.nMc;
    if (Nt < 2 || Nmc < 1 || !job.inst) return res;

    // Time grid: display-unix for the axis, BJD for the orbital model.
    std::vector<double> bjdGrid(Nt);
    res.ts.resize(Nt);
    for (int i = 0; i < Nt; ++i) {
        const double mjd = job.mjdStart + (job.mjdEnd - job.mjdStart) * i / (Nt - 1);
        res.ts[i]  = displayUnix(mjd, job.offsetHours);
        bjdGrid[i] = job.inst->mjdToBjd(mjd, job.raDeg, job.decDeg);
    }

    // Draw the parameter set once and reuse it at every epoch: a band is the
    // spread of whole curves, so each sample has to keep the same (P, T0, ...)
    // across the grid. Storing the parameters rather than the Nt x Nmc value
    // matrix is what keeps 10^5 samples affordable in memory.
    std::vector<double> sK(Nmc), sK2(Nmc), sGam(Nmc), sP(Nmc), sT0(Nmc), sE(Nmc), sOm(Nmc);
    std::mt19937_64 rng(0xC0FFEEull);
    std::normal_distribution<double> N01(0.0, 1.0);
    for (int m = 0; m < Nmc; ++m) {
        sK[m]   = std::abs(drawAsym(job.K, job.sK, N01(rng)));
        sK2[m]  = job.hasK2 ? std::abs(drawAsym(job.K2, job.sK2, N01(rng))) : 0.0;
        sGam[m] = drawAsym(job.gamma, job.sGamma, N01(rng));
        double p = drawAsym(job.period, job.sPeriod, N01(rng));
        sP[m]   = (p > 0.0) ? p : job.period;
        sT0[m]  = drawAsym(job.T0, job.sT0, N01(rng));
        sE[m]   = job.eccentric ? std::clamp(drawAsym(job.ecc, job.sEcc, N01(rng)), 0.0, 0.95) : 0.0;
        sOm[m]  = job.eccentric ? drawAsym(job.omega, job.sOmega, N01(rng)) : 0.0;
    }

    auto alloc = [Nt](ObservabilityDialog::Band& b) {
        b.med.resize(Nt); b.lo68.resize(Nt); b.hi68.resize(Nt);
        b.lo95.resize(Nt); b.hi95.resize(Nt);
    };
    alloc(res.primary);
    if (job.hasK2) alloc(res.secondary);
    res.hasSecondary = job.hasK2;

    // The unit orbit function is amplitude-independent, so one Kepler solve per
    // (epoch, sample) serves both components: RV1 = gamma + K1*u, RV2 = gamma - K2*u.
    RVFit unit;
    unit.setEccentric(job.eccentric);
    unit.setK(1.0);
    unit.setGamma(0.0);

    std::vector<double> buf1(Nmc), buf2(job.hasK2 ? Nmc : 0);
    double q[5];
    int lastPct = -1;

    for (int i = 0; i < Nt; ++i) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return res;

        const double t = bjdGrid[i];
        for (int m = 0; m < Nmc; ++m) {
            if (job.eccentric) { unit.setEccentricity(sE[m]); unit.setOmega(sOm[m]); }
            double phase = std::fmod((t - sT0[m]) / sP[m], 1.0);
            if (phase < 0.0) phase += 1.0;
            const double u = unit.calculateRVAtPhase(phase);
            buf1[m] = sGam[m] + sK[m] * u;
            if (job.hasK2) buf2[m] = sGam[m] - sK2[m] * u;
        }

        quantiles5(buf1, q);
        res.primary.lo95[i] = q[0]; res.primary.lo68[i] = q[1];
        res.primary.med[i]  = q[2];
        res.primary.hi68[i] = q[3]; res.primary.hi95[i] = q[4];

        if (job.hasK2) {
            quantiles5(buf2, q);
            res.secondary.lo95[i] = q[0]; res.secondary.lo68[i] = q[1];
            res.secondary.med[i]  = q[2];
            res.secondary.hi68[i] = q[3]; res.secondary.hi95[i] = q[4];
        }

        if (progress) {
            const int pct = int(100.0 * (i + 1) / Nt);
            if (pct != lastPct) { lastPct = pct; progress(pct); }
        }
    }

    // Observable windows, sampled finer than the RV grid (a coarse grid over a
    // year would smear night edges by hours).
    if (job.shade) {
        const double spanDays = job.mjdEnd - job.mjdStart;
        const int    maskN = std::clamp(int(std::ceil(spanDays * 48.0)), Nt, 40000);
        bool  inWindow = false;
        double winStart = 0.0;
        for (int i = 0; i < maskN; ++i) {
            const double mjd = job.mjdStart + spanDays * i / (maskN - 1);
            const bool ok =
                Observability::sunAltitudeDeg(*job.inst, mjd) <= job.sunAlt &&
                Observability::altitudeDeg(job.raDeg, job.decDeg, *job.inst, mjd) >= job.minAlt;
            if (ok && !inWindow) { inWindow = true; winStart = mjd; }
            else if (!ok && inWindow) {
                inWindow = false;
                res.observable.append({displayUnix(winStart, job.offsetHours),
                                       displayUnix(mjd, job.offsetHours)});
            }
        }
        if (inWindow)
            res.observable.append({displayUnix(winStart, job.offsetHours),
                                   displayUnix(job.mjdEnd, job.offsetHours)});
        // A year of nights is ~365 rectangles; far beyond that the shading is
        // narrower than a pixel and only adds ink.
        if (res.observable.size() > 250) {
            res.observable.clear();
            res.shadeOmitted = true;
        }
    }

    res.ok = !(cancel && cancel->load(std::memory_order_relaxed));
    return res;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

ObservabilityDialog::ObservabilityDialog(std::shared_ptr<Star> star,
                                         DatabaseManager* dbm,
                                         QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm)
{
    setWindowTitle(QString("Observability - %1")
                       .arg(_star->getAlias().isEmpty() ? _star->getSourceId()
                                                        : _star->getAlias()));
    resize(1040, 760);
    setupUi();
    populateInstruments();
    onConfigChanged();
}

ObservabilityDialog::~ObservabilityDialog()
{
    // The MC worker outlives the dialog only until its next cancel check; the
    // queued result call is dropped by Qt once the receiver is gone.
    if (_rvCancel) _rvCancel->store(true, std::memory_order_relaxed);
}

void ObservabilityDialog::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ── Shared config row ─────────────────────────────────────────────────
    auto* configBox    = new QGroupBox("Configuration", this);
    auto* configLayout = new QGridLayout(configBox);

    configLayout->addWidget(new QLabel("Observatory:"), 0, 0);
    _instrumentCombo = new QComboBox(this);
    _instrumentCombo->setMinimumWidth(200);
    configLayout->addWidget(_instrumentCombo, 0, 1);

    configLayout->addWidget(new QLabel("Date (UTC):"), 0, 2);
    _dateEdit = new QDateEdit(QDate::currentDate(), this);
    _dateEdit->setCalendarPopup(true);
    _dateEdit->setDisplayFormat("yyyy-MM-dd");
    configLayout->addWidget(_dateEdit, 0, 3);

    configLayout->addWidget(new QLabel("Min alt:"), 0, 4);
    _minAltSpin = new QDoubleSpinBox(this);
    _minAltSpin->setRange(0, 90);  _minAltSpin->setDecimals(1);
    _minAltSpin->setSuffix("°");   _minAltSpin->setValue(30);
    configLayout->addWidget(_minAltSpin, 0, 5);

    configLayout->addWidget(new QLabel("Sun alt:"), 0, 6);
    _sunAltSpin = new QDoubleSpinBox(this);
    _sunAltSpin->setRange(-90, 0); _sunAltSpin->setDecimals(1);
    _sunAltSpin->setSuffix("°");   _sunAltSpin->setValue(-18);
    _sunAltSpin->setToolTip("Twilight: -18° astronomical, -12° nautical, -6° civil, 0° horizon");
    configLayout->addWidget(_sunAltSpin, 0, 7);

    _useUtcCheck = new QCheckBox("UTC", this);
    _useUtcCheck->setToolTip("Display times in UTC instead of observatory local mean time");
    configLayout->addWidget(_useUtcCheck, 0, 8);
    connect(_useUtcCheck, &QCheckBox::toggled, this, &ObservabilityDialog::onConfigChanged);

    mainLayout->addWidget(configBox);

    // ── Tabs ──────────────────────────────────────────────────────────────
    _tabs = new QTabWidget(this);
    mainLayout->addWidget(_tabs, 1);

    auto makePlot = [](QWidget* parent) {
        auto* p = new QCustomPlot(parent);
        p->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        PanelUtils::stylePlot(p);
        connect(p, &QCustomPlot::beforeReplot, p, [p]() { syncCalendarTickDensity(p); });
        return p;
    };

    // Night altitude tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        _nightPlot = makePlot(w);
        l->addWidget(_nightPlot, 1);
        _nightSummary = new QLabel(w);
        _nightSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _nightSummary->setWordWrap(true);
        l->addWidget(_nightSummary);
        _tabs->addTab(w, "Night altitude");
    }

    // Yearly observable hours tab
    {
        auto* w   = new QWidget;
        auto* l   = new QVBoxLayout(w);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel("From:"));
        _yearFromEdit = new QDateEdit(QDate::currentDate(), w);
        _yearFromEdit->setCalendarPopup(true);
        _yearFromEdit->setDisplayFormat("yyyy-MM-dd");
        _yearFromEdit->setDateRange(QDate(1900, 1, 1), QDate(2200, 12, 31));
        row->addWidget(_yearFromEdit);
        row->addWidget(new QLabel("to"));
        _yearToEdit = new QDateEdit(QDate::currentDate().addYears(1), w);
        _yearToEdit->setCalendarPopup(true);
        _yearToEdit->setDisplayFormat("yyyy-MM-dd");
        _yearToEdit->setDateRange(QDate(1900, 1, 1), QDate(2200, 12, 31));
        row->addWidget(_yearToEdit);

        auto* nextYear = new QPushButton("Next 12 months", w);
        nextYear->setToolTip("Reset the range to the twelve months starting today");
        connect(nextYear, &QPushButton::clicked, this, [this]() {
            const QDate today = QDate::currentDate();
            QSignalBlocker b1(_yearFromEdit), b2(_yearToEdit);
            _yearFromEdit->setDate(today);
            _yearToEdit->setDate(today.addYears(1));
            plotYearlyHours();
        });
        row->addWidget(nextYear);
        row->addStretch();
        l->addLayout(row);

        _yearlyPlot = makePlot(w);
        l->addWidget(_yearlyPlot, 1);
        _yearSummary = new QLabel(w);
        _yearSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _yearSummary->setWordWrap(true);
        l->addWidget(_yearSummary);
        _tabs->addTab(w, "Yearly observable hours");
    }

    // RV prediction tab
    {
        auto* w    = new QWidget;
        auto* l    = new QVBoxLayout(w);
        auto* grid = new QGridLayout();

        grid->addWidget(new QLabel("Range:"), 0, 0);
        _rvRangeCombo = new QComboBox(w);
        _rvRangeCombo->addItem("Night of the selected date");
        _rvRangeCombo->addItem("Custom range");
        grid->addWidget(_rvRangeCombo, 0, 1);

        grid->addWidget(new QLabel("From (UTC):"), 0, 2);
        _rvFromEdit = new QDateTimeEdit(w);
        _rvFromEdit->setCalendarPopup(true);
        _rvFromEdit->setTimeZone(QTimeZone::utc());
        _rvFromEdit->setDisplayFormat("yyyy-MM-dd HH:mm");
        _rvFromEdit->setDateTimeRange(QDateTime(QDate(1900, 1, 1), QTime(0, 0), QTimeZone::utc()),
                                      QDateTime(QDate(2200, 1, 1), QTime(0, 0), QTimeZone::utc()));
        grid->addWidget(_rvFromEdit, 0, 3);

        grid->addWidget(new QLabel("To (UTC):"), 0, 4);
        _rvToEdit = new QDateTimeEdit(w);
        _rvToEdit->setCalendarPopup(true);
        _rvToEdit->setTimeZone(QTimeZone::utc());
        _rvToEdit->setDisplayFormat("yyyy-MM-dd HH:mm");
        _rvToEdit->setDateTimeRange(QDateTime(QDate(1900, 1, 1), QTime(0, 0), QTimeZone::utc()),
                                    QDateTime(QDate(2200, 1, 1), QTime(0, 0), QTimeZone::utc()));
        grid->addWidget(_rvToEdit, 0, 5);
        grid->setColumnStretch(6, 1);

        grid->addWidget(new QLabel("MC samples:"), 1, 0);
        _nMcSpin = new QSpinBox(w);
        _nMcSpin->setRange(50, 500000);
        _nMcSpin->setSingleStep(500);
        _nMcSpin->setGroupSeparatorShown(true);
        _nMcSpin->setKeyboardTracking(false);   // don't recompute per keystroke
        _nMcSpin->setValue(2000);
        _nMcSpin->setToolTip("Parameter draws per epoch. The run happens off the GUI thread, "
                             "so large values stay responsive - they just take longer.");
        grid->addWidget(_nMcSpin, 1, 1);

        grid->addWidget(new QLabel("Grid points:"), 1, 2);
        _nGridSpin = new QSpinBox(w);
        _nGridSpin->setRange(20, 5000);
        _nGridSpin->setSingleStep(50);
        _nGridSpin->setKeyboardTracking(false);
        _nGridSpin->setValue(300);
        _nGridSpin->setToolTip("Epochs sampled across the range. Long ranges need more "
                               "points to resolve the orbit.");
        grid->addWidget(_nGridSpin, 1, 3);

        _rvShadeCheck = new QCheckBox("Shade observable windows", w);
        _rvShadeCheck->setChecked(true);
        _rvShadeCheck->setToolTip("Highlight the intervals where the target is above the "
                                  "minimum altitude and the Sun is below the twilight limit");
        grid->addWidget(_rvShadeCheck, 1, 4, 1, 2);

        l->addLayout(grid);

        _rvProgress = new QProgressBar(w);
        _rvProgress->setRange(0, 100);
        _rvProgress->setTextVisible(false);
        _rvProgress->setMaximumHeight(6);
        _rvProgress->setVisible(false);
        l->addWidget(_rvProgress);

        _rvPlot = makePlot(w);
        // Six entries (median + two bands per component) floating over the data
        // cover a third of the axis rect, so the RV legend lives under the plot
        // as a single row instead of on top of it.
        _rvPlot->plotLayout()->addElement(1, 0, _rvPlot->legend);
        _rvPlot->plotLayout()->setRowStretchFactor(1, 0.001);
        // foColumnsFirst advances along a row and wraps after `wrap` columns,
        // so this is one horizontal strip of up to six entries.
        _rvPlot->legend->setFillOrder(QCPLegend::foColumnsFirst);
        _rvPlot->legend->setWrap(6);
        _rvPlot->legend->setRowSpacing(1);
        // Median curves get their own layer above "main": the secondary's bands
        // are added after the primary's median and would otherwise wash it out.
        _rvPlot->addLayer("medians", _rvPlot->layer("main"), QCustomPlot::limAbove);
        l->addWidget(_rvPlot, 1);

        _rvStatusLabel = new QLabel(w);
        _rvStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _rvStatusLabel->setWordWrap(true);
        l->addWidget(_rvStatusLabel);
        _tabs->addTab(w, "RV prediction");
    }

    // A big MC run should not restart on every intermediate spinbox value, so
    // the RV tab coalesces rapid changes into one launch.
    _rvDebounce = new QTimer(this);
    _rvDebounce->setSingleShot(true);
    _rvDebounce->setInterval(250);
    connect(_rvDebounce, &QTimer::timeout, this, &ObservabilityDialog::startRvPrediction);

    auto trigger = [this]() { onConfigChanged(); };
    connect(_instrumentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, trigger);
    connect(_dateEdit, &QDateEdit::dateChanged, this, trigger);
    connect(_minAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, trigger);
    connect(_sunAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, trigger);

    connect(_yearFromEdit, &QDateEdit::dateChanged,
            this, &ObservabilityDialog::onYearRangeChanged);
    connect(_yearToEdit, &QDateEdit::dateChanged,
            this, &ObservabilityDialog::onYearRangeChanged);

    connect(_rvRangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateRvRangeControls();
        onRvSettingsChanged();
    });
    connect(_rvFromEdit, &QDateTimeEdit::dateTimeChanged,
            this, &ObservabilityDialog::onRvSettingsChanged);
    connect(_rvToEdit, &QDateTimeEdit::dateTimeChanged,
            this, &ObservabilityDialog::onRvSettingsChanged);
    connect(_nMcSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ObservabilityDialog::onRvSettingsChanged);
    connect(_nGridSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ObservabilityDialog::onRvSettingsChanged);
    connect(_rvShadeCheck, &QCheckBox::toggled,
            this, &ObservabilityDialog::onRvSettingsChanged);

    updateRvRangeControls();
}

void ObservabilityDialog::populateInstruments()
{
    _instruments.clear();
    _instrumentCombo->blockSignals(true);
    _instrumentCombo->clear();
    if (_dbm) {
        for (const auto& inst : _dbm->getAllInstruments()) {
            if (!inst || inst->isSpaceBased() || !inst->hasLocation()) continue;
            _instruments.push_back(inst);
            _instrumentCombo->addItem(inst->getFullName().isEmpty()
                                          ? inst->getName()
                                          : inst->getFullName());
        }
    }
    _instrumentCombo->blockSignals(false);
}

std::shared_ptr<Instrument> ObservabilityDialog::currentInstrument() const
{
    const int idx = _instrumentCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(_instruments.size())) return nullptr;
    return _instruments[idx];
}

void ObservabilityDialog::onConfigChanged()
{
    plotNightAltitude();
    plotYearlyHours();
    onRvSettingsChanged();
}

void ObservabilityDialog::onYearRangeChanged() { plotYearlyHours(); }

void ObservabilityDialog::onRvSettingsChanged()
{
    if (_rvDebounce) _rvDebounce->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab 1: Altitude during the chosen night
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::plotNightAltitude()
{
    PanelUtils::stylePlot(_nightPlot);
    _nightPlot->clearGraphs();
    _nightPlot->clearItems();
    _nightPlot->legend->setVisible(true);

    auto inst = currentInstrument();
    if (!inst) {
        _nightSummary->setText("No ground-based instrument available.");
        _nightPlot->replot();
        return;
    }

    Observability::Config cfg;
    cfg.minAltitudeDeg = _minAltSpin->value();
    cfg.sunAltitudeDeg = _sunAltSpin->value();

    auto night = Observability::computeNight(*inst, _dateEdit->date(), cfg);
    if (!night.valid) {
        _nightSummary->setText("No night at this date (polar day).");
        _nightPlot->replot();
        return;
    }

    const double offsetHours = _useUtcCheck->isChecked() ? 0.0
                                                         : inst->getLongitude() / 15.0;

    constexpr int N = 240;
    QVector<double> ts, alts, sunAlts;
    ts.reserve(N); alts.reserve(N); sunAlts.reserve(N);
    for (int i = 0; i < N; ++i) {
        const double mjd = night.mjdStart + (night.mjdEnd - night.mjdStart) * i / (N - 1);
        ts.append(displayUnix(mjd, offsetHours));
        // Clamp negatives to 0 so a "rising from below horizon" never drags the line below
        alts.append(std::max(0.0,
            Observability::altitudeDeg(_star->getRa(), _star->getDec(), *inst, mjd)));
        sunAlts.append(Observability::sunAltitudeDeg(*inst, mjd));
    }

    const QColor targetColor = PanelUtils::pointColor();
    const QColor sunColor    = PanelUtils::secondaryPointColor();
    const QColor limitColor  = PanelUtils::fitCurveColor();

    auto* targetGraph = _nightPlot->addGraph();
    targetGraph->setData(ts, alts);
    targetGraph->setName("Target altitude");
    QPen tp(targetColor); tp.setWidth(2);
    targetGraph->setPen(tp);
    targetGraph->setBrush(QBrush(PanelUtils::errorBandFor(targetColor)));

    // Sun curve: clamp to [0, 90] for display (drawn against same y-axis,
    // but the night is by definition when sun is below threshold, so it just sits at 0)
    QVector<double> sunDisplay;
    sunDisplay.reserve(N);
    for (double s : sunAlts) sunDisplay.append(std::max(0.0, s));

    auto* sunGraph = _nightPlot->addGraph();
    sunGraph->setData(ts, sunDisplay);
    sunGraph->setName("Sun altitude");
    QPen sp(sunColor); sp.setStyle(Qt::DashLine); sp.setWidth(2);
    sunGraph->setPen(sp);

    // The minimum-altitude threshold is a hard decision boundary, not chrome,
    // so it takes the theme's attention hue rather than the muted annotation one.
    auto* threshold = new QCPItemStraightLine(_nightPlot);
    threshold->point1->setCoords(ts.first(), cfg.minAltitudeDeg);
    threshold->point2->setCoords(ts.last(),  cfg.minAltitudeDeg);
    QPen thrPen(limitColor);
    thrPen.setWidth(2);
    thrPen.setStyle(Qt::DashLine);
    threshold->setPen(thrPen);

    applyTimeTicker(_nightPlot->xAxis, ts.last() - ts.first());
    _nightPlot->xAxis->setLabel(_useUtcCheck->isChecked()
                                   ? "UTC"
                                   : "Local mean time (longitude-based)");
    _nightPlot->yAxis->setLabel("Altitude [deg]");
    _nightPlot->xAxis->setRange(ts.first(), ts.last());
    _nightPlot->yAxis->setRange(0, 90);     // always 0-90°

    const double hours  = Observability::observableHours(
        _star->getRa(), _star->getDec(), *inst, night, cfg);
    const double maxAlt = *std::max_element(alts.constBegin(), alts.constEnd());

    const QString tzLabel = _useUtcCheck->isChecked() ? "UTC" : "local";
    _nightSummary->setText(QString(
        "Night: %1 → %2 %3   ·   Observable hours (alt ≥ %4°): %5   ·   Max altitude: %6°")
        .arg(formatTime(night.mjdStart, offsetHours, "yyyy-MM-dd HH:mm"))
        .arg(formatTime(night.mjdEnd,   offsetHours, "yyyy-MM-dd HH:mm"))
        .arg(tzLabel)
        .arg(cfg.minAltitudeDeg, 0, 'f', 1)
        .arg(hours, 0, 'f', 2)
        .arg(maxAlt, 0, 'f', 1));

    _nightPlot->replot();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab 2: Observable hours per night over an arbitrary date range
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::plotYearlyHours()
{
    PanelUtils::stylePlot(_yearlyPlot);
    _yearlyPlot->clearGraphs();
    _yearlyPlot->clearItems();
    _yearlyPlot->legend->setVisible(true);
    _yearSummary->clear();

    auto inst = currentInstrument();
    if (!inst) { _yearlyPlot->replot(); return; }

    QDate start = _yearFromEdit->date();
    QDate end   = _yearToEdit->date();
    if (end < start) std::swap(start, end);

    Observability::Config cfg;
    cfg.minAltitudeDeg = _minAltSpin->value();
    cfg.sunAltitudeDeg = _sunAltSpin->value();

    // Every night costs an iterative sun-crossing solve, so very long ranges are
    // subsampled: the curve is smooth on a scale of weeks, and 4000 points is
    // already more than the axis rect can resolve.
    const qint64 days = start.daysTo(end) + 1;
    const int    step = int(std::max<qint64>(1, (days + 3999) / 4000));

    QVector<double> xs, ys;
    xs.reserve(int(days / step) + 1);
    ys.reserve(int(days / step) + 1);

    double maxHours = 0.0;
    QDate  bestDate;
    int    nightsObservable = 0;
    double totalHours = 0.0;
    for (QDate d = start; d <= end; d = d.addDays(step)) {
        const auto nw = Observability::computeNight(*inst, d, cfg);
        double h = 0.0;
        if (nw.valid)
            h = Observability::observableHours(
                _star->getRa(), _star->getDec(), *inst, nw, cfg);
        xs.append(dateToUnix(d));
        ys.append(h);
        if (h > 0.0) ++nightsObservable;
        totalHours += h * step;
        if (h > maxHours) { maxHours = h; bestDate = d; }
    }

    const QColor lineColor = PanelUtils::lcColor(2);   // green: "time available"
    auto* g = _yearlyPlot->addGraph();
    g->setData(xs, ys);
    QPen p(lineColor); p.setWidth(2);
    g->setPen(p);
    g->setBrush(QBrush(PanelUtils::errorBandFor(lineColor)));
    g->setName(QString("Hours above %1°").arg(cfg.minAltitudeDeg, 0, 'f', 0));

    // "Today" marker, so a range that starts in the past still reads at a glance.
    const double todayKey = dateToUnix(QDate::currentDate());
    if (!xs.isEmpty() && todayKey >= xs.first() && todayKey <= xs.last()) {
        auto* today = new QCPItemStraightLine(_yearlyPlot);
        today->point1->setCoords(todayKey, 0.0);
        today->point2->setCoords(todayKey, 1.0);
        QPen tp(PanelUtils::plotAnnotationColor());
        tp.setStyle(Qt::DashLine);
        today->setPen(tp);
    }

    applyTimeTicker(_yearlyPlot->xAxis, xs.isEmpty() ? 0.0 : xs.last() - xs.first());
    _yearlyPlot->xAxis->setLabel("Date (UTC)");
    _yearlyPlot->yAxis->setLabel("Observable hours per night");
    if (!xs.empty())
        _yearlyPlot->xAxis->setRange(xs.first(), xs.last());
    // Auto-scale ceiling but always anchored at 0
    _yearlyPlot->yAxis->setRange(0.0, std::max(1.0, std::ceil(maxHours + 0.5)));
    _yearlyPlot->replot();

    if (bestDate.isValid()) {
        _yearSummary->setText(QString(
            "%1 nights with the target above %2°   ·   best night %3 (%4 h)   ·   "
            "%5 h total over %6 nights")
            .arg(nightsObservable * step)
            .arg(cfg.minAltitudeDeg, 0, 'f', 0)
            .arg(bestDate.toString("yyyy-MM-dd"))
            .arg(maxHours, 0, 'f', 2)
            .arg(totalHours, 0, 'f', 0)
            .arg(days));
    } else {
        _yearSummary->setText(QString("The target never rises above %1° at this site "
                                      "during the selected range.")
                                  .arg(cfg.minAltitudeDeg, 0, 'f', 0));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab 3: Predicted RV over an arbitrary window, with Monte-Carlo bands
// ─────────────────────────────────────────────────────────────────────────────
void ObservabilityDialog::updateRvRangeControls()
{
    const bool custom = _rvRangeCombo->currentIndex() == 1;
    _rvFromEdit->setEnabled(custom);
    _rvToEdit->setEnabled(custom);
    _rvFromEdit->setToolTip(custom ? QString()
                                   : QString("Mirrors the night of the selected date. "
                                             "Switch to \"Custom range\" to edit."));
    _rvToEdit->setToolTip(_rvFromEdit->toolTip());
}

bool ObservabilityDialog::currentRvWindow(double& mjdStart, double& mjdEnd,
                                          QString& why) const
{
    auto inst = currentInstrument();
    if (!inst) { why = "No ground-based instrument available."; return false; }

    if (_rvRangeCombo->currentIndex() == 0) {
        Observability::Config cfg;
        cfg.minAltitudeDeg = _minAltSpin->value();
        cfg.sunAltitudeDeg = _sunAltSpin->value();
        const auto night = Observability::computeNight(*inst, _dateEdit->date(), cfg);
        if (!night.valid) { why = "No night at this date (polar day)."; return false; }
        mjdStart = night.mjdStart;
        mjdEnd   = night.mjdEnd;
        return true;
    }

    mjdStart = dateTimeToMjd(_rvFromEdit->dateTime());
    mjdEnd   = dateTimeToMjd(_rvToEdit->dateTime());
    if (!(mjdEnd > mjdStart)) {
        why = "The custom range ends at or before it starts.";
        return false;
    }
    return true;
}

void ObservabilityDialog::startRvPrediction()
{
    // Whatever is in flight is now answering an obsolete question.
    if (_rvCancel) _rvCancel->store(true, std::memory_order_relaxed);
    const quint64 gen = ++_rvGeneration;
    if (_rvPoll) _rvPoll->stop();

    PanelUtils::stylePlot(_rvPlot);
    _rvPlot->legend->setVisible(true);
    _rvProgress->setVisible(false);

    // The plot is not cleared here: the previous prediction stays on screen
    // while the new one is sampled, so a debounced spinbox does not strobe.
    auto fail = [this](const QString& msg) {
        _rvPlot->clearGraphs();
        _rvPlot->clearItems();
        _rvStatusLabel->setText(msg);
        _rvPlot->replot();
    };

    auto inst = currentInstrument();
    if (!inst) return fail("No ground-based instrument available.");

    auto rvCurve = _star->getRVCurve();
    auto bestFit = rvCurve ? rvCurve->getBestFit() : nullptr;
    if (!bestFit) return fail("No best-fit RV curve available for this star.");

    if (!(bestFit->getPeriod() > 0.0) || !std::isfinite(bestFit->getPeriod()))
        return fail("The best-fit RV curve has no usable period.");

    double mjd0 = 0.0, mjd1 = 0.0;
    QString why;
    if (!currentRvWindow(mjd0, mjd1, why)) return fail(why);

    // In night mode the range fields are a read-out, so switching to "Custom
    // range" starts from the window the user was already looking at.
    if (_rvRangeCombo->currentIndex() == 0) {
        QSignalBlocker b1(_rvFromEdit), b2(_rvToEdit);
        _rvFromEdit->setDateTime(mjdToDateTime(mjd0));
        _rvToEdit->setDateTime(mjdToDateTime(mjd1));
    }

    RvJob job;
    job.inst        = inst;
    job.raDeg       = _star->getRa();
    job.decDeg      = _star->getDec();
    job.mjdStart    = mjd0;
    job.mjdEnd      = mjd1;
    job.offsetHours = _useUtcCheck->isChecked() ? 0.0 : inst->getLongitude() / 15.0;
    job.nGrid       = _nGridSpin->value();
    job.nMc         = _nMcSpin->value();
    job.shade       = _rvShadeCheck->isChecked();
    job.minAlt      = _minAltSpin->value();
    job.sunAlt      = _sunAltSpin->value();

    job.eccentric = bestFit->isEccentric();
    job.hasK2     = bestFit->hasK2();
    job.K         = bestFit->getK();
    job.K2        = bestFit->getK2();
    job.gamma     = bestFit->getGamma();
    job.period    = bestFit->getPeriod();
    job.ecc       = bestFit->getEccentricity();
    job.omega     = bestFit->getOmega();

    job.sK     = makeSigma(bestFit->getKError(),  bestFit->getKErrorUp(),  bestFit->getKErrorDown());
    job.sK2    = makeSigma(bestFit->getK2Error(), bestFit->getK2ErrorUp(), bestFit->getK2ErrorDown());
    job.sGamma = makeSigma(bestFit->getGammaError(), bestFit->getGammaErrorUp(),
                           bestFit->getGammaErrorDown());
    job.sPeriod = makeSigma(bestFit->getPeriodError(), bestFit->getPeriodErrorUp(),
                            bestFit->getPeriodErrorDown());
    job.sEcc    = makeSigma(bestFit->getEccentricityError(), bestFit->getEccentricityErrorUp(),
                            bestFit->getEccentricityErrorDown());
    job.sOmega  = makeSigma(bestFit->getOmegaError(), bestFit->getOmegaErrorUp(),
                            bestFit->getOmegaErrorDown());

    // Absolute T0 (BJD of phase 0). The model helper is used rather than
    // rebuilding it from phi here, because that would drop the sign phi carries
    // in the eccentric convention.
    job.T0   = bestFit->foldEpochBJD();
    job.sT0  = makeSigma(bestFit->getT0Error(), bestFit->getT0ErrorUp(),
                         bestFit->getT0ErrorDown());
    if (!job.sT0.any()) {
        // No stored T0 error: propagate it from phi and the period instead.
        const Sigma sPhi = makeSigma(bestFit->getPhiError(), bestFit->getPhiErrorUp(),
                                     bestFit->getPhiErrorDown());
        const double sSym = std::hypot(0.5 * (sPhi.up + sPhi.down) * job.period,
                                       bestFit->getPhi() *
                                           0.5 * (job.sPeriod.up + job.sPeriod.down));
        job.sT0.up = job.sT0.down = sSym;
    }

    const bool noUncertainties = !job.sK.any() && !job.sGamma.any() &&
                                 !job.sPeriod.any() && !job.sT0.any() &&
                                 !job.sEcc.any() && !job.sOmega.any() &&
                                 !(job.hasK2 && job.sK2.any());

    // ── Status text (built here, on the GUI thread) ───────────────────────
    auto pm = [](const Sigma& s) {
        if (s.up == s.down) return QString("± %1").arg(s.up, 0, 'g', 3);
        return QString("+%1 -%2").arg(s.up, 0, 'g', 3).arg(s.down, 0, 'g', 3);
    };
    const double tMid   = job.inst->mjdToBjd(0.5 * (mjd0 + mjd1), job.raDeg, job.decDeg);
    const double cycles = (job.period > 0.0) ? (tMid - job.T0) / job.period : 0.0;
    const double phaseSigma =
        (job.period > 0.0)
            ? std::hypot(0.5 * (job.sT0.up + job.sT0.down) / job.period,
                         cycles * 0.5 * (job.sPeriod.up + job.sPeriod.down) / job.period)
            : 0.0;

    _rvParamText = QString("K = %1 %2 km/s   ·   γ = %3 %4 km/s   ·   P = %5 %6 d   ·   "
                           "T₀(BJD) = %7 %8")
                       .arg(job.K, 0, 'f', 2).arg(pm(job.sK))
                       .arg(job.gamma, 0, 'f', 2).arg(pm(job.sGamma))
                       .arg(job.period, 0, 'g', 6).arg(pm(job.sPeriod))
                       .arg(job.T0, 0, 'f', 4).arg(pm(job.sT0));
    if (job.hasK2)
        _rvParamText += QString("   ·   K₂ = %1 %2 km/s")
                            .arg(job.K2, 0, 'f', 2).arg(pm(job.sK2));
    if (job.eccentric)
        _rvParamText += QString("   ·   e = %1 %2   ·   ω = %3 %4")
                            .arg(job.ecc, 0, 'f', 3).arg(pm(job.sEcc))
                            .arg(job.omega, 0, 'f', 3).arg(pm(job.sOmega));
    _rvParamText += QString("\nSpan: %1 d   ·   cycles since T₀: %2   ·   "
                            "propagated phase σ at mid-epoch: %3")
                        .arg(mjd1 - mjd0, 0, 'f', 3)
                        .arg(cycles, 0, 'f', 1)
                        .arg(phaseSigma, 0, 'f', 3);
    if (noUncertainties)
        _rvParamText += "\n⚠ No parameter uncertainties stored - bands collapse to the median.";

    // ── Launch ────────────────────────────────────────────────────────────
    // The worker gets only value types and shared flags; it never touches a
    // widget, so a dialog closed mid-run just flips the cancel flag and the
    // result is discarded when nobody reads the future.
    auto cancel   = std::make_shared<std::atomic_bool>(false);
    auto progress = std::make_shared<std::atomic_int>(0);
    _rvCancel        = cancel;
    _rvProgressValue = progress;

    _rvProgress->setValue(0);
    _rvProgress->setVisible(true);
    _rvStatusLabel->setText(
        QString("Sampling %L1 draws on %L2 epochs...").arg(job.nMc).arg(job.nGrid)
        + "\n" + _rvParamText);
    if (!_rvPoll) {
        _rvPoll = new QTimer(this);
        connect(_rvPoll, &QTimer::timeout, this, [this]() {
            if (_rvProgressValue) _rvProgress->setValue(_rvProgressValue->load());
        });
    }
    _rvPoll->start(120);

    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    auto* watcher = new QFutureWatcher<RvResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, gen, elapsed]() {
                watcher->deleteLater();
                if (gen != _rvGeneration) return;   // superseded by a newer run
                if (_rvPoll) _rvPoll->stop();
                _rvProgress->setVisible(false);
                showRvResult(watcher->result(), elapsed->elapsed() / 1000.0);
            });
    watcher->setFuture(QtConcurrent::run([job, cancel, progress]() {
        return runRvJob(job, cancel,
                        [progress](int pct) { progress->store(pct); });
    }));
}

void ObservabilityDialog::showRvResult(const RvResult& res, double elapsedSec)
{
    _rvPlot->clearGraphs();
    _rvPlot->clearItems();

    if (!res.ok || res.ts.isEmpty()) {
        _rvStatusLabel->setText("Prediction cancelled.\n" + _rvParamText);
        _rvPlot->replot();
        return;
    }

    // Observable windows go on the grid layer so they sit behind the curves but
    // above the plot background.
    if (!res.observable.isEmpty()) {
        const bool dense = res.observable.size() > 40;
        QColor shade = PanelUtils::towardBg(PanelUtils::lcColor(2),
                                            PanelUtils::isDarkTheme() ? 0.50 : 0.35);
        shade.setAlpha(PanelUtils::isDarkTheme() ? (dense ? 55 : 80)
                                                 : (dense ? 45 : 60));
        for (const auto& w : res.observable) {
            auto* r = new QCPItemRect(_rvPlot);
            r->setLayer("grid");
            for (auto* pos : {r->topLeft, r->bottomRight}) {
                pos->setAxes(_rvPlot->xAxis, _rvPlot->yAxis);
                pos->setAxisRect(_rvPlot->axisRect());
                pos->setTypeX(QCPItemPosition::ptPlotCoords);
                pos->setTypeY(QCPItemPosition::ptAxisRectRatio);
            }
            r->topLeft->setCoords(w.first, 0.0);
            r->bottomRight->setCoords(w.second, 1.0);
            r->setPen(Qt::NoPen);
            r->setBrush(QBrush(shade));
        }
    }

    auto addBand = [this](const QVector<double>& lo, const QVector<double>& hi,
                          const QVector<double>& ts, const QColor& fill,
                          const QString& name)
    {
        auto* gLo = _rvPlot->addGraph();
        gLo->setData(ts, lo);
        gLo->setPen(QPen(Qt::NoPen));
        gLo->removeFromLegend();
        auto* gHi = _rvPlot->addGraph();
        gHi->setData(ts, hi);
        gHi->setPen(QPen(Qt::NoPen));
        gHi->setBrush(QBrush(fill));
        gHi->setChannelFillGraph(gLo);
        gHi->setName(name);
    };

    auto drawComponent = [&](const Band& b, const QColor& base,
                             const QString& label, Qt::PenStyle style)
    {
        QColor c95 = PanelUtils::errorBandFor(base);
        c95.setAlpha(c95.alpha() / 2);
        const QColor c68 = PanelUtils::errorBandFor(base);
        addBand(b.lo95, b.hi95, res.ts, c95, label + " 95%");
        addBand(b.lo68, b.hi68, res.ts, c68, label + " 68%");

        auto* g = _rvPlot->addGraph();
        g->setLayer("medians");
        g->setData(res.ts, b.med);
        QPen p(PanelUtils::modelCurveFor(base));
        p.setWidth(2);
        p.setStyle(style);
        g->setPen(p);
        g->setName(label + " median");
    };

    drawComponent(res.primary, PanelUtils::pointColor(), "Primary", Qt::SolidLine);
    if (res.hasSecondary)
        drawComponent(res.secondary, PanelUtils::secondaryPointColor(),
                      "Secondary", Qt::DashLine);

    applyTimeTicker(_rvPlot->xAxis, res.ts.last() - res.ts.first());
    _rvPlot->xAxis->setLabel(_useUtcCheck->isChecked()
                                 ? "UTC"
                                 : "Local mean time (longitude-based)");
    _rvPlot->yAxis->setLabel("Predicted RV [km/s]");
    _rvPlot->rescaleAxes();
    _rvPlot->xAxis->setRange(res.ts.first(), res.ts.last());

    QString status = _rvParamText;
    status += QString("\n%L1 draws x %L2 epochs in %3 s")
                  .arg(_nMcSpin->value()).arg(res.ts.size()).arg(elapsedSec, 0, 'f', 1);
    if (res.shadeOmitted)
        status += "   ·   observable-window shading omitted (too many windows for this span)";
    _rvStatusLabel->setText(status);

    _rvPlot->replot();
}
