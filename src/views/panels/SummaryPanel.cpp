#include "SummaryPanel.h"
#include "PanelUtils.h"

#include "models/Star.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "utils/CrossRefResolver.h"
#include "utils/AppPaths.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <random>

namespace {

struct PropRow {
    QString label;
    QString value;
    QString copyValue;
};

class CopyEventFilter : public QObject
{
public:
    CopyEventFilter(const QString& text, QWidget* target, QObject* parent = nullptr)
        : QObject(parent), _text(text), _target(target) {}

protected:
    bool eventFilter(QObject*, QEvent* ev) override
    {
        if (ev->type() == QEvent::MouseButtonPress) {
            QApplication::clipboard()->setText(_text);
            auto* popup = new QLabel("\xe2\x9c\x93 Copied");
            popup->setAttribute(Qt::WA_DeleteOnClose);
            popup->setAttribute(Qt::WA_ShowWithoutActivating);
            popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
            popup->setStyleSheet(
                "QLabel { background: #4CAF50; color: white; font-weight: bold;"
                " padding: 4px 12px; border-radius: 4px; font-size: 12px; }");
            popup->adjustSize();
            popup->move(QCursor::pos() + QPoint(12, 12));
            popup->show();
            QTimer::singleShot(1000, popup, &QLabel::close);
            return true;
        }
        return false;
    }

private:
    QString  _text;
    QWidget* _target;
};

void makeCopyable(QLabel* label, const QString& textToCopy)
{
    label->setCursor(Qt::PointingHandCursor);
    label->installEventFilter(new CopyEventFilter(textToCopy, label, label));
}

QString bibcodeDisplayJournal(const QString& rawAbbrev)
{
    static const QMap<QString, QString> map = {
        {"Natur", "Nature"}, {"NatAs", "Nat. Astron."},
        {"Sci..", "Science"},
    };
    auto it = map.find(rawAbbrev);
    if (it != map.end()) return *it;
    QString c = rawAbbrev;
    c.remove('.');
    return c.trimmed();
}

QString formatBibcodeMeta(const QString& bib)
{
    if (bib.length() != 19) return QString();
    int year = bib.left(4).toInt();
    QString journal = bibcodeDisplayJournal(bib.mid(4, 5));
    QString volume = bib.mid(9, 4);
    volume.remove('.');
    volume = volume.trimmed();
    QChar section = bib.at(13);
    QString page = bib.mid(14, 4);
    page.remove('.');
    page = page.trimmed();
    if (section.isLetter()) page = QString(section) + page;

    QStringList parts;
    if (year > 0) parts << QString::number(year);
    if (!journal.isEmpty()) parts << journal;
    if (!volume.isEmpty()) {
        if (!page.isEmpty())
            parts << (volume + ", " + page);
        else
            parts << volume;
    }
    return parts.join(", ");
}
struct ValDisp {
    QString display;
    QString copy;
};
inline ValDisp fmtValErr(double v, double err, int prec,
                         const QString &unit = "") {
    QString num = QString::number(v, 'f', prec);
    QString s   = num;
    if (std::isfinite(err) && err > 0.0)
        s += QString(" ± %1").arg(err, 0, 'f', prec);
    if (!unit.isEmpty())
        s += " " + unit;
    return {s, num};
}

// Mass function (M_sun) with K [km/s], P [days]
inline double massFunctionMsun(double K_kms, double P_days, double e) {
    constexpr double C  = 1.0361e-7;
    const double     ef = std::max(0.0, 1.0 - e * e);
    return C * std::pow(K_kms, 3) * P_days * std::pow(ef, 1.5);
}

// Solve  M2³·sin³i = f·(M1+M2)²
double solveCompanionMass(double f, double M1, double sini) {
    if (!std::isfinite(f) || f <= 0.0 || !std::isfinite(M1) || M1 <= 0.0 ||
        !std::isfinite(sini) || sini <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();

    const double s3 = sini * sini * sini;
    double       M2 = std::cbrt(f * M1 * M1) / sini;
    if (M2 <= 0.0)
        M2 = 0.1;

    for (int i = 0; i < 80; ++i) {
        const double sum = M1 + M2;
        const double g   = M2 * M2 * M2 * s3 - f * sum * sum;
        const double gp  = 3.0 * M2 * M2 * s3 - 2.0 * f * sum;
        if (std::abs(gp) < 1e-30)
            break;
        const double dM = g / gp;
        M2 -= dM;
        if (M2 <= 0.0)
            M2 = 1e-6;
        if (std::abs(dM) < 1e-12)
            break;
    }
    return M2;
}

inline double m2Of(double P, double K, double M1, double e, double sini) {
    return solveCompanionMass(massFunctionMsun(K, P, e), M1, sini);
}

// Linearised error propagation via central differences.
// Roughly 10 solver calls — microseconds vs. milliseconds for the MC.
double propagateM2Error(double P, double eP, double K, double eK, double M1,
                        double eM1, double e, double ee, double sini,
                        double esini) {
    double var = 0.0;
    auto   add = [&](double v, double err, auto &&eval) {
        if (!std::isfinite(err) || err <= 0.0)
            return;
        const double h  = std::max(1e-7, 1e-4 * std::abs(v));
        const double up = eval(v + h);
        const double dn = eval(v - h);
        if (!std::isfinite(up) || !std::isfinite(dn))
            return;
        const double d = (up - dn) / (2.0 * h);
        var += d * d * err * err;
    };
    add(P, eP, [&](double x) { return m2Of(x, K, M1, e, sini); });
    add(K, eK, [&](double x) { return m2Of(P, x, M1, e, sini); });
    add(M1, eM1, [&](double x) { return m2Of(P, K, x, e, sini); });
    add(e, ee, [&](double x) { return m2Of(P, K, M1, x, sini); });
    add(sini, esini, [&](double x) { return m2Of(P, K, M1, e, x); });
    return std::sqrt(var);
}

QWidget *buildPropertyGrid(const std::vector<PropRow> &rows,
                           const QColor &valCol, const QColor &labelCol) {
    QWidget     *grid = new QWidget;
    QGridLayout *gl   = new QGridLayout(grid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setHorizontalSpacing(16);
    gl->setVerticalSpacing(4);
    if (rows.empty())
        return grid;

    int maxPerCol = static_cast<int>((rows.size() + 1) / 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        int     col = (i < maxPerCol) ? 0 : 2;
        int     row = (i < maxPerCol) ? i : i - maxPerCol;
        QString copyText =
            rows[i].copyValue.isEmpty() ? rows[i].value : rows[i].copyValue;

        QLabel *lbl = new QLabel(rows[i].label);
        lbl->setStyleSheet(
            QString("font-size: 11px; font-weight: 600; color: %1; "
                    "background: transparent; border: none;")
                .arg(labelCol.name()));
        makeCopyable(lbl, copyText);

        QLabel *val = new QLabel(rows[i].value);
        val->setStyleSheet(QString("font-size: 12px; color: %1; background: "
                                   "transparent; border: none;")
                               .arg(valCol.name()));
        makeCopyable(val, copyText);

        gl->addWidget(lbl, row, col);
        gl->addWidget(val, row, col + 1);
    }
    gl->setColumnStretch(1, 1);
    if (rows.size() > static_cast<size_t>(maxPerCol))
        gl->setColumnStretch(3, 1);
    return grid;
}

} // anonymous namespace

SummaryPanel::SummaryPanel(const Context& ctx, QWidget* parent)
    : DetailPanel(ctx, parent)
{
    setupUi();
    rebuild();
}

void SummaryPanel::setupUi() {
    auto *box   = new QGroupBox("Summary", this);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(box);

    auto *bl = new QVBoxLayout(box);
    _scroll  = new QScrollArea;
    _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bl->addWidget(_scroll);

    _refResolver = new CrossRefResolver(AppPaths::database(), this);

    connect(_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &SummaryPanel::onSummaryScrolled);
}

void SummaryPanel::rebuild() {
    _builtDark = PanelUtils::isDarkTheme();
    _scroll->setWidget(buildDashboard());
}
void SummaryPanel::refresh() { rebuild(); }

void SummaryPanel::refreshTheme() {
    if (_builtDark == PanelUtils::isDarkTheme())
        return; 
    rebuild();
}

QWidget *SummaryPanel::buildDashboard() {
    ensureCompanionMasses();

    _refCardHost     = nullptr;
    _refSpinner      = nullptr;
    _loadingMoreRefs = false;
    _pendingRefs.clear();

    QWidget *container = new QWidget;

    QVBoxLayout *layout    = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(10);

    layout->addWidget(createNameHeader());
    layout->addWidget(createMetricCardsRow());
    layout->addWidget(createPropertiesSection());

    auto                   rvCurve = _ctx.star->getRVCurve();
    std::shared_ptr<RVFit> bestFit = rvCurve ? rvCurve->getBestFit() : nullptr;
    if (bestFit && bestFit->getPeriod() > 0)
        layout->addWidget(createOrbitalFitSection());

    if (QWidget *comp = createCompanionSection())
        layout->addWidget(comp);

    layout->addWidget(createDataInventorySection());

    if (!_ctx.star->getBibcodes().empty())
        layout->addWidget(createReferencesSection());

    layout->addStretch();
    return container;
}

QWidget* SummaryPanel::createNameHeader()
{
    bool dark = PanelUtils::isDarkTheme();

    QWidget* header = new QWidget;
    QHBoxLayout* hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(4, 0, 4, 0);
    hLayout->setSpacing(12);

    // Left side: name + source ID
    QVBoxLayout* nameCol = new QVBoxLayout;
    nameCol->setSpacing(2);

    QString displayName = _ctx.star->getAlias().isEmpty() ? _ctx.star->getSourceId() : _ctx.star->getAlias();

    QLabel* nameLabel = new QLabel(displayName);
    nameLabel->setStyleSheet(QString("font-size: 20px; font-weight: 700; color: %1; background: transparent;")
        .arg(dark ? "white" : "#1a1a1a"));
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameCol->addWidget(nameLabel);

    // Gaia source ID line
    QString subText;
    if (!_ctx.star->getAlias().isEmpty() && !_ctx.star->getSourceId().isEmpty())
        subText = QString("Gaia DR3 %1").arg(_ctx.star->getSourceId());
    if (!_ctx.star->getTic().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += QString("TIC %1").arg(_ctx.star->getTic());
    }
    if (!_ctx.star->getJName().isEmpty()) {
        if (!subText.isEmpty()) subText += "  ·  ";
        subText += _ctx.star->getJName();
    }

    if (!subText.isEmpty()) {
        QLabel* subLabel = new QLabel(subText);
        subLabel->setStyleSheet(QString("font-size: 12px; color: %1; background: transparent;")
            .arg(dark ? "#999" : "#666"));
        subLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameCol->addWidget(subLabel);
    }

    hLayout->addLayout(nameCol, 1);

    // Right side: spectral class badge (if available)
    QString specClass = _ctx.star->getSpecClass();
    if (!specClass.isEmpty()) {
        QColor badgeColor = specClassColor(specClass);
        QColor badgeText = accentTextColor(badgeColor);

        QLabel* badge = new QLabel(specClass);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedHeight(30);
        badge->setMinimumWidth(60);
        badge->setStyleSheet(QString(
            "font-size: 13px; font-weight: 700; color: %1; "
            "background: %2; border-radius: 6px; "
            "padding: 2px 12px; border: none;"
        ).arg(badgeText.name(), badgeColor.name()));
        hLayout->addWidget(badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    return header;
}

QWidget *SummaryPanel::createMetricCardsRow() {
    QWidget     *row    = new QWidget;
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto has = [](double v) { return std::isfinite(v) && v != 0.0; };

    Star        &S    = *_ctx.star;
    const bool   dark = PanelUtils::isDarkTheme();
    const QColor inactive =
        dark ? QColor(100, 100, 100) : QColor(180, 180, 180);

    // ── log(p)
    {
        const double logP    = S.getLogP();
        const int    nPoints = S.getRVNPoints();
        const int    nSpec   = S.getNSpectra();
        QString      subtitle;
        if (nPoints > 0)
            subtitle = QString("from %1 points").arg(nPoints);
        else if (nSpec > 0)
            subtitle = QString("from %1 spectra").arg(nSpec);

        QString value = has(logP) ? QString::number(logP, 'f', 2) : "—";
        layout->addWidget(
            createMetricCard(value, "log(p)", subtitle, logPColor(logP)));
    }

    // ── ΔRV_max
    {
        const double drv   = S.getDeltaRV();
        const double edrv  = S.getEDeltaRV();
        QString      value = has(drv) ? QString::number(drv, 'f', 1) : "—";
        QString      subtitle;
        if (has(drv)) {
            subtitle = has(edrv) ? QString("± %1 km/s").arg(edrv, 0, 'f', 1)
                                 : QString("km/s");
        }
        layout->addWidget(
            createMetricCard(value, "ΔRV_max", subtitle, deltaRVColor(drv)));
    }

    // ── N spectra (cached; no lazy load)
    {
        const int n = S.getNSpectra();
        QString   subtitle;
        if (S.getNFitSpectra() > 0)
            subtitle = QString("%1 fitted").arg(S.getNFitSpectra());

        QColor accent = (n > 0) ? QColor(86, 156, 214) : inactive;
        layout->addWidget(
            createMetricCard(QString::number(n), "Spectra", subtitle, accent));
    }

    // ── N RV points (cached)
    {
        const int    n    = S.getRVNPoints();
        const double span = S.getRVTimespan();
        QString      subtitle;
        if (n > 0 && std::isfinite(span) && span > 0)
            subtitle = QString("%1 d span").arg(span, 0, 'f', 0);

        QColor accent = (n > 0) ? QColor(86, 180, 120) : inactive;
        layout->addWidget(createMetricCard(QString::number(n), "RV Points",
                                           subtitle, accent));
    }

    return row;
}

QWidget* SummaryPanel::createMetricCard(const QString& value, const QString& label,
                                           const QString& subtitle, const QColor& accentColor)
{
    bool dark = PanelUtils::isDarkTheme();
    QColor cardBg   = dark ? QColor(50, 50, 55)   : QColor(248, 248, 250);
    QColor border   = dark ? QColor(70, 70, 75)    : QColor(210, 210, 215);
    QColor labelCol = dark ? QColor(160, 160, 165) : QColor(110, 110, 115);
    QColor subCol   = dark ? QColor(130, 130, 135) : QColor(140, 140, 145);

    QFrame* card = new QFrame;
    card->setObjectName("metricCard");
    card->setStyleSheet(QString(
        "QFrame#metricCard { background: %1; border: 1px solid %2; "
        "border-left: 4px solid %3; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name(), accentColor.name()));

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);

    QLabel* valueLabel = new QLabel(value);
    valueLabel->setStyleSheet(QString(
        "font-size: 22px; font-weight: 700; color: %1; border: none; background: transparent;"
    ).arg(accentColor.name()));
    valueLabel->setAlignment(Qt::AlignLeft);
    if (value != "—")
        makeCopyable(valueLabel, value);
    layout->addWidget(valueLabel);

    QLabel* labelWidget = new QLabel(label);
    labelWidget->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1; border: none; background: transparent;"
    ).arg(labelCol.name()));
    if (value != "—")
        makeCopyable(labelWidget, value);
    layout->addWidget(labelWidget);

    if (!subtitle.isEmpty()) {
        QLabel* subLabel = new QLabel(subtitle);
        subLabel->setStyleSheet(QString(
            "font-size: 10px; color: %1; border: none; background: transparent;"
        ).arg(subCol.name()));
        layout->addWidget(subLabel);
    }

    layout->addStretch();
    card->setMinimumWidth(100);
    card->setMinimumHeight(80);
    return card;
}

QWidget *SummaryPanel::createPropertiesSection() {
    auto has = [](double v) { return std::isfinite(v) && v != 0.0; };

    const bool   dark   = PanelUtils::isDarkTheme();
    const QColor valCol = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    const QColor labelCol =
        dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    auto addV = [&](std::vector<PropRow> &rows, const QString &l, double v,
                    double err, int prec, const QString &unit = "") {
        if (!has(v))
            return;
        auto d = fmtValErr(v, err, prec, unit);
        rows.push_back({l, d.display, d.copy});
    };
    auto addPlain = [&](std::vector<PropRow> &rows, const QString &l, double v,
                        int prec, const QString &unit = "") {
        if (!has(v))
            return;
        QString n = QString::number(v, 'f', prec);
        rows.push_back({l, unit.isEmpty() ? n : n + " " + unit, n});
    };

    Star &S = *_ctx.star;

    // ── Astrometry ─────────────────────────────────────────────────────────
    std::vector<PropRow> astroC, astroF;
    if (has(S.getRa()) && has(S.getDec())) {
        QString raNum  = QString::number(S.getRa(), 'f', 6);
        QString decNum = QString::number(S.getDec(), 'f', 6);
        astroC.push_back({"RA", raNum + "°", raNum});
        astroC.push_back({"Dec", decNum + "°", decNum});
        astroF.push_back({"RA", raNum + "°", raNum});
        astroF.push_back({"Dec", decNum + "°", decNum});
    }
    addV(astroC, "Parallax", S.getPlx(), S.getEPlx(), 3, "mas");
    addV(astroC, "μ_RA", S.getPmra(), S.getEPmra(), 3, "mas/yr");
    addV(astroC, "μ_Dec", S.getPmdec(), S.getEPmdec(), 3, "mas/yr");

    addV(astroF, "Parallax", S.getPlx(), S.getEPlx(), 4, "mas");
    addV(astroF, "μ_RA", S.getPmra(), S.getEPmra(), 4, "mas/yr");
    addV(astroF, "μ_Dec", S.getPmdec(), S.getEPmdec(), 4, "mas/yr");
    addPlain(astroF, "ρ(μα,μδ)", S.getPmraPmdecCorr(), 3);
    addPlain(astroF, "ρ(ϖ,μα)", S.getPlxPmraCorr(), 3);
    addPlain(astroF, "ρ(ϖ,μδ)", S.getPlxPmdecCorr(), 3);

    // ── Photometry ─────────────────────────────────────────────────────────
    std::vector<PropRow> photoC, photoF;
    addV(photoC, "G", S.getGmag(), S.getEGmag(), 3, "mag");
    addPlain(photoC, "BP−RP", S.getBpRp(), 3, "mag");
    addV(photoC, "BP", S.getBp(), S.getEBp(), 3, "mag");
    addV(photoC, "RP", S.getRp(), S.getERp(), 3, "mag");

    addV(photoF, "G", S.getGmag(), S.getEGmag(), 4, "mag");
    addV(photoF, "BP", S.getBp(), S.getEBp(), 4, "mag");
    addV(photoF, "RP", S.getRp(), S.getERp(), 4, "mag");
    addPlain(photoF, "BP−RP", S.getBpRp(), 4, "mag");

    // ── Light-curve fit (shown in expanded photometry) ─────────────────────
    std::shared_ptr<LCFit> bestLC;
    if (auto phot = S.getPhotometry()) {
        for (const auto &src : phot->getLightcurveSources()) {
            if (auto f = phot->getBestLCFit(src)) {
                if (!bestLC || (f->chi2 > 0 &&
                                (bestLC->chi2 <= 0 || f->chi2 < bestLC->chi2)))
                    bestLC = f;
            }
        }
    }
    if (bestLC) {
        addV(photoF, "LC Period", bestLC->period, bestLC->periodError, 6, "d");
        addV(photoF, "T₀ (BJD)", bestLC->t0BJD, bestLC->t0BJDError, 6, "");
        addV(photoF, "Inclination", bestLC->inclination,
             bestLC->inclinationError, 2, "°");
        addV(photoF, "q (M₂/M₁)", bestLC->q, bestLC->qError, 3, "");
        addV(photoF, "r₁/a", bestLC->r1, bestLC->r1Error, 4, "");
        addV(photoF, "r₂/a", bestLC->r2, bestLC->r2Error, 4, "");
        addV(photoF, "T₁", bestLC->t1, bestLC->t1Error, 0, "K");
        addV(photoF, "T₂", bestLC->t2, bestLC->t2Error, 0, "K");
        addV(photoF, "v_scale", bestLC->velocityScale,
             bestLC->velocityScaleError, 2, "km/s");
        addPlain(photoF, "LC χ²", bestLC->chi2, 2);
        addPlain(photoF, "LC rms", bestLC->rms, 4);

        // Orbital separation a [R☉] from r₁/a together with SED radius R₁
        if (std::isfinite(bestLC->r1) && bestLC->r1 > 0 &&
            std::isfinite(S.getSedRadius1()) && S.getSedRadius1() > 0) {
            const double a = S.getSedRadius1() / bestLC->r1;
            QString      n = QString::number(a, 'f', 2);
            photoF.push_back({"Sep. a (LC)", n + " R☉", n});
        }
    }

    // ── Atmospheric ────────────────────────────────────────────────────────
    std::vector<PropRow> atmosC, atmosF;
    addV(atmosC, "T_eff", S.getTeff(), S.getETeff(), 0, "K");
    addV(atmosC, "log g", S.getLogg(), S.getELogg(), 2, "dex");
    addV(atmosC, "log(He/H)", S.getHe(), S.getEHe(), 2);

    if (!S.getSpecClass().isEmpty())
        atmosF.push_back({"Spec. Class", S.getSpecClass(), S.getSpecClass()});
    addV(atmosF, "T_eff", S.getTeff(), S.getETeff(), 0, "K");
    addV(atmosF, "log g", S.getLogg(), S.getELogg(), 3, "dex");
    addV(atmosF, "log(He/H)", S.getHe(), S.getEHe(), 3);
    if (S.getNSpectra() > 0)
        atmosF.push_back({"N Spectra", QString::number(S.getNSpectra()),
                          QString::number(S.getNSpectra())});
    if (S.getNFitSpectra() > 0)
        atmosF.push_back({"N Fit Spectra", QString::number(S.getNFitSpectra()),
                          QString::number(S.getNFitSpectra())});

    // ── Radial velocity (when no orbital section is shown) ─────────────────
    auto                   rvCurve = S.getRVCurve();
    std::shared_ptr<RVFit> bestFit;
    if (rvCurve)
        bestFit = rvCurve->getBestFit();
    const bool hasOrbital = bestFit && bestFit->getPeriod() > 0;

    std::vector<PropRow> rvC, rvF;
    if (!hasOrbital) {
        addV(rvC, "RV_med", S.getRVMed(), S.getERVMed(), 2, "km/s");
        if (!has(S.getRVMed()))
            addV(rvC, "RV_avg", S.getRVAvg(), S.getERVAvg(), 2, "km/s");
        if (rvCurve && rvCurve->getNumPoints() > 0) {
            double minRV = rvCurve->getMinRV(), maxRV = rvCurve->getMaxRV();
            if (std::isfinite(minRV) && std::isfinite(maxRV)) {
                double  mid = minRV + (maxRV - minRV) / 2.0;
                QString n   = QString::number(mid, 'f', 2);
                rvC.push_back({"RV_mid", n + " km/s", n});
            }
        }

        addV(rvF, "RV_med", S.getRVMed(), S.getERVMed(), 2, "km/s");
        addV(rvF, "RV_avg", S.getRVAvg(), S.getERVAvg(), 2, "km/s");
        addV(rvF, "ΔRV_max", S.getDeltaRV(), S.getEDeltaRV(), 2, "km/s");
        addPlain(rvF, "log p", S.getLogP(), 2);
        addPlain(rvF, "Timespan", S.getRVTimespan(), 1, "d");
        if (S.getRVNPoints() > 0)
            rvF.push_back({"N RV points", QString::number(S.getRVNPoints()),
                           QString::number(S.getRVNPoints())});
    }

    // ── Compose container with expandable subsections ─────────────────────
    QWidget     *container = new QWidget;
    QVBoxLayout *vLayout   = new QVBoxLayout(container);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(6);

    auto addSub = [&](const QString &title, const std::vector<PropRow> &c,
                      const std::vector<PropRow> &f) {
        if (c.empty() && f.empty())
            return;
        QWidget *compactGrid = buildPropertyGrid(c, valCol, labelCol);
        QWidget *fullGrid    = (f.size() > c.size())
                                   ? buildPropertyGrid(f, valCol, labelCol)
                                   : nullptr;
        vLayout->addWidget(
            createExpandableSectionFrame(title, compactGrid, fullGrid));
    };

    addSub("Astrometry", astroC, astroF);
    addSub("Photometry", photoC, photoF);
    addSub("Atmospheric Parameters", atmosC, atmosF);
    addSub("Radial Velocity", rvC, rvF);

    if (vLayout->count() == 0) {
        QLabel *empty = new QLabel("No catalog data available yet.");
        empty->setStyleSheet(
            "color: gray; font-style: italic; background: transparent;");
        vLayout->addWidget(empty);
    }

    return container;
}

QWidget *SummaryPanel::createOrbitalFitSection() {
    const bool   dark   = PanelUtils::isDarkTheme();
    const QColor valCol = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    const QColor labelCol =
        dark ? QColor(140, 140, 145) : QColor(100, 100, 105);
    auto has = [](double v) { return std::isfinite(v) && v != 0.0; };

    auto rvCurve = _ctx.star->getRVCurve();
    auto bestFit = rvCurve->getBestFit();

    std::vector<PropRow> compact, full;
    auto pushV = [&](std::vector<PropRow> &rows, const QString &l, double v,
                     double e, int p, const QString &u = "") {
        auto d = fmtValErr(v, e, p, u);
        rows.push_back({l, d.display, d.copy});
    };

    pushV(compact, "Period", bestFit->getPeriod(), bestFit->getPeriodError(), 6,
          "d");
    pushV(compact, "K", bestFit->getK(), bestFit->getKError(), 2, "km/s");
    pushV(compact, "γ", bestFit->getGamma(), bestFit->getGammaError(), 2,
          "km/s");
    pushV(compact, "T₀ (ϕ)", bestFit->getPhi(), bestFit->getPhiError(), 4);
    if (bestFit->isEccentric()) {
        pushV(compact, "e", bestFit->getEccentricity(),
              bestFit->getEccentricityError(), 4);
        pushV(compact, "ω", bestFit->getOmega(), bestFit->getOmegaError(), 1,
              "°");
    }
    if (has(bestFit->getRms())) {
        QString n = QString::number(bestFit->getRms(), 'f', 2);
        compact.push_back({"RMS", n + " km/s", n});
    }
    if (!bestFit->getFitMethod().isEmpty())
        compact.push_back(
            {"Method", bestFit->getFitMethod(), bestFit->getFitMethod()});

    // Full set: everything compact has, plus χ², T0 (BJD), reference epoch, t0
    // raw, etc.
    full = compact;
    if (has(bestFit->getChi2())) {
        QString n = QString::number(bestFit->getChi2(), 'f', 2);
        full.push_back({"χ²", n, n});
    }
    if (has(bestFit->getT0BJD())) {
        QString n = QString::number(bestFit->getT0BJD(), 'f', 6);
        full.push_back({"T₀ (BJD)", n, n});
    }
    if (has(bestFit->getReferenceBJD())) {
        QString n = QString::number(bestFit->getReferenceBJD(), 'f', 6);
        full.push_back({"Ref. BJD", n, n});
    }
    if (!bestFit->isEccentric()) {
        full.push_back({"e", "0 (circular)", "0"});
    }

    QWidget *compactGrid = buildPropertyGrid(compact, valCol, labelCol);
    QWidget *fullGrid    = (full.size() > compact.size())
                               ? buildPropertyGrid(full, valCol, labelCol)
                               : nullptr;

    return createExpandableSectionFrame("Orbital Solution", compactGrid,
                                        fullGrid);
}

QWidget* SummaryPanel::createDataInventorySection()
{
    bool dark = PanelUtils::isDarkTheme();
    QColor tagBg     = dark ? QColor(60, 65, 70)   : QColor(230, 235, 240);
    QColor tagBorder = dark ? QColor(80, 85, 90)    : QColor(200, 205, 210);
    QColor tagText   = dark ? QColor(195, 200, 205) : QColor(50, 55, 60);
    QColor checkCol  = QColor(80, 180, 100);
    QColor crossCol  = dark ? QColor(100, 100, 105) : QColor(170, 170, 175);

    auto spectra = _ctx.star->getSpectra();
    auto rvCurve = _ctx.star->getRVCurve();
    auto phot    = _ctx.star->getPhotometry();

    struct Inventory {
        QString label;
        bool available;
        QString detail;
    };

    std::vector<Inventory> items;

    // Spectra
    {
        const int n       = _ctx.star->getNSpectra();
        const int nFitted = _ctx.star->getNFitSpectra();
        QString   detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 total").arg(n);
            if (nFitted > 0)
                parts << QString("%1 fitted").arg(nFitted);
            if (_ctx.star->hasSpectraLoaded()) {
                QSet<QString> instruments;
                for (auto &sp : _ctx.star->getSpectra())
                    if (!sp->getInstrument().isEmpty())
                        instruments.insert(sp->getInstrument());
                if (!instruments.isEmpty()) {
                    QStringList instList(instruments.begin(),
                                         instruments.end());
                    instList.sort();
                    parts << instList.join(", ");
                }
            }
            detail = parts.join(" · ");
        }
        items.push_back({"Spectra", n > 0, detail});
    }

    // RV curve
    {
        const int    n    = _ctx.star->getRVNPoints();
        const double span = _ctx.star->getRVTimespan();
        QString      detail;
        if (n > 0) {
            QStringList parts;
            parts << QString("%1 points").arg(n);
            if (rvCurve && rvCurve->getNumFits() > 0)
                parts << QString("%1 fit(s)").arg(rvCurve->getNumFits());
            if (std::isfinite(span) && span > 0)
                parts << QString("%1 d span").arg(span, 0, 'f', 0);
            detail = parts.join(" · ");
        }
        items.push_back({"RV Curve", n > 0, detail});
    }

    // Light curves
    {
        bool hasLC = false;
        QString detail;
        if (phot) {
            auto sources = phot->getLightcurveSources();
            if (!sources.empty()) {
                hasLC = true;
                QStringList srcList;
                for (auto& s : sources) srcList.append(s);
                detail = srcList.join(", ");
            }
        }
        items.push_back({"Light Curves", hasLC, detail});
    }

    // SED
    {
        bool hasSED = false;
        QString detail;
        if (phot) {
            auto sed = phot->getBestSEDModel();
            if (sed) {
                hasSED = true;
                QStringList parts;
                parts << QString("%1-comp").arg(sed->numComponents);

                auto fmtAsym = [](double val, double up, double down, int prec) -> QString {
                    return QString("%1<sup><small>+%2</small></sup><sub><small>-%3</small></sub>")
                        .arg(val,  0, 'f', prec)
                        .arg(up,   0, 'f', prec)
                        .arg(down, 0, 'f', prec);
                };

                // Show primary component Teff, radius, mass
                if (!sed->components.empty()) {
                    const auto& c1 = sed->components[0];
                    if (c1.teff > 0)
                        parts << QString("T₁=%1 K").arg(c1.teff, 0, 'f', 0);
                    if (c1.radius.value > 0)
                        parts << QString("R₁=%1 R☉").arg(fmtAsym(c1.radius.value, c1.radius.errUp, c1.radius.errDown, 3));
                    if (c1.mass.value > 0)
                        parts << QString("M₁=%1 M☉").arg(fmtAsym(c1.mass.value, c1.mass.errUp, c1.mass.errDown, 3));
                }
                // Show companion Teff, radius, mass if 2-component
                if (sed->numComponents >= 2 && sed->components.size() >= 2) {
                    const auto& c2 = sed->components[1];
                    if (c2.teff > 0)
                        parts << QString("T₂=%1 K").arg(c2.teff, 0, 'f', 0);
                    if (c2.radius.value > 0)
                        parts << QString("R₂=%1 R☉").arg(fmtAsym(c2.radius.value, c2.radius.errUp, c2.radius.errDown, 3));
                    if (c2.mass.value > 0)
                        parts << QString("M₂=%1 M☉").arg(fmtAsym(c2.mass.value, c2.mass.errUp, c2.mass.errDown, 3));
                }
                if (sed->distanceMode > 0)
                    parts << QString("d=%1 pc").arg(sed->distanceMode, 0, 'f', 0);
                if (sed->chi2Reduced > 0)
                    parts << QString("χ²=%1").arg(sed->chi2Reduced, 0, 'f', 2);
                detail = parts.join(" · ");
            }
        }
        items.push_back({"SED Fit", hasSED, detail});
    }

    // Build tag strip
    QWidget* content = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    for (auto& item : items) {
        QHBoxLayout* row = new QHBoxLayout;
        row->setSpacing(8);

        // Status indicator
        QLabel* indicator = new QLabel(item.available ? "●" : "○");
        indicator->setFixedWidth(16);
        indicator->setAlignment(Qt::AlignCenter);
        indicator->setStyleSheet(QString(
            "font-size: 12px; color: %1; background: transparent; border: none;"
        ).arg(item.available ? checkCol.name() : crossCol.name()));
        row->addWidget(indicator);

        // Label
        QLabel* lbl = new QLabel(item.label);
        lbl->setFixedWidth(120);
        lbl->setStyleSheet(QString(
            "font-size: 12px; font-weight: 600; color: %1; background: transparent; border: none;"
        ).arg(tagText.name()));
        row->addWidget(lbl);

        // Detail
        if (!item.detail.isEmpty()) {
            QLabel* det = new QLabel(item.detail);
            det->setTextFormat(Qt::RichText); 
            det->setStyleSheet(QString(
                "font-size: 11px; color: %1; background: transparent; border: none;"
            ).arg(dark ? "#aaa" : "#777"));
            det->setTextInteractionFlags(Qt::TextSelectableByMouse);
            row->addWidget(det, 1);
        } else {
            row->addStretch();
        }

        layout->addLayout(row);
    }

    return createSectionFrame("Data Inventory", content);
}

QWidget *SummaryPanel::createReferencesSection() {
    QStringList bibcodes;
    for (const auto &b : _ctx.star->getBibcodes())
        bibcodes << b;
    std::sort(bibcodes.begin(), bibcodes.end(), std::greater<QString>());

    QWidget     *content = new QWidget;
    QVBoxLayout *layout  = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    _refCardHost     = new QWidget;
    auto *cardLayout = new QVBoxLayout(_refCardHost);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(6);
    layout->addWidget(_refCardHost);

    _refSpinner = makeLoadingRow();
    _refSpinner->setVisible(false);
    layout->addWidget(_refSpinner);

    _pendingRefs = bibcodes;
    appendReferenceBatch(); // first kRefBatchSize cards, synchronously

    return createSectionFrame("References", content);
}

void SummaryPanel::buildReferenceCards(QWidget           *host,
                                       const QStringList &bibcodes) {
    bool   dark        = PanelUtils::isDarkTheme();
    QColor accentColor = dark ? QColor(100, 130, 200) : QColor(60, 100, 180);
    QColor cardBg      = dark ? QColor(48, 50, 56) : QColor(250, 250, 252);
    QColor cardBorder  = dark ? QColor(65, 68, 75) : QColor(215, 218, 225);
    QColor titleColor  = dark ? QColor(225, 228, 235) : QColor(25, 25, 30);
    QColor subtitleCol = dark ? QColor(145, 150, 160) : QColor(100, 105, 115);
    QColor bodyCol     = dark ? QColor(190, 195, 205) : QColor(55, 60, 70);
    QColor abstractBg  = dark ? QColor(42, 44, 50) : QColor(243, 244, 248);
    QColor linkColor   = dark ? QColor(120, 160, 230) : QColor(40, 90, 180);
    QColor loadingCol  = dark ? QColor(110, 115, 125) : QColor(150, 155, 165);

    QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(host->layout());
    if (!layout)
        return;

    QString cardStyle =
        QString("QFrame#refCard { background: %1; border: 1px solid %2; "
                "border-left: 3px solid %3; border-radius: 5px; }")
            .arg(cardBg.name(), cardBorder.name(), accentColor.name());

    QString linkBtnStyle =
        QString("QPushButton { font-size: 10px; color: %1; border: none; "
                "background: transparent; padding: 0 4px; }"
                "QPushButton:hover { color: %2; }")
            .arg(linkColor.name(), titleColor.name());

    // ── ONE DB read for all bibcodes instead of N. ──
    const QMap<QString, BibcodeInfo> cache =
        _refResolver->lookupCacheBatch(bibcodes);

    QStringList toResolve;

    for (const auto &bib : bibcodes) {
        QString adsUrl =
            QString("https://ui.adsabs.harvard.edu/abs/%1/abstract").arg(bib);
        QString metaStr = formatBibcodeMeta(bib);

        QFrame *card = new QFrame;
        card->setObjectName("refCard");
        card->setStyleSheet(cardStyle);
        card->setToolTip(bib);

        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(3);

        QLabel *titleLabel = new QLabel(bib);
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setWordWrap(true);
        titleLabel->setStyleSheet(
            QString("font-size: 12px; font-weight: 600; color: %1; "
                    "background: transparent; border: none;")
                .arg(titleColor.name()));
        cardLayout->addWidget(titleLabel);

        QLabel *subtitleLabel = new QLabel(metaStr);
        subtitleLabel->setTextFormat(Qt::PlainText);
        subtitleLabel->setWordWrap(true);
        subtitleLabel->setStyleSheet(
            QString("font-size: 10px; color: %1; background: transparent; "
                    "border: none;")
                .arg(subtitleCol.name()));
        cardLayout->addWidget(subtitleLabel);

        QWidget     *btnRow    = new QWidget;
        QHBoxLayout *btnLayout = new QHBoxLayout(btnRow);
        btnLayout->setContentsMargins(0, 2, 0, 0);
        btnLayout->setSpacing(8);

        QPushButton *abstractBtn = new QPushButton("\u25b8 Abstract");
        abstractBtn->setFlat(true);
        abstractBtn->setFixedHeight(20);
        abstractBtn->setCursor(Qt::PointingHandCursor);
        abstractBtn->setStyleSheet(linkBtnStyle);
        abstractBtn->setVisible(false);
        btnLayout->addWidget(abstractBtn);

        QPushButton *adsScrapeBtn =
            new QPushButton("\u21bb Fetch from NASA/ADS");
        adsScrapeBtn->setFlat(true);
        adsScrapeBtn->setFixedHeight(20);
        adsScrapeBtn->setCursor(Qt::PointingHandCursor);
        adsScrapeBtn->setToolTip(
            "CrossRef has no record. Click to scrape the ADS page once.");
        adsScrapeBtn->setStyleSheet(linkBtnStyle);
        adsScrapeBtn->setVisible(false);
        btnLayout->addWidget(adsScrapeBtn);

        btnLayout->addStretch();

        QPushButton *adsBtn = new QPushButton("Open on ADS \u2197");
        adsBtn->setFlat(true);
        adsBtn->setFixedHeight(20);
        adsBtn->setCursor(Qt::PointingHandCursor);
        adsBtn->setToolTip(adsUrl);
        adsBtn->setStyleSheet(linkBtnStyle);
        connect(adsBtn, &QPushButton::clicked, this,
                [adsUrl]() { QDesktopServices::openUrl(QUrl(adsUrl)); });
        btnLayout->addWidget(adsBtn);

        cardLayout->addWidget(btnRow);

        QLabel *loadingLabel = new QLabel("Resolving\u2026");
        loadingLabel->setTextFormat(Qt::PlainText);
        loadingLabel->setStyleSheet(
            QString("font-size: 10px; font-style: italic; color: %1; "
                    "background: transparent; border: none;")
                .arg(loadingCol.name()));
        loadingLabel->setVisible(false);
        cardLayout->addWidget(loadingLabel);

        QLabel *abstractLabel = new QLabel;
        abstractLabel->setTextFormat(Qt::PlainText);
        abstractLabel->setWordWrap(true);
        abstractLabel->setStyleSheet(
            QString("font-size: 11px; color: %1; background: %2; "
                    "border: none; border-radius: 3px; padding: 6px 8px;")
                .arg(bodyCol.name(), abstractBg.name()));
        abstractLabel->setVisible(false);
        cardLayout->addWidget(abstractLabel);

        connect(abstractBtn, &QPushButton::clicked, this,
                [abstractBtn, abstractLabel]() {
                    bool show = !abstractLabel->isVisible();
                    abstractLabel->setVisible(show);
                    abstractBtn->setText(show ? "\u25be Abstract"
                                              : "\u25b8 Abstract");
                });

        layout->addWidget(card);

        auto populateCard = [titleLabel, subtitleLabel, loadingLabel,
                             abstractLabel, abstractBtn, titleColor,
                             subtitleCol, metaStr](const BibcodeInfo &info) {
            titleLabel->setText(info.title);
            titleLabel->setStyleSheet(
                QString("font-size: 12px; font-weight: 600; color: %1; "
                        "background: transparent; border: none;")
                    .arg(titleColor.name()));
            titleLabel->setCursor(Qt::PointingHandCursor);
            makeCopyable(titleLabel, info.title);

            subtitleLabel->setText(info.authors.isEmpty()
                                       ? metaStr
                                       : info.authors + "  \u00b7  " + metaStr);
            if (!info.abstract.isEmpty()) {
                abstractLabel->setText(info.abstract);
                abstractBtn->setVisible(true);
            }
            loadingLabel->setVisible(false);
        };

        auto it = cache.find(bib);
        if (it != cache.end() && !it->title.isEmpty()) {
            populateCard(*it);
        } else {
            loadingLabel->setVisible(true);
            toResolve << bib;

            connect(_refResolver, &CrossRefResolver::resolved, card,
                    [bib, populateCard, adsScrapeBtn](
                        const QString &resolvedBib, const BibcodeInfo &info) {
                        if (resolvedBib != bib)
                            return;
                        adsScrapeBtn->setVisible(false);
                        populateCard(info);
                    });

            connect(
                _refResolver, &CrossRefResolver::fetchFailed, card,
                [bib, loadingLabel, adsScrapeBtn](const QString &failedBib) {
                    if (failedBib != bib)
                        return;
                    loadingLabel->setVisible(false);
                    loadingLabel->setText("Resolving\u2026");
                    adsScrapeBtn->setVisible(true);
                    adsScrapeBtn->setEnabled(true);
                });

            connect(adsScrapeBtn, &QPushButton::clicked, this,
                    [this, bib, loadingLabel, adsScrapeBtn]() {
                        adsScrapeBtn->setEnabled(false);
                        adsScrapeBtn->setVisible(false);
                        loadingLabel->setText("Fetching from NASA/ADS\u2026");
                        loadingLabel->setVisible(true);
                        _refResolver->resolveViaADS(bib);
                    });
        }
    }

    if (!toResolve.isEmpty())
        _refResolver->resolve(toResolve);
}

void SummaryPanel::addReferenceCards(QWidget           *host,
                                     const QStringList &bibcodes) {
    QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(host->layout());
    if (!layout || bibcodes.isEmpty())
        return;

    bool   dark        = PanelUtils::isDarkTheme();
    QColor accentColor = dark ? QColor(100, 130, 200) : QColor(60, 100, 180);
    QColor cardBg      = dark ? QColor(48, 50, 56) : QColor(250, 250, 252);
    QColor cardBorder  = dark ? QColor(65, 68, 75) : QColor(215, 218, 225);
    QColor titleColor  = dark ? QColor(225, 228, 235) : QColor(25, 25, 30);
    QColor subtitleCol = dark ? QColor(145, 150, 160) : QColor(100, 105, 115);
    QColor bodyCol     = dark ? QColor(190, 195, 205) : QColor(55, 60, 70);
    QColor abstractBg  = dark ? QColor(42, 44, 50) : QColor(243, 244, 248);
    QColor linkColor   = dark ? QColor(120, 160, 230) : QColor(40, 90, 180);
    QColor loadingCol  = dark ? QColor(110, 115, 125) : QColor(150, 155, 165);

    QString cardStyle =
        QString("QFrame#refCard { background: %1; border: 1px solid %2; "
                "border-left: 3px solid %3; border-radius: 5px; }")
            .arg(cardBg.name(), cardBorder.name(), accentColor.name());

    QString linkBtnStyle =
        QString("QPushButton { font-size: 10px; color: %1; border: none; "
                "background: transparent; padding: 0 4px; }"
                "QPushButton:hover { color: %2; }")
            .arg(linkColor.name(), titleColor.name());

    // One DB read for this page.
    const QMap<QString, BibcodeInfo> cache =
        _refResolver->lookupCacheBatch(bibcodes);

    QStringList toResolve;

    for (const auto &bib : bibcodes) {
        QString adsUrl =
            QString("https://ui.adsabs.harvard.edu/abs/%1/abstract").arg(bib);
        QString metaStr = formatBibcodeMeta(bib);

        QFrame *card = new QFrame;
        card->setObjectName("refCard");
        card->setStyleSheet(cardStyle);
        card->setToolTip(bib);

        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(3);

        QLabel *titleLabel = new QLabel(bib);
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setWordWrap(true);
        titleLabel->setStyleSheet(
            QString("font-size: 12px; font-weight: 600; color: %1; "
                    "background: transparent; border: none;")
                .arg(titleColor.name()));
        cardLayout->addWidget(titleLabel);

        QLabel *subtitleLabel = new QLabel(metaStr);
        subtitleLabel->setTextFormat(Qt::PlainText);
        subtitleLabel->setWordWrap(true);
        subtitleLabel->setStyleSheet(
            QString("font-size: 10px; color: %1; background: transparent; "
                    "border: none;")
                .arg(subtitleCol.name()));
        cardLayout->addWidget(subtitleLabel);

        QWidget     *btnRow    = new QWidget;
        QHBoxLayout *btnLayout = new QHBoxLayout(btnRow);
        btnLayout->setContentsMargins(0, 2, 0, 0);
        btnLayout->setSpacing(8);

        QPushButton *abstractBtn = new QPushButton("\u25b8 Abstract");
        abstractBtn->setFlat(true);
        abstractBtn->setFixedHeight(20);
        abstractBtn->setCursor(Qt::PointingHandCursor);
        abstractBtn->setStyleSheet(linkBtnStyle);
        abstractBtn->setVisible(false);
        btnLayout->addWidget(abstractBtn);

        QPushButton *adsScrapeBtn =
            new QPushButton("\u21bb Fetch from NASA/ADS");
        adsScrapeBtn->setFlat(true);
        adsScrapeBtn->setFixedHeight(20);
        adsScrapeBtn->setCursor(Qt::PointingHandCursor);
        adsScrapeBtn->setToolTip(
            "CrossRef has no record. Click to scrape the ADS page once.");
        adsScrapeBtn->setStyleSheet(linkBtnStyle);
        adsScrapeBtn->setVisible(false);
        btnLayout->addWidget(adsScrapeBtn);

        btnLayout->addStretch();

        QPushButton *adsBtn = new QPushButton("Open on ADS \u2197");
        adsBtn->setFlat(true);
        adsBtn->setFixedHeight(20);
        adsBtn->setCursor(Qt::PointingHandCursor);
        adsBtn->setToolTip(adsUrl);
        adsBtn->setStyleSheet(linkBtnStyle);
        connect(adsBtn, &QPushButton::clicked, this,
                [adsUrl]() { QDesktopServices::openUrl(QUrl(adsUrl)); });
        btnLayout->addWidget(adsBtn);

        cardLayout->addWidget(btnRow);

        QLabel *loadingLabel = new QLabel("Resolving\u2026");
        loadingLabel->setTextFormat(Qt::PlainText);
        loadingLabel->setStyleSheet(
            QString("font-size: 10px; font-style: italic; color: %1; "
                    "background: transparent; border: none;")
                .arg(loadingCol.name()));
        loadingLabel->setVisible(false);
        cardLayout->addWidget(loadingLabel);

        QLabel *abstractLabel = new QLabel;
        abstractLabel->setTextFormat(Qt::PlainText);
        abstractLabel->setWordWrap(true);
        abstractLabel->setStyleSheet(
            QString("font-size: 11px; color: %1; background: %2; "
                    "border: none; border-radius: 3px; padding: 6px 8px;")
                .arg(bodyCol.name(), abstractBg.name()));
        abstractLabel->setVisible(false);
        cardLayout->addWidget(abstractLabel);

        connect(abstractBtn, &QPushButton::clicked, this,
                [abstractBtn, abstractLabel]() {
                    bool show = !abstractLabel->isVisible();
                    abstractLabel->setVisible(show);
                    abstractBtn->setText(show ? "\u25be Abstract"
                                              : "\u25b8 Abstract");
                });

        layout->addWidget(card);

        auto populateCard = [titleLabel, subtitleLabel, loadingLabel,
                             abstractLabel, abstractBtn, titleColor,
                             subtitleCol, metaStr](const BibcodeInfo &info) {
            titleLabel->setText(info.title);
            titleLabel->setStyleSheet(
                QString("font-size: 12px; font-weight: 600; color: %1; "
                        "background: transparent; border: none;")
                    .arg(titleColor.name()));
            titleLabel->setCursor(Qt::PointingHandCursor);
            makeCopyable(titleLabel, info.title);

            subtitleLabel->setText(info.authors.isEmpty()
                                       ? metaStr
                                       : info.authors + "  \u00b7  " + metaStr);
            if (!info.abstract.isEmpty()) {
                abstractLabel->setText(info.abstract);
                abstractBtn->setVisible(true);
            }
            loadingLabel->setVisible(false);
        };

        auto it = cache.find(bib);
        if (it != cache.end() && !it->title.isEmpty()) {
            populateCard(*it);
        } else {
            loadingLabel->setVisible(true);
            toResolve << bib;

            connect(_refResolver, &CrossRefResolver::resolved, card,
                    [bib, populateCard, adsScrapeBtn](
                        const QString &resolvedBib, const BibcodeInfo &info) {
                        if (resolvedBib != bib)
                            return;
                        adsScrapeBtn->setVisible(false);
                        populateCard(info);
                    });

            connect(
                _refResolver, &CrossRefResolver::fetchFailed, card,
                [bib, loadingLabel, adsScrapeBtn](const QString &failedBib) {
                    if (failedBib != bib)
                        return;
                    loadingLabel->setVisible(false);
                    loadingLabel->setText("Resolving\u2026");
                    adsScrapeBtn->setVisible(true);
                    adsScrapeBtn->setEnabled(true);
                });

            connect(adsScrapeBtn, &QPushButton::clicked, this,
                    [this, bib, loadingLabel, adsScrapeBtn]() {
                        adsScrapeBtn->setEnabled(false);
                        adsScrapeBtn->setVisible(false);
                        loadingLabel->setText("Fetching from NASA/ADS\u2026");
                        loadingLabel->setVisible(true);
                        _refResolver->resolveViaADS(bib);
                    });
        }
    }

    if (!toResolve.isEmpty())
        _refResolver->resolve(toResolve);
}

void SummaryPanel::appendReferenceBatch() {
    if (!_refCardHost || _pendingRefs.isEmpty())
        return;

    QStringList batch;
    for (int i = 0; i < kRefBatchSize && !_pendingRefs.isEmpty(); ++i)
        batch << _pendingRefs.takeFirst();

    addReferenceCards(_refCardHost, batch);

    if (_refSpinner)
        _refSpinner->setVisible(!_pendingRefs.isEmpty());
}

void SummaryPanel::onSummaryScrolled() {
    if (!_refCardHost || _pendingRefs.isEmpty() || _loadingMoreRefs)
        return;

    QScrollBar   *sb         = _scroll->verticalScrollBar();
    constexpr int kTriggerPx = 120; // start loading a bit early
    if (sb->value() < sb->maximum() - kTriggerPx)
        return;

    _loadingMoreRefs = true;
    if (_refSpinner)
        _refSpinner->setVisible(true);

    // Defer so the spinner actually paints before the (synchronous) card build.
    QTimer::singleShot(120, this, [this]() {
        appendReferenceBatch();
        _loadingMoreRefs = false;
    });
}

QWidget *SummaryPanel::makeLoadingRow() {
    const bool dark = PanelUtils::isDarkTheme();

    QWidget     *row = new QWidget;
    QHBoxLayout *l   = new QHBoxLayout(row);
    l->setContentsMargins(0, 4, 0, 6);
    l->setSpacing(8);

    // Indeterminate progress bar = built-in animated busy indicator.
    QProgressBar *spin = new QProgressBar;
    spin->setRange(0, 0);
    spin->setFixedSize(90, 6);
    spin->setTextVisible(false);
    l->addWidget(spin, 0, Qt::AlignVCenter);

    QLabel *lbl = new QLabel("Loading more references\u2026");
    lbl->setStyleSheet(QString("font-size: 10px; font-style: italic; color: %1;"
                               " background: transparent; border: none;")
                           .arg(dark ? "#8a8f99" : "#969aa3"));
    l->addWidget(lbl);
    l->addStretch();
    return row;
}

QFrame* SummaryPanel::createSectionFrame(const QString& title, QWidget* content)
{
    bool dark = PanelUtils::isDarkTheme();
    QColor cardBg  = dark ? QColor(50, 50, 55) : QColor(248, 248, 250);
    QColor border  = dark ? QColor(70, 70, 75)  : QColor(210, 210, 215);
    QColor titleCol = dark ? QColor(180, 180, 185) : QColor(90, 90, 95);

    QFrame* frame = new QFrame;
    frame->setFrameShape(QFrame::NoFrame);
    frame->setStyleSheet(QString(
        "QFrame#sectionCard { background: %1; border: 1px solid %2; border-radius: 6px; }"
    ).arg(cardBg.name(), border.name()));
    frame->setObjectName("sectionCard");

    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    if (!title.isEmpty()) {
        QLabel* titleLabel = new QLabel(title);
        titleLabel->setStyleSheet(QString(
            "font-size: 11px; font-weight: 600; color: %1; "
            "text-transform: uppercase; letter-spacing: 1px; "
            "padding-bottom: 4px; border: none; background: transparent;"
        ).arg(titleCol.name()));
        layout->addWidget(titleLabel);
    }

    layout->addWidget(content);
    return frame;
}

QWidget *SummaryPanel::createCompanionSection() {
    const double mMin    = _ctx.star->getCompMassMin();
    const double eMin    = _ctx.star->getECompMassMin();
    const double mTrue   = _ctx.star->getCompMassTrue();
    const double eTrue   = _ctx.star->getECompMassTrue();
    const bool   hasMin  = std::isfinite(mMin) && mMin > 0.0;
    const bool   hasTrue = std::isfinite(mTrue) && mTrue > 0.0;

    // Also gather inputs for f(M) and a, so the section can show *something*
    // even when only some pieces are present.
    const MassInputs &in = _cachedMassInputs;

    const bool hasMassFunc   = in.valid;
    const bool hasSeparation = in.valid && std::isfinite(in.P) && in.P > 0.0;
    const bool hasQ          = Star::isSet(_ctx.star->getPhotQ());

    if (!hasMin && !hasTrue && !hasMassFunc && !hasSeparation && !hasQ)
        return nullptr;

    const bool   dark   = PanelUtils::isDarkTheme();
    const QColor valCol = dark ? QColor(220, 220, 225) : QColor(30, 30, 35);
    const QColor labelCol =
        dark ? QColor(140, 140, 145) : QColor(100, 100, 105);

    std::vector<PropRow> rows;

    if (hasMin) {
        auto d = fmtValErr(mMin, eMin, 3, "M☉");
        rows.push_back({"M₂ (min)", d.display, d.copy});
    }
    if (hasTrue) {
        auto d = fmtValErr(mTrue, eTrue, 3, "M☉");
        rows.push_back({"M₂ (true)", d.display, d.copy});
    }
    if (hasMassFunc) {
        const double f = massFunctionMsun(in.K, in.P, in.e);
        QString      n = QString::number(f, 'f', 5);
        rows.push_back({"f(M)", n + " M☉", n});
    }
    if (hasSeparation) {
        const double M2    = hasTrue ? mTrue : (hasMin ? mMin : 0.0);
        const double Mtot  = in.M1 + M2;
        const double Py    = in.P / 365.25;
        const double aAU   = std::cbrt(Mtot * Py * Py);
        const double aRsun = aAU * 215.032;
        QString      n     = QString::number(aRsun, 'f', 2);
        QString      unit  = hasTrue ? " R☉" : " R☉ (min)";
        rows.push_back({"a", n + unit, n});
    }
    if (hasQ) {
        auto d =
            fmtValErr(_ctx.star->getPhotQ(), _ctx.star->getPhotEQ(), 3, "");
        rows.push_back({"q (LC)", d.display, d.copy});
    }

    QWidget *grid = buildPropertyGrid(rows, valCol, labelCol);
    return createSectionFrame("Companion", grid);
}

QColor SummaryPanel::logPColor(double logP) const
{
    // Very negative = highly variable = important
    if (std::isnan(logP) || logP == 0.0)
        return PanelUtils::isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (logP < -10.0)
        return QColor(220, 50, 50);    // Red — extremely significant
    if (logP < -5.0)
        return QColor(230, 150, 30);   // Orange — significant
    if (logP < -2.0)
        return QColor(200, 200, 50);   // Yellow — marginal
    return QColor(80, 180, 80);        // Green — consistent with constant
}

QColor SummaryPanel::deltaRVColor(double deltaRV) const
{
    if (std::isnan(deltaRV) || deltaRV == 0.0)
        return PanelUtils::isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (deltaRV > 100.0)
        return QColor(220, 50, 50);
    if (deltaRV > 30.0)
        return QColor(230, 150, 30);
    if (deltaRV > 10.0)
        return QColor(200, 200, 50);
    return QColor(80, 180, 80);
}

QColor SummaryPanel::specClassColor(const QString& specClass) const
{
    if (specClass.isEmpty()) return PanelUtils::isDarkTheme() ? QColor(140, 140, 140) : QColor(120, 120, 120);
    QChar first = specClass.at(0).toUpper();
    if (first == 'O') return QColor(100, 140, 255);
    if (first == 'B') return QColor(130, 170, 255);
    if (first == 'A') return QColor(180, 200, 255);
    if (first == 'F') return QColor(255, 255, 200);
    if (first == 'G') return QColor(255, 230, 140);
    if (first == 'K') return QColor(255, 180, 100);
    if (first == 'M') return QColor(255, 120, 80);
    // Subdwarf / white dwarf prefixes
    if (specClass.startsWith("sd", Qt::CaseInsensitive))
        return QColor(130, 170, 255);
    return PanelUtils::isDarkTheme() ? QColor(170, 170, 170) : QColor(100, 100, 100);
}

QColor SummaryPanel::accentTextColor(const QColor& accent) const
{
    // Return white or black text depending on accent luminance
    return (accent.lightnessF() > 0.55) ? QColor(20, 20, 20) : QColor(240, 240, 240);
}

QFrame *SummaryPanel::createExpandableSectionFrame(const QString &title,
                                                   QWidget *compactContent,
                                                   QWidget *expandedContent) {
    // No expansion if no extra content
    if (!expandedContent)
        return createSectionFrame(title, compactContent);

    QFrame      *frame  = createSectionFrame(title, compactContent);
    QVBoxLayout *layout = qobject_cast<QVBoxLayout *>(frame->layout());
    if (!layout)
        return frame;

    expandedContent->setVisible(false);
    layout->addWidget(expandedContent);

    const bool   dark   = PanelUtils::isDarkTheme();
    QPushButton *toggle = new QPushButton("Show all ▾");
    toggle->setFlat(true);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setStyleSheet(
        QString("QPushButton { font-size: 10px; color: %1; background: "
                "transparent; "
                "border: none; padding: 2px 0; text-align: left; }"
                "QPushButton:hover { color: %2; }")
            .arg(dark ? "#8aa3c8" : "#3a5a90", dark ? "#cfdaee" : "#1d3160"));
    layout->addWidget(toggle, 0, Qt::AlignLeft);

    QObject::connect(toggle, &QPushButton::clicked, this,
                     [compactContent, expandedContent, toggle]() {
                         const bool showAll = !expandedContent->isVisible();
                         compactContent->setVisible(!showAll);
                         expandedContent->setVisible(showAll);
                         toggle->setText(showAll ? "Show less ▴"
                                                 : "Show all ▾");
                     });

    return frame;
}

bool SummaryPanel::MassInputs::sameAs(const MassInputs &o) const noexcept {
    auto eq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b))
            return true;
        if (std::isnan(a) || std::isnan(b))
            return false;
        return std::abs(a - b) <= 1e-12 * std::max(1.0, std::abs(a));
    };
    return valid == o.valid && hasIncl == o.hasIncl && eq(P, o.P) &&
           eq(eP, o.eP) && eq(K, o.K) && eq(eK, o.eK) && eq(M1, o.M1) &&
           eq(eM1, o.eM1) && eq(e, o.e) && eq(ee, o.ee) && eq(sini, o.sini) &&
           eq(esini, o.esini);
}

void SummaryPanel::ensureCompanionMasses() {
    MassInputs in;

    auto                   rvCurve = _ctx.star->getRVCurve();
    std::shared_ptr<RVFit> fit     = rvCurve ? rvCurve->getBestFit() : nullptr;

    if (fit && fit->getPeriod() > 0) {
        in.P  = fit->getPeriod();
        in.eP = fit->getPeriodError();
        in.K  = fit->getK();
        in.eK = fit->getKError();
        if (fit->isEccentric()) {
            in.e  = fit->getEccentricity();
            in.ee = fit->getEccentricityError();
        }
    } else {
        in.P  = _ctx.star->getRVPeriod();
        in.eP = _ctx.star->getRVEPeriod();
        in.K  = _ctx.star->getRVK();
        in.eK = _ctx.star->getRVEK();
        if (Star::isSet(_ctx.star->getRVEcc()))
            in.e = _ctx.star->getRVEcc();
    }
    in.M1  = _ctx.star->getSedMass1();
    in.eM1 = _ctx.star->getSedEMass1();

    const double iDeg = _ctx.star->getPhotIncl();
    if (Star::isSet(iDeg) && iDeg > 0.0) {
        constexpr double D2R   = M_PI / 180.0;
        const double     iRad  = iDeg * D2R;
        const double     eiDeg = _ctx.star->getPhotEIncl();
        in.hasIncl             = true;
        in.sini                = std::sin(iRad);
        in.esini               = (std::isfinite(eiDeg) && eiDeg > 0.0)
                                     ? std::abs(std::cos(iRad)) * eiDeg * D2R
                                     : 0.0;
    }

    in.valid = std::isfinite(in.P) && in.P > 0.0 && std::isfinite(in.K) &&
               in.K > 0.0 && std::isfinite(in.M1) && in.M1 > 0.0;

    if (_hasMassCache && _cachedMassInputs.sameAs(in))
        return;

    _cachedMassInputs = in;
    _hasMassCache     = true;

    if (!in.valid) {
        _cachedMassMin  = {};
        _cachedMassTrue = {};
    } else {
        const double mMin = m2Of(in.P, in.K, in.M1, in.e, 1.0);
        const double sMin = propagateM2Error(in.P, in.eP, in.K, in.eK, in.M1,
                                             in.eM1, in.e, in.ee, 1.0, 0.0);
        _cachedMassMin    = {mMin, sMin};

        if (in.hasIncl) {
            const double mT = m2Of(in.P, in.K, in.M1, in.e, in.sini);
            const double sT =
                propagateM2Error(in.P, in.eP, in.K, in.eK, in.M1, in.eM1, in.e,
                                 in.ee, in.sini, in.esini);
            _cachedMassTrue = {mT, sT};
        } else {
            _cachedMassTrue = {};
        }
    }

    // Persist to Star only when truly different.
    Star &s   = *_ctx.star;
    auto  neq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b))
            return false;
        if (std::isnan(a) || std::isnan(b))
            return true;
        return std::abs(a - b) > 1e-9 * std::max(1.0, std::abs(a));
    };

    bool changed = false;
    if (neq(s.getCompMassMin(), _cachedMassMin.value)) {
        s.setCompMassMin(_cachedMassMin.value);
        changed = true;
    }
    if (neq(s.getECompMassMin(), _cachedMassMin.error)) {
        s.setECompMassMin(_cachedMassMin.error);
        changed = true;
    }
    if (neq(s.getCompMassTrue(), _cachedMassTrue.value)) {
        s.setCompMassTrue(_cachedMassTrue.value);
        changed = true;
    }
    if (neq(s.getECompMassTrue(), _cachedMassTrue.error)) {
        s.setECompMassTrue(_cachedMassTrue.error);
        changed = true;
    }
    if (changed)
        s.markSummaryDirty();
}