#include "SummaryPanel.h"
#include "PanelUtils.h"

#include "controllers/ApplicationController.h"
#include "dialogs/ReidentifyStarDialog.h"
#include "models/AsymmetricErrors.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "db/DatabaseManager.h"
#include "kinematics/StarKinematics.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/CrossRefResolver.h"
#include "utils/UiIcons.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
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

// --- Theme-derived colours -------------------------------------------------
// All card/panel chrome is derived from the active theme (published by
// ThemeManager) so panels read as a single, uniformly elevated surface that
// matches the theme instead of hardcoded near-white/dark grey.

// Render a monochrome CURRENTCOLOR SVG template into a themed icon.
QIcon themedSvgIcon(const QString& resourcePath, const QColor& color, int px = 16) {
    QFile f(resourcePath);
    if (!f.open(QFile::ReadOnly | QFile::Text)) return QIcon();
    QString svg = QString::fromUtf8(f.readAll());
    svg.replace("CURRENTCOLOR", color.name(QColor::HexRgb));

    QSvgRenderer renderer(svg.toUtf8());
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Render into the logical px×px rect (the pixmap carries the dpr) so the
    // glyph fills and is centred instead of landing in a corner.
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();
    return QIcon(pm);
}

// Blend two colours by t (0 -> a, 1 -> b).
QColor blendColor(const QColor& a, const QColor& b, double t) {
    return QColor(
        static_cast<int>(a.red()   + (b.red()   - a.red())   * t),
        static_cast<int>(a.green() + (b.green() - a.green()) * t),
        static_cast<int>(a.blue()  + (b.blue()  - a.blue())  * t));
}

// Primary readable text on the elevated surface.
QColor primaryTextColor() { return PanelUtils::themeFg(); }

// Secondary/label text: foreground blended toward the surface.
QColor mutedTextColor() {
    return blendColor(PanelUtils::themeFg(), PanelUtils::themeSurface(), 0.40);
}

// Hairline border around cards: surface nudged toward the foreground a touch.
QColor sectionBorderColor() {
    return blendColor(PanelUtils::themeSurface(), PanelUtils::themeFg(), 0.18);
}

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
// Formats "v ± e unit"; when an asymmetric interval is set (errUp/errDown
// finite, see AsymmetricErrors.h) renders "v ⁺ᵘ₋d unit" instead, using the
// same rich-text superscript/subscript style as the SED inventory.
inline ValDisp fmtValErr(double v, double err, int prec,
                         const QString &unit = "",
                         double errUp = AsymErr::unset,
                         double errDown = AsymErr::unset) {
    QString num = QString::number(v, 'f', prec);
    QString s   = num;
    const double up   = AsymErr::upOr(errUp, err);
    const double down = AsymErr::downOr(errDown, err);
    if (AsymErr::hasAsymmetric(errUp, errDown) &&
        std::isfinite(up) && std::isfinite(down) && (up > 0.0 || down > 0.0)) {
        if (up == down)
            s += QString(" ± %1").arg(up, 0, 'f', prec);
        else
            s += QString("<sup><small>+%1</small></sup>"
                         "<sub><small>−%2</small></sub>")
                     .arg(up, 0, 'f', prec)
                     .arg(down, 0, 'f', prec);
    } else if (std::isfinite(err) && err > 0.0) {
        s += QString(" ± %1").arg(err, 0, 'f', prec);
    }
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

// Solve  M2³·sin³i = f·(M1+M2)²  for M2.
double solveCompanionMass(double f, double M1, double sini) {
    if (!std::isfinite(f) || f <= 0.0 || !std::isfinite(M1) || M1 <= 0.0 ||
        !std::isfinite(sini) || sini <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();

    // Reduce to the edge-on form  M2³ = fp·(M1+M2)²  with fp = f / sin³i.
    const double fp = f / (sini * sini * sini);

    // Fixed-point pre-iteration  M2 = cbrt(fp·(M1+M2)²).  Its derivative at the
    // root is (2/3)·M2/(M1+M2) < 1, so it is a contraction that climbs to the
    // root monotonically from below for *any* seed. A bare Newton step, by
    // contrast, overshoots into M2 < 0 for massive companions (q ≳ 1) — the
    // seed lands left of the curve's minimum where g is still decreasing — and
    // the old clamp then trapped it at ~1e-6 (i.e. a spurious M2 ≈ 0).
    double M2 = std::cbrt(fp) * std::cbrt(M1 * M1); // small-M2 seed
    for (int i = 0; i < 80; ++i) {
        const double sum  = M1 + M2;
        const double next = std::cbrt(fp * sum * sum);
        const bool   done = std::abs(next - M2) < 1e-13 * std::max(next, 1e-9);
        M2 = next;
        if (done)
            break;
    }

    // Newton polish for quadratic convergence on g(M2) = M2³ - fp·(M1+M2)².
    for (int i = 0; i < 20; ++i) {
        const double sum = M1 + M2;
        const double g   = M2 * M2 * M2 - fp * sum * sum;
        const double gp  = 3.0 * M2 * M2 - 2.0 * fp * sum;
        if (gp <= 0.0)
            break; // left of the minimum; pre-iteration already has the root
        const double dM = g / gp;
        const double cand = M2 - dM;
        if (cand <= 0.0)
            break; // reject an overshoot, keep the pre-iteration value
        M2 = cand;
        if (std::abs(dM) < 1e-12 * std::max(M2, 1e-6))
            break;
    }
    return M2;
}

inline double m2Of(double P, double K, double M1, double e, double sini) {
    return solveCompanionMass(massFunctionMsun(K, P, e), M1, sini);
}

// Linearised error propagation via central differences.
// Roughly 10 solver calls - microseconds vs. milliseconds for the MC.
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

// ── Two-piece-Gaussian Monte-Carlo propagation ──────────────────────────
// As soon as an input carries an asymmetric interval, linearised Gaussian
// propagation is biased (and adding per-side errors in quadrature is worse,
// Barlow 2003). Instead each input's posterior is reconstructed from
// (v, σ₊, σ₋) as a two-piece ("dimidiated") Gaussian — z ~ N(0,1) scaled by
// σ₊ above the centre and σ₋ below it. Unlike the continuous split normal,
// this reproduces the stored 15.9/50/84.1 percentiles *exactly*, which is
// precisely the information (v, σ₊, σ₋) encodes. Inputs are drawn jointly
// (independently of each other) and the target is evaluated per draw; the
// returned up/down are the distances from `central` to the 84.1/15.9
// percentiles — the same convention the fit solvers use. Bounded inputs
// are redrawn until they land inside their physical range (truncation).
class SplitNormalMC {
  public:
    // sym is the legacy symmetric σ; up/down (NaN = unset) override it.
    void add(double v, double sym, double up, double down, double lo,
             double hi) {
        if (AsymErr::hasAsymmetric(up, down))
            _anyAsym = true;
        Input in;
        in.v    = v;
        in.up   = std::max(0.0, AsymErr::upOr(up, sym));
        in.down = std::max(0.0, AsymErr::downOr(down, sym));
        if (!std::isfinite(in.up))   in.up = 0.0;
        if (!std::isfinite(in.down)) in.down = 0.0;
        in.lo = lo;
        in.hi = hi;
        _inputs.push_back(in);
    }

    // Only worth running when some input is genuinely two-sided; otherwise
    // the linearised path is equivalent and much cheaper.
    bool anyAsymmetric() const { return _anyAsym; }

    // eval receives the drawn inputs (in add() order); NaN results are
    // skipped. Returns false when too few draws evaluate to a finite mass.
    template <typename F>
    bool run(double central, F &&eval, double &outUp, double &outDown,
             int n = 20000) const {
        std::mt19937_64 rng(0x5eedULL);
        std::normal_distribution<double> gauss(0.0, 1.0);

        std::vector<double> x(_inputs.size());
        std::vector<double> out;
        out.reserve(n);
        for (int k = 0; k < n; ++k) {
            for (size_t j = 0; j < _inputs.size(); ++j) {
                const auto &in = _inputs[j];
                double      d  = in.v;
                if (in.up > 0.0 || in.down > 0.0) {
                    bool inside = false;
                    for (int attempt = 0; attempt < 50 && !inside; ++attempt) {
                        const double z = gauss(rng);
                        d = in.v + z * (z >= 0.0 ? in.up : in.down);
                        inside = d >= in.lo && d <= in.hi;
                    }
                    if (!inside)
                        d = std::clamp(d, in.lo, in.hi);
                }
                x[j] = d;
            }
            const double m = eval(x);
            if (std::isfinite(m))
                out.push_back(m);
        }
        if (out.size() < 100)
            return false;
        std::sort(out.begin(), out.end());
        auto pct = [&](double p) {
            const double idx = p * (out.size() - 1);
            const size_t lo  = static_cast<size_t>(std::floor(idx));
            const size_t hi  = static_cast<size_t>(std::ceil(idx));
            const double w   = idx - lo;
            return out[lo] * (1.0 - w) + out[hi] * w;
        };
        outUp   = std::max(0.0, pct(0.841) - central);
        outDown = std::max(0.0, central - pct(0.159));
        return true;
    }

  private:
    struct Input {
        double v, up, down, lo, hi;
    };
    std::vector<Input> _inputs;
    bool               _anyAsym = false;
};

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

SummaryPanel::SummaryPanel(const Context& ctx, QWidget* parent, bool deferPopulate)
    : DetailPanel(ctx, parent)
{
    setupUi();
    if (deferPopulate)
        showLoadingShimmer(1);
    else
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

    _refResolver = new CrossRefResolver(AppPaths::database(), _ctx.controller->settings()->adsApiToken(), this);

    connect(_ctx.controller->settings(), &AppSettings::adsApiTokenChanged,
            _refResolver, [this]() {
                _refResolver->setAdsApiToken(
                    _ctx.controller->settings()->adsApiToken());
            });
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
    ensureGalacticKinematics();

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

    if (QWidget *gal = createGalacticSection())
        layout->addWidget(gal);

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
    QVBoxLayout *nameCol = new QVBoxLayout;
    nameCol->setSpacing(2);

    QString displayName = _ctx.star->getAlias().isEmpty()
                              ? _ctx.star->getSourceId()
                              : _ctx.star->getAlias();

    // ── Name row: name label + re-identify (pen) button ──────────────────
    QHBoxLayout *nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(6);

    QLabel *nameLabel = new QLabel(displayName);
    nameLabel->setStyleSheet(QString("font-size: 20px; font-weight: 700; "
                                     "color: %1; background: transparent;")
                                 .arg(dark ? "white" : "#1a1a1a"));
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameRow->addWidget(nameLabel);

    QPushButton *editBtn = new QPushButton;
    editBtn->setIcon(themedSvgIcon(":/icons/pencil.svg",
                                   QColor(dark ? "#8aa3c8" : "#5a6b85")));
    editBtn->setIconSize(QSize(15, 15));
    editBtn->setFlat(true);
    editBtn->setCursor(Qt::PointingHandCursor);
    editBtn->setFixedSize(22, 22);
    editBtn->setToolTip(
        "Re-identify this star (pick a different nearby source)");
    editBtn->setStyleSheet(
        "QPushButton { border: none; background: transparent; }"
        "QPushButton:hover { background: rgba(127,127,127,0.18); border-radius: 4px; }");
    connect(editBtn, &QPushButton::clicked, this, [this]() {
        ReidentifyStarDialog dlg(_ctx.star, _ctx.dbm, _ctx.projectId, this);
        if (dlg.exec() == QDialog::Accepted) {
            // The dialog already applied fields + called dbm->updateStar().
            _ctx.star->markSummaryDirty(); // notify app to reload
            refresh();                     // re-render this panel
        }
    });
    // Align to the top of the row rather than its geometric centre: the 20px
    // label box carries descent/leading space below the glyphs, so a centred
    // pen drifts down toward the alias line. Top alignment puts the icon on the
    // name's optical (cap-height) centre.
    nameRow->addWidget(editBtn, 0, Qt::AlignTop);
    nameRow->addStretch();

    nameCol->addLayout(nameRow);

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

    // Right side: editable spectral-class badge
    hLayout->addWidget(createSpecClassBadge(), 0,
                       Qt::AlignRight | Qt::AlignVCenter);

    return header;
}

QWidget *SummaryPanel::createSpecClassBadge() {
    const bool    dark      = PanelUtils::isDarkTheme();
    const QString specClass = _ctx.star->getSpecClass();
    const bool    empty     = specClass.isEmpty();

    QWidget     *host = new QWidget;
    QHBoxLayout *l    = new QHBoxLayout(host);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);

    QColor badgeColor =
        empty ? (dark ? QColor(70, 70, 75) : QColor(225, 225, 230))
              : specClassColor(specClass);
    QColor badgeText =
        empty ? (dark ? QColor(160, 160, 165) : QColor(110, 110, 115))
              : accentTextColor(badgeColor);

    // --- display badge (a flat button so it's natively clickable) ---
    QPushButton *badge = new QPushButton(empty ? "＋ Spec. class" : specClass);
    badge->setCursor(Qt::PointingHandCursor);
    badge->setFixedHeight(30);
    badge->setMinimumWidth(60);
    badge->setToolTip("Click to edit spectral class");
    badge->setStyleSheet(
        QString("QPushButton { font-size: 13px; font-weight: 700; color: %1;"
                " background: %2; border-radius: 6px; padding: 2px 12px; "
                "border: none; }"
                "QPushButton:hover { text-decoration: underline; }")
            .arg(badgeText.name(), badgeColor.name()));

    // --- inline editor ---
    QLineEdit *editor = new QLineEdit(specClass);
    editor->setFixedHeight(30);
    editor->setMinimumWidth(90);
    editor->setMaxLength(32);
    editor->setAlignment(Qt::AlignCenter);
    editor->setVisible(false);
    editor->setStyleSheet(
        QString(
            "QLineEdit { font-size: 13px; font-weight: 700; border-radius: 6px;"
            " padding: 2px 8px; border: 1px solid %1; background: %2; color: "
            "%3; }")
            .arg(dark ? "#5a5a5f" : "#bbbbbb", dark ? "#3a3a3f" : "#ffffff",
                 dark ? "#eeeeee" : "#222222"));

    l->addWidget(badge);
    l->addWidget(editor);

    // Click -> enter edit mode
    connect(badge, &QPushButton::clicked, this, [this, badge, editor]() {
        _specEditing = true;
        badge->setVisible(false);
        editor->setText(_ctx.star->getSpecClass());
        editor->setVisible(true);
        editor->setFocus();
        editor->selectAll();
    });

    // Commit on Enter or focus-out (returnPressed also fires editingFinished).
    // Defer the rebuild so we don't delete the editor inside its own signal.
    connect(editor, &QLineEdit::editingFinished, this, [this, editor]() {
        if (!_specEditing)
            return;
        _specEditing      = false;
        const QString val = editor->text();
        QTimer::singleShot(0, this, [this, val]() { commitSpecClass(val); });
    });

    return host;
}

void SummaryPanel::commitSpecClass(const QString &raw) {
    const QString newClass = raw.trimmed();

    if (newClass != _ctx.star->getSpecClass()) {
        _ctx.star->setSpecClass(newClass);

        // Persist to the database
        _ctx.dbm->updateStar(_ctx.projectId, _ctx.star);

        // Let other views (table, etc.) know the star changed
        _ctx.star->markSummaryDirty();
    }

    rebuild(); // restores the badge view with the new text/colour
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

        QString value = has(logP) ? QString::number(logP, 'f', 2) : "-";
        layout->addWidget(
            createMetricCard(value, "log(p)", subtitle, logPColor(logP)));
    }

    // ── ΔRV_max
    {
        const double drv   = S.getDeltaRV();
        const double edrv  = S.getEDeltaRV();
        QString      value = has(drv) ? QString::number(drv, 'f', 1) : "-";
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
    QColor cardBg   = PanelUtils::themeSurface();
    QColor border   = sectionBorderColor();
    QColor labelCol = mutedTextColor();
    QColor subCol   = blendColor(PanelUtils::themeFg(), PanelUtils::themeSurface(), 0.50);

    QFrame* card = new QFrame;
    card->setObjectName("metricCard");
    card->setStyleSheet(QString(
        "QFrame#metricCard { background: %1; border: 1px solid %2; "
        "border-left: 4px solid %3; border-radius: 6px; }"
        "QFrame#metricCard > QWidget { background: transparent; }"
    ).arg(cardBg.name(), border.name(), accentColor.name()));

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);

    QLabel* valueLabel = new QLabel(value);
    valueLabel->setStyleSheet(QString(
        "font-size: 22px; font-weight: 700; color: %1; border: none; background: transparent;"
    ).arg(accentColor.name()));
    valueLabel->setAlignment(Qt::AlignLeft);
    if (value != "-")
        makeCopyable(valueLabel, value);
    layout->addWidget(valueLabel);

    QLabel* labelWidget = new QLabel(label);
    labelWidget->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1; border: none; background: transparent;"
    ).arg(labelCol.name()));
    if (value != "-")
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

    const QColor valCol   = primaryTextColor();
    const QColor labelCol = mutedTextColor();

    auto addV = [&](std::vector<PropRow> &rows, const QString &l, double v,
                    double err, int prec, const QString &unit = "",
                    double errUp = AsymErr::unset,
                    double errDown = AsymErr::unset) {
        if (!has(v))
            return;
        auto d = fmtValErr(v, err, prec, unit, errUp, errDown);
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
        addV(photoF, "LC Period", bestLC->period, bestLC->periodError, 6, "d",
             bestLC->periodErrorUp, bestLC->periodErrorDown);
        addV(photoF, "T₀ (BJD)", bestLC->t0BJD, bestLC->t0BJDError, 6, "",
             bestLC->t0BJDErrorUp, bestLC->t0BJDErrorDown);
        addV(photoF, "Inclination", bestLC->inclination,
             bestLC->inclinationError, 2, "°",
             bestLC->inclinationErrorUp, bestLC->inclinationErrorDown);
        addV(photoF, "q (M₂/M₁)", bestLC->q, bestLC->qError, 3, "",
             bestLC->qErrorUp, bestLC->qErrorDown);
        addV(photoF, "r₁/a", bestLC->r1, bestLC->r1Error, 4, "",
             bestLC->r1ErrorUp, bestLC->r1ErrorDown);
        addV(photoF, "r₂/a", bestLC->r2, bestLC->r2Error, 4, "",
             bestLC->r2ErrorUp, bestLC->r2ErrorDown);
        addV(photoF, "T₁", bestLC->t1, bestLC->t1Error, 0, "K",
             bestLC->t1ErrorUp, bestLC->t1ErrorDown);
        addV(photoF, "T₂", bestLC->t2, bestLC->t2Error, 0, "K",
             bestLC->t2ErrorUp, bestLC->t2ErrorDown);
        addV(photoF, "v_scale", bestLC->velocityScale,
             bestLC->velocityScaleError, 2, "km/s",
             bestLC->velocityScaleErrorUp, bestLC->velocityScaleErrorDown);
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
    addV(atmosC, "T_eff", S.getTeff(), S.getETeff(), 0, "K",
         S.getETeffUp(), S.getETeffDown());
    addV(atmosC, "log g", S.getLogg(), S.getELogg(), 2, "dex",
         S.getELoggUp(), S.getELoggDown());
    addV(atmosC, "log(He/H)", S.getHe(), S.getEHe(), 2, "",
         S.getEHeUp(), S.getEHeDown());

    if (!S.getSpecClass().isEmpty())
        atmosF.push_back({"Spec. Class", S.getSpecClass(), S.getSpecClass()});
    addV(atmosF, "T_eff", S.getTeff(), S.getETeff(), 0, "K",
         S.getETeffUp(), S.getETeffDown());
    addV(atmosF, "log g", S.getLogg(), S.getELogg(), 3, "dex",
         S.getELoggUp(), S.getELoggDown());
    addV(atmosF, "log(He/H)", S.getHe(), S.getEHe(), 3, "",
         S.getEHeUp(), S.getEHeDown());
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
    const QColor valCol   = primaryTextColor();
    const QColor labelCol = mutedTextColor();
    auto has = [](double v) { return std::isfinite(v) && v != 0.0; };

    auto rvCurve = _ctx.star->getRVCurve();
    auto bestFit = rvCurve->getBestFit();

    std::vector<PropRow> compact, full;
    auto pushV = [&](std::vector<PropRow> &rows, const QString &l, double v,
                     double e, int p, const QString &u = "",
                     double eUp = AsymErr::unset,
                     double eDown = AsymErr::unset) {
        auto d = fmtValErr(v, e, p, u, eUp, eDown);
        rows.push_back({l, d.display, d.copy});
    };

    pushV(compact, "Period", bestFit->getPeriod(), bestFit->getPeriodError(), 6,
          "d", bestFit->getPeriodErrorUp(), bestFit->getPeriodErrorDown());
    pushV(compact, "K", bestFit->getK(), bestFit->getKError(), 2, "km/s",
          bestFit->getKErrorUp(), bestFit->getKErrorDown());
    pushV(compact, "γ", bestFit->getGamma(), bestFit->getGammaError(), 2,
          "km/s", bestFit->getGammaErrorUp(), bestFit->getGammaErrorDown());
    pushV(compact, "T₀ (ϕ)", bestFit->getPhi(), bestFit->getPhiError(), 4, "",
          bestFit->getPhiErrorUp(), bestFit->getPhiErrorDown());
    if (bestFit->isEccentric()) {
        pushV(compact, "e", bestFit->getEccentricity(),
              bestFit->getEccentricityError(), 4, "",
              bestFit->getEccentricityErrorUp(),
              bestFit->getEccentricityErrorDown());
        pushV(compact, "ω", bestFit->getOmega(), bestFit->getOmegaError(), 1,
              "°", bestFit->getOmegaErrorUp(), bestFit->getOmegaErrorDown());
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
    QColor tagText   = primaryTextColor();
    QColor checkCol  = QColor(80, 180, 100);
    QColor crossCol  = blendColor(PanelUtils::themeFg(), PanelUtils::themeSurface(), 0.55);
    QColor detailCol = mutedTextColor();

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
            ).arg(detailCol.name()));
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
    // Inner reference cards sit on the elevated section surface; render them on
    // the theme base so they read as a recessed list item, with theme-derived
    // border/text so the whole card matches the active theme.
    QColor cardBg      = PanelUtils::themeBg();
    QColor cardBorder  = sectionBorderColor();
    QColor titleColor  = primaryTextColor();
    QColor subtitleCol = mutedTextColor();
    QColor bodyCol     = blendColor(PanelUtils::themeFg(), PanelUtils::themeBg(), 0.15);
    QColor abstractBg  = blendColor(PanelUtils::themeBg(), PanelUtils::themeFg(), 0.05);
    QColor linkColor   = dark ? QColor(120, 160, 230) : QColor(40, 90, 180);
    QColor loadingCol  = mutedTextColor();

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
    // Same theme-derived scheme as buildReferenceCards (recessed item on the
    // elevated section surface).
    QColor cardBg      = PanelUtils::themeBg();
    QColor cardBorder  = sectionBorderColor();
    QColor titleColor  = primaryTextColor();
    QColor subtitleCol = mutedTextColor();
    QColor bodyCol     = blendColor(PanelUtils::themeFg(), PanelUtils::themeBg(), 0.15);
    QColor abstractBg  = blendColor(PanelUtils::themeBg(), PanelUtils::themeFg(), 0.05);
    QColor linkColor   = dark ? QColor(120, 160, 230) : QColor(40, 90, 180);
    QColor loadingCol  = mutedTextColor();

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
                        if (!_refResolver->hasAdsApiToken()) {
                            QMessageBox::information(
                                this, "ADS API Token Required",
                                "Please provide your NASA ADS API token in the "
                                "Settings "
                                "to fetch references from NASA ADS.\n\n"
                                "You can get a free token at:\n"
                                "https://ui.adsabs.harvard.edu/user/settings/"
                                "token");
                            return;
                        }
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
    QColor cardBg  = PanelUtils::themeSurface();
    QColor border  = sectionBorderColor();
    QColor titleCol = mutedTextColor();

    QFrame* frame = new QFrame;
    frame->setFrameShape(QFrame::NoFrame);
    frame->setObjectName("sectionCard");
    // The QFrame#sectionCard rule paints the elevated surface; the inner content
    // widgets would otherwise pick up the global `QWidget { background-color }`
    // theme rule (leaving a stray theme-bg rectangle behind the params), so make
    // every descendant QWidget transparent and let the card surface show through.
    frame->setStyleSheet(QString(
        "QFrame#sectionCard { background: %1; border: 1px solid %2; border-radius: 6px; }"
        "QFrame#sectionCard > QWidget { background: transparent; }"
    ).arg(cardBg.name(), border.name()));

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

    if (content)
        content->setAttribute(Qt::WA_StyledBackground, true);
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

    // SED primary mass
    const double m1    = _ctx.star->getSedMass1();
    const double eM1   = _ctx.star->getSedEMass1();
    const bool   hasM1 = std::isfinite(m1) && m1 > 0.0;

    // Light-curve inclination
    const double incl    = _ctx.star->getPhotIncl();
    const double eIncl   = _ctx.star->getPhotEIncl();
    const bool   hasIncl = Star::isSet(incl) && incl > 0.0;

    // Also gather inputs for f(M) and a, so the section can show *something*
    // even when only some pieces are present.
    const MassInputs &in = _cachedMassInputs;

    const bool hasMassFunc   = in.valid;
    const bool hasSeparation = in.valid && std::isfinite(in.P) && in.P > 0.0;
    // q must be genuinely set AND non-zero - isSet() only rejects NaN.
    const bool hasQ =
        Star::isSet(_ctx.star->getPhotQ()) && _ctx.star->getPhotQ() > 0.0;

    if (!hasMin && !hasTrue && !hasMassFunc && !hasSeparation && !hasQ &&
        !hasM1 && !hasIncl)
        return nullptr;

    const QColor valCol   = primaryTextColor();
    const QColor labelCol = mutedTextColor();

    std::vector<PropRow> rows;

    if (hasM1) {
        auto d = fmtValErr(m1, eM1, 3, "M☉", _ctx.star->getSedEMass1Up(),
                           _ctx.star->getSedEMass1Down());
        rows.push_back({"M₁ (SED)", d.display, d.copy});
    }
    if (hasMin) {
        auto d = fmtValErr(mMin, eMin, 3, "M☉",
                           _ctx.star->getECompMassMinUp(),
                           _ctx.star->getECompMassMinDown());
        rows.push_back({"M₂ (min)", d.display, d.copy});
    }
    if (hasTrue) {
        auto d = fmtValErr(mTrue, eTrue, 3, "M☉",
                           _ctx.star->getECompMassTrueUp(),
                           _ctx.star->getECompMassTrueDown());
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
    if (hasIncl) {
        auto d = fmtValErr(incl, eIncl, 2, "°", _ctx.star->getPhotEInclUp(),
                           _ctx.star->getPhotEInclDown());
        rows.push_back({"i (LC)", d.display, d.copy});
    }
    if (hasQ) {
        auto d =
            fmtValErr(_ctx.star->getPhotQ(), _ctx.star->getPhotEQ(), 3, "",
                      _ctx.star->getPhotEQUp(), _ctx.star->getPhotEQDown());
        rows.push_back({"q (LC)", d.display, d.copy});
    }

    QWidget *grid = buildPropertyGrid(rows, valCol, labelCol);

    // Warn when the photometric mass ratio could not be reconciled with the RV
    // mass function (it implied sin i > 1), so M₂(true) was withheld or taken
    // from the inclination instead of q·M₁.
    if (_cachedMassTrueInconsistent) {
        QWidget     *wrap = new QWidget;
        QVBoxLayout *vl   = new QVBoxLayout(wrap);
        vl->setContentsMargins(0, 0, 0, 0);
        vl->setSpacing(6);
        vl->addWidget(grid);

        QLabel *warn = new QLabel(
            tr("⚠ q is incompatible with the RV mass function "
               "(implies sin i > 1); M₂(true) not taken from q·M₁."));
        warn->setWordWrap(true);
        warn->setStyleSheet(
            "font-size: 11px; color: #d08a30; background: transparent; "
            "border: none;");
        vl->addWidget(warn);
        return createSectionFrame("Companion", wrap);
    }

    return createSectionFrame("Companion", grid);
}

QColor SummaryPanel::logPColor(double logP) const
{
    // Very negative = highly variable = important
    if (std::isnan(logP) || logP == 0.0)
        return PanelUtils::isDarkTheme() ? QColor(100, 100, 100) : QColor(180, 180, 180);
    if (logP < -10.0)
        return QColor(220, 50, 50);    // Red - extremely significant
    if (logP < -5.0)
        return QColor(230, 150, 30);   // Orange - significant
    if (logP < -2.0)
        return QColor(200, 200, 50);   // Yellow - marginal
    return QColor(80, 180, 80);        // Green - consistent with constant
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
    QPushButton *toggle = new QPushButton(tr("Show all"));
    UiIcons::apply(toggle, UiIcons::Role::DisclosureCollapsed, 12);
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
                         toggle->setText(showAll ? tr("Show less") : tr("Show all"));
                         UiIcons::apply(toggle,
                                        showAll ? UiIcons::Role::DisclosureExpanded
                                                : UiIcons::Role::DisclosureCollapsed,
                                        12);
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
    return valid == o.valid && hasIncl == o.hasIncl && hasQ == o.hasQ &&
           eq(P, o.P) && eq(eP, o.eP) && eq(K, o.K) && eq(eK, o.eK) &&
           eq(M1, o.M1) && eq(eM1, o.eM1) && eq(e, o.e) && eq(ee, o.ee) &&
           eq(sini, o.sini) && eq(esini, o.esini) && eq(q, o.q) &&
           eq(eQ, o.eQ) && eq(ePUp, o.ePUp) && eq(ePDown, o.ePDown) &&
           eq(eKUp, o.eKUp) && eq(eKDown, o.eKDown) && eq(eM1Up, o.eM1Up) &&
           eq(eM1Down, o.eM1Down) && eq(eeUp, o.eeUp) &&
           eq(eeDown, o.eeDown) && eq(eQUp, o.eQUp) &&
           eq(eQDown, o.eQDown) && eq(iDeg, o.iDeg) && eq(eIDeg, o.eIDeg) &&
           eq(eIDegUp, o.eIDegUp) && eq(eIDegDown, o.eIDegDown);
}

void SummaryPanel::ensureCompanionMasses() {
    MassInputs in;

    auto                   rvCurve = _ctx.star->getRVCurve();
    std::shared_ptr<RVFit> fit     = rvCurve ? rvCurve->getBestFit() : nullptr;

    if (fit && fit->getPeriod() > 0) {
        in.P      = fit->getPeriod();
        in.eP     = fit->getPeriodError();
        in.ePUp   = fit->getPeriodErrorUp();
        in.ePDown = fit->getPeriodErrorDown();
        in.K      = fit->getK();
        in.eK     = fit->getKError();
        in.eKUp   = fit->getKErrorUp();
        in.eKDown = fit->getKErrorDown();
        if (fit->isEccentric()) {
            in.e      = fit->getEccentricity();
            in.ee     = fit->getEccentricityError();
            in.eeUp   = fit->getEccentricityErrorUp();
            in.eeDown = fit->getEccentricityErrorDown();
        }
    } else {
        in.P      = _ctx.star->getRVPeriod();
        in.eP     = _ctx.star->getRVEPeriod();
        in.ePUp   = _ctx.star->getRVEPeriodUp();
        in.ePDown = _ctx.star->getRVEPeriodDown();
        in.K      = _ctx.star->getRVK();
        in.eK     = _ctx.star->getRVEK();
        in.eKUp   = _ctx.star->getRVEKUp();
        in.eKDown = _ctx.star->getRVEKDown();
        if (Star::isSet(_ctx.star->getRVEcc()))
            in.e = _ctx.star->getRVEcc();
    }
    in.M1      = _ctx.star->getSedMass1();
    in.eM1     = _ctx.star->getSedEMass1();
    in.eM1Up   = _ctx.star->getSedEMass1Up();
    in.eM1Down = _ctx.star->getSedEMass1Down();

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
        in.iDeg     = iDeg;
        in.eIDeg    = (std::isfinite(eiDeg) && eiDeg > 0.0) ? eiDeg : 0.0;
        in.eIDegUp  = _ctx.star->getPhotEInclUp();
        in.eIDegDown = _ctx.star->getPhotEInclDown();
    }

    // Mass ratio q = M2/M1 from the light-curve solution. When present it gives
    // an inclination-independent handle on the true companion mass (M2 = q·M1).
    const double qVal  = _ctx.star->getPhotQ();
    const double eqVal = _ctx.star->getPhotEQ();
    in.hasQ            = Star::isSet(qVal) && qVal > 0.0;
    if (in.hasQ) {
        in.q      = qVal;
        in.eQ     = (std::isfinite(eqVal) && eqVal > 0.0) ? eqVal : 0.0;
        in.eQUp   = _ctx.star->getPhotEQUp();
        in.eQDown = _ctx.star->getPhotEQDown();
    }

    in.valid = std::isfinite(in.P) && in.P > 0.0 && std::isfinite(in.K) &&
               in.K > 0.0 && std::isfinite(in.M1) && in.M1 > 0.0;

    // Recompute only when the inputs actually changed. The reconciliation below
    // always runs, so a stale or incorrect value already in the database is
    // detected and corrected every time the panel is opened.
    if (!_hasMassCache || !_cachedMassInputs.sameAs(in)) {
        _cachedMassInputs = in;
        _hasMassCache     = true;

        _cachedMassTrueInconsistent = false;
        if (!in.valid) {
            _cachedMassMin  = {};
            _cachedMassTrue = {};
        } else {
            constexpr double kInf = std::numeric_limits<double>::infinity();

            // Symmetric inputs keep the fast linearised errors; any input
            // with an asymmetric interval switches the result to split-
            // normal MC percentiles (stored through the merge rule, so a
            // near-symmetric outcome collapses back to a single σ).
            auto applyMC = [](MassResult r, const SplitNormalMC &mc,
                              auto &&eval) {
                double u = 0.0, d = 0.0;
                if (mc.anyAsymmetric() && mc.run(r.value, eval, u, d)) {
                    const auto st = AsymErr::toStorage(u, d);
                    r.error   = st.sym;
                    r.errUp   = st.up;
                    r.errDown = st.down;
                }
                return r;
            };
            auto addCommon = [&](SplitNormalMC &mc) {
                mc.add(in.P, in.eP, in.ePUp, in.ePDown, 1e-9, kInf);
                mc.add(in.K, in.eK, in.eKUp, in.eKDown, 1e-9, kInf);
                mc.add(in.M1, in.eM1, in.eM1Up, in.eM1Down, 1e-9, kInf);
                mc.add(in.e, in.ee, in.eeUp, in.eeDown, 0.0, 0.999);
            };
            // True mass from the mass function at the stored inclination.
            auto inclMass = [&]() -> MassResult {
                const double mT = m2Of(in.P, in.K, in.M1, in.e, in.sini);
                const double sT =
                    propagateM2Error(in.P, in.eP, in.K, in.eK, in.M1, in.eM1,
                                     in.e, in.ee, in.sini, in.esini);
                SplitNormalMC mc;
                addCommon(mc);
                // Resample the inclination itself (degrees); sin i folds
                // naturally at 90°, so the range spans both sides.
                mc.add(in.iDeg, in.eIDeg, in.eIDegUp, in.eIDegDown, 0.01,
                       179.99);
                return applyMC({mT, sT}, mc, [](const std::vector<double> &x) {
                    const double s = std::sin(x[4] * M_PI / 180.0);
                    return s > 1e-6
                               ? m2Of(x[0], x[1], x[2], x[3], s)
                               : std::numeric_limits<double>::quiet_NaN();
                });
            };

            // M2 (min): edge-on (sin i = 1) lower bound from the mass function.
            const double mMin = m2Of(in.P, in.K, in.M1, in.e, 1.0);
            const double sMin = propagateM2Error(in.P, in.eP, in.K, in.eK, in.M1,
                                                 in.eM1, in.e, in.ee, 1.0, 0.0);
            {
                SplitNormalMC mc;
                addCommon(mc);
                _cachedMassMin =
                    applyMC({mMin, sMin}, mc, [](const std::vector<double> &x) {
                        return m2Of(x[0], x[1], x[2], x[3], 1.0);
                    });
            }

            // M2 (true). Two light-curve constraints can pin the companion
            // mass: the mass ratio q = M2/M1 and the orbital inclination i.
            // The spectroscopic mass function f = M2³sin³i/(M1+M2)² ties them
            // to the RV data. Multiplying q·M1 blindly ignores f and lets the
            // result drop *below* the edge-on floor M2_min whenever the
            // photometric q is incompatible with the RV semi-amplitude. So we
            // derive the mass self-consistently and flag irreconcilable data.
            if (in.hasQ) {
                const double f = massFunctionMsun(in.K, in.P, in.e);
                // Inclination implied by (f, q, M1) through the mass function:
                //   f = q³·M1·sin³i / (1+q)²   ⇒   sin³i = f(1+q)² / (q³·M1).
                const double oneP = 1.0 + in.q;
                const double s3   = (in.q > 0.0 && in.M1 > 0.0)
                                        ? f * oneP * oneP /
                                              (in.q * in.q * in.q * in.M1)
                                        : std::numeric_limits<double>::quiet_NaN();
                const double siniImp =
                    std::isfinite(s3) ? std::cbrt(s3)
                                      : std::numeric_limits<double>::quiet_NaN();

                if (std::isfinite(siniImp) && siniImp <= 1.0 + 1e-6) {
                    // Consistent: q·M1 satisfies the mass function with a
                    // physical sin i ≤ 1, hence it is guaranteed to sit on or
                    // above M2_min (which is the same curve evaluated at i=90°).
                    const double mT = in.q * in.M1;
                    const double rq = (in.q > 0.0) ? in.eQ / in.q : 0.0;
                    const double rm = (in.M1 > 0.0) ? in.eM1 / in.M1 : 0.0;
                    const double sT = mT * std::sqrt(rq * rq + rm * rm);
                    SplitNormalMC mc;
                    mc.add(in.q, in.eQ, in.eQUp, in.eQDown, 1e-9, kInf);
                    mc.add(in.M1, in.eM1, in.eM1Up, in.eM1Down, 1e-9, kInf);
                    _cachedMassTrue = applyMC(
                        {mT, sT}, mc, [](const std::vector<double> &x) {
                            return x[0] * x[1];
                        });
                } else {
                    // q and the RV mass function disagree (they would require
                    // sin i > 1). Don't report a sub-floor q·M1. Fall back to
                    // the inclination-derived mass when one is available (it is
                    // always ≥ M2_min); otherwise leave the true mass unset so
                    // only the M2_min lower limit is shown.
                    _cachedMassTrueInconsistent = true;
                    if (in.hasIncl) {
                        _cachedMassTrue = inclMass();
                    } else {
                        _cachedMassTrue = {};
                    }
                }
            } else if (in.hasIncl) {
                _cachedMassTrue = inclMass();
            } else {
                _cachedMassTrue = {};
            }
        }
    }

    // Reconcile against the value already stored on the Star / in the database
    // and persist only when it has genuinely drifted (stale fit, edited q, etc.).
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
    if (neq(s.getECompMassMinUp(), _cachedMassMin.errUp)) {
        s.setECompMassMinUp(_cachedMassMin.errUp);
        changed = true;
    }
    if (neq(s.getECompMassMinDown(), _cachedMassMin.errDown)) {
        s.setECompMassMinDown(_cachedMassMin.errDown);
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
    if (neq(s.getECompMassTrueUp(), _cachedMassTrue.errUp)) {
        s.setECompMassTrueUp(_cachedMassTrue.errUp);
        changed = true;
    }
    if (neq(s.getECompMassTrueDown(), _cachedMassTrue.errDown)) {
        s.setECompMassTrueDown(_cachedMassTrue.errDown);
        changed = true;
    }
    if (changed)
        s.persistSummary();
}

void SummaryPanel::ensureGalacticKinematics() {
    // Assemble the input; when astrometry/RV is incomplete leave any stored
    // values untouched (they may come from an import).
    GalKin::KinematicsInput in;
    _galInputsValid = GalKin::kinematicsInputFromStar(*_ctx.star, in);
    if (!_galInputsValid)
        return;

    // Recompute only when the inputs actually changed (the MC costs a few ms;
    // the panel rebuilds on every refresh).
    auto same = [](const GalKin::KinematicsInput &a,
                   const GalKin::KinematicsInput &b) {
        auto eq = [](double x, double y) {
            if (std::isnan(x) && std::isnan(y)) return true;
            return x == y;
        };
        return eq(a.raDeg, b.raDeg) && eq(a.decDeg, b.decDeg) &&
               eq(a.parallaxMas, b.parallaxMas) &&
               eq(a.parallaxErrMas, b.parallaxErrMas) &&
               eq(a.pmraMasYr, b.pmraMasYr) && eq(a.pmraErr, b.pmraErr) &&
               eq(a.pmdecMasYr, b.pmdecMasYr) && eq(a.pmdecErr, b.pmdecErr) &&
               eq(a.plxPmraCorr, b.plxPmraCorr) &&
               eq(a.plxPmdecCorr, b.plxPmdecCorr) &&
               eq(a.pmraPmdecCorr, b.pmraPmdecCorr) &&
               eq(a.rvKmS, b.rvKmS) && eq(a.rvErrUp, b.rvErrUp) &&
               eq(a.rvErrDown, b.rvErrDown);
    };
    const bool haveStored = Star::isSet(_ctx.star->getGalU()) &&
                            Star::isSet(_ctx.star->getGalX());
    if (_hasGalCache && same(_cachedGalInputs, in) && haveStored)
        return;
    _cachedGalInputs = in;
    _hasGalCache     = true;

    bool changed = false;
    if (GalKin::computeAndStoreUVWXYZ(*_ctx.star,
                                      GalKin::GalacticPotential::Model::AS,
                                      10000, &changed) &&
        changed)
        _ctx.star->persistSummary();
}

QWidget *SummaryPanel::createGalacticSection() {
    Star &s = *_ctx.star;

    const bool hasUVW = Star::isSet(s.getGalU()) && Star::isSet(s.getGalV()) &&
                        Star::isSet(s.getGalW());
    const bool hasXYZ = Star::isSet(s.getGalX()) && Star::isSet(s.getGalY()) &&
                        Star::isSet(s.getGalZ());
    const bool hasPop = Star::isSet(s.getGalPThin()) ||
                        Star::isSet(s.getGalPThick()) ||
                        Star::isSet(s.getGalPHalo());
    const bool hasOrbit = Star::isSet(s.getGalJz()) ||
                          Star::isSet(s.getGalEcc());
    if (!hasUVW && !hasXYZ && !hasPop && !hasOrbit)
        return nullptr;

    const QColor valCol   = primaryTextColor();
    const QColor labelCol = mutedTextColor();

    std::vector<PropRow> rows;
    if (hasUVW) {
        auto u = fmtValErr(s.getGalU(), s.getGalEU(), 1, "km/s",
                           s.getGalEUUp(), s.getGalEUDown());
        auto v = fmtValErr(s.getGalV(), s.getGalEV(), 1, "km/s",
                           s.getGalEVUp(), s.getGalEVDown());
        auto w = fmtValErr(s.getGalW(), s.getGalEW(), 1, "km/s",
                           s.getGalEWUp(), s.getGalEWDown());
        rows.push_back({"U", u.display, u.copy});
        rows.push_back({"V", v.display, v.copy});
        rows.push_back({"W", w.display, w.copy});
    }
    if (hasXYZ) {
        auto x = fmtValErr(s.getGalX(), s.getGalEX(), 3, "kpc",
                           s.getGalEXUp(), s.getGalEXDown());
        auto y = fmtValErr(s.getGalY(), s.getGalEY(), 3, "kpc",
                           s.getGalEYUp(), s.getGalEYDown());
        auto z = fmtValErr(s.getGalZ(), s.getGalEZ(), 3, "kpc",
                           s.getGalEZUp(), s.getGalEZDown());
        rows.push_back({"X", x.display, x.copy});
        rows.push_back({"Y", y.display, y.copy});
        rows.push_back({"Z", z.display, z.copy});
    }
    if (hasOrbit) {
        if (Star::isSet(s.getGalJz())) {
            auto jz = fmtValErr(s.getGalJz(), s.getGalEJz(), 0, "kpc km/s",
                                s.getGalEJzUp(), s.getGalEJzDown());
            rows.push_back({"J_z", jz.display, jz.copy});
        }
        if (Star::isSet(s.getGalEcc())) {
            auto ecc = fmtValErr(s.getGalEcc(), s.getGalEEcc(), 3, "",
                                 s.getGalEEccUp(), s.getGalEEccDown());
            rows.push_back({"ecc", ecc.display, ecc.copy});
        }
    }
    if (hasPop) {
        auto addP = [&](const char *name, double p, double ep) {
            if (!Star::isSet(p))
                return;
            auto d = fmtValErr(p, ep, 2, "");
            rows.push_back({name, d.display, d.copy});
        };
        addP("P(thin)", s.getGalPThin(), s.getGalEPThin());
        addP("P(thick)", s.getGalPThick(), s.getGalEPThick());
        addP("P(halo)", s.getGalPHalo(), s.getGalEPHalo());
    }

    QWidget *grid = buildPropertyGrid(rows, valCol, labelCol);

    // Footnote when the astrometry is incomplete so the values shown must
    // come from an import rather than an in-app computation.
    if (!_galInputsValid) {
        QWidget     *wrap = new QWidget;
        QVBoxLayout *vl   = new QVBoxLayout(wrap);
        vl->setContentsMargins(0, 0, 0, 0);
        vl->setSpacing(6);
        vl->addWidget(grid);
        QLabel *note = new QLabel(
            tr("Imported values — astrometry/RV incomplete, cannot recompute."));
        note->setWordWrap(true);
        note->setStyleSheet(QString("font-size: 10px; color: %1; background: "
                                    "transparent; border: none; font-style: italic;")
                                .arg(mutedTextColor().name()));
        vl->addWidget(note);
        return createSectionFrame("Galactic Kinematics", wrap);
    }

    return createSectionFrame("Galactic Kinematics", grid);
}