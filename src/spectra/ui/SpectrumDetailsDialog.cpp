#include "spectra/ui/SpectrumDetailsDialog.h"

#include "app/ui/WindowSizing.h"
#include "app/ui/panels/PanelUtils.h"
#include "core/Instrument.h"
#include "core/InstrumentMode.h"
#include "core/Quantity.h"
#include "core/QuantityFormat.h"
#include "core/Stats.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"
#include "fitting/ElementAbundances.h"
#include "spectra/Spectrum.h"
#include "ui/widgets/CopyToast.h"
#include "ui/widgets/ElidedLabel.h"
#include "ui/widgets/QuantityLabel.h"
#include "ui/widgets/ResponsiveGridLayout.h"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// MJD of 1970-01-01T00:00:00Z, the anchor for the UTC stamp shown next to the
// stored epoch.
constexpr double kMjdUnixEpoch = 40587.0;

QColor valueColor() { return PanelUtils::themeFg(); }
QColor labelColor()
{
    return PanelUtils::mix(PanelUtils::themeFg(), PanelUtils::themeSurface(), 0.40);
}

QDateTime mjdToUtc(double mjd)
{
    return QDateTime::fromMSecsSinceEpoch(
        qint64(std::llround((mjd - kMjdUnixEpoch) * 86400000.0)), QTimeZone::utc());
}

// ── Click-to-copy for the plain-text rows ───────────────────────────────────
// The measured rows are QuantityLabels, which bring their own copy behaviour;
// this gives the text rows (identifiers, file paths, flags) the same one-click
// feel the summary panel's text rows have.
class CopyFilter : public QObject
{
public:
    CopyFilter(QString text, QObject* parent)
        : QObject(parent), _text(std::move(text)) {}

protected:
    bool eventFilter(QObject*, QEvent* ev) override
    {
        if (ev->type() == QEvent::MouseButtonPress) {
            CopyToast::copy(_text);
            return true;
        }
        return false;
    }

private:
    QString _text;
};

void makeCopyable(QWidget* w, const QString& text)
{
    if (text.isEmpty()) return;
    w->setCursor(Qt::PointingHandCursor);
    w->installEventFilter(new CopyFilter(text, w));
}

// One property row: either a measured value or a piece of text.
struct Row {
    QString  label;
    bool     useQuantity = false;
    Quantity quantity;
    QString  text;
    QString  copy;
};

void addQ(std::vector<Row>& rows, const QString& label, double value,
          double error, int precision, const QString& unit = QString(),
          double errUp = AsymErr::unset, double errDown = AsymErr::unset,
          const QString& name = QString())
{
    if (!std::isfinite(value)) return;
    Row r;
    r.label       = label;
    r.useQuantity = true;
    r.quantity    = Quantity(value, error, precision, unit, errUp, errDown, name);
    rows.push_back(r);
}

void addText(std::vector<Row>& rows, const QString& label, const QString& text,
             const QString& copy = QString())
{
    if (text.isEmpty()) return;
    Row r;
    r.label = label;
    r.text  = text;
    r.copy  = copy.isEmpty() ? text : copy;
    rows.push_back(r);
}

void addBool(std::vector<Row>& rows, const QString& label, bool value)
{
    addText(rows, label, value ? QStringLiteral("Yes") : QStringLiteral("No"));
}

void addInt(std::vector<Row>& rows, const QString& label, long long value)
{
    addText(rows, label, QString::number(value));
}

QLabel* makeRowLabel(const QString& text)
{
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(QString("font-size: 11px; font-weight: 600; color: %1; "
                              "background: transparent; border: none;")
                           .arg(labelColor().name()));
    return lbl;
}

QWidget* buildGrid(const std::vector<Row>& rows)
{
    auto* grid = new QWidget;
    auto* gl   = new ResponsiveGridLayout(grid);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setColumnSpacing(18);
    gl->setRowSpacing(3);
    gl->setPairSpacing(8);
    // Two pair-columns unless a row carries something long (a UUID, a file
    // path, a URL): the layout would fit those side by side only by squeezing
    // both, and a wrapped-looking identifier is worse than a taller section.
    // Below that bound the layout still drops to one column when the width
    // does not allow two.
    const bool anyLong =
        std::any_of(rows.begin(), rows.end(), [](const Row& r) {
            return !r.useQuantity && r.text.size() > 20;
        });
    gl->setMaxColumns(anyLong ? 1 : 2);

    for (const Row& row : rows) {
        QLabel*  lbl = makeRowLabel(row.label);
        QWidget* val = nullptr;

        if (row.useQuantity) {
            auto* ql = new QuantityLabel(row.quantity);
            ql->setTextPixelSize(12);
            ql->setColors(valueColor(), labelColor());
            const Quantity q = row.quantity;
            makeCopyable(lbl, QuantityFormat::copyText(q));
            val = ql;
        } else {
            auto* plain = new ElidedLabel(row.text);
            plain->setMinimumTextWidth(60);
            plain->setStyleSheet(QString("font-size: 12px; color: %1; "
                                         "background: transparent; border: none;")
                                     .arg(valueColor().name()));
            makeCopyable(plain, row.copy);
            makeCopyable(lbl, row.copy);
            val = plain;
        }
        gl->addPair(lbl, val);
    }
    return grid;
}

QGroupBox* makeSection(const QString& title, QWidget* content)
{
    auto* box = new QGroupBox(title);
    auto* v   = new QVBoxLayout(box);
    v->setContentsMargins(10, 6, 10, 8);
    v->addWidget(content);
    return box;
}

QString plainDumpOf(const std::vector<Row>& rows)
{
    QString out;
    for (const Row& r : rows) {
        out += r.label;
        out += ": ";
        out += r.useQuantity ? QuantityFormat::plainText(r.quantity) : r.text;
        out += '\n';
    }
    return out;
}

// ── Abundances ──────────────────────────────────────────────────────────────
// Their own table rather than the property grid: each element carries the
// fitted log abundance, its solar-relative form and a note saying whether the
// value is a measurement, a frozen input or a grid-edge limit, and three
// columns read far better than three property rows per element.
struct AbundanceRow {
    QString  element;
    Quantity raw;
    Quantity solar;
    bool     hasSolar = false;
    QString  note;
};

std::vector<AbundanceRow> abundanceRows(const QMap<QString, FittedAbundance>& map)
{
    std::vector<AbundanceRow> out;
    const auto& elements = astra::elements::all();

    for (int i = 0; i < elements.size(); ++i) {
        const auto& info = elements[i];
        auto it = map.constFind(info.symbol);
        if (it == map.constEnd()) continue;

        const FittedAbundance& a = it.value();
        // An element switched off in the grid carries no measurement.
        if (!a.isSet() || astra::elements::isSwitchedOff(a.value)) continue;

        AbundanceRow r;
        r.element = info.display;
        const double err = (std::isfinite(a.error) && a.error > 0.0) ? a.error
                                                                    : AsymErr::unset;
        r.raw = Quantity(a.value, err, 3, QString(), AsymErr::unset,
                         AsymErr::unset, QString("\\log n(%1)").arg(info.display));

        const double sol = astra::elements::toSolarRelative(i, a.value);
        if (std::isfinite(sol)) {
            r.hasSolar = true;
            r.solar = Quantity(sol, err, 3, QStringLiteral("dex"), AsymErr::unset,
                               AsymErr::unset,
                               QString("[%1/H]").arg(info.display));
        }

        if (a.frozen)             r.note = QStringLiteral("frozen");
        else if (a.limitSide < 0) r.note = QStringLiteral("upper limit");
        else if (a.limitSide > 0) r.note = QStringLiteral("lower limit");

        out.push_back(r);
    }

    // Elements the grid resolves but ASTRA does not carry a column for still
    // belong in the record; they follow the known ones, in name order.
    QStringList extra;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        if (!astra::elements::bySymbol(it.key()) && it.value().isSet() &&
            !astra::elements::isSwitchedOff(it.value().value))
            extra << it.key();
    extra.sort();
    for (const QString& sym : extra) {
        const FittedAbundance& a = map.value(sym);
        AbundanceRow r;
        r.element = sym;
        const double err = (std::isfinite(a.error) && a.error > 0.0) ? a.error
                                                                    : AsymErr::unset;
        r.raw = Quantity(a.value, err, 3);
        if (a.frozen)             r.note = QStringLiteral("frozen");
        else if (a.limitSide < 0) r.note = QStringLiteral("upper limit");
        else if (a.limitSide > 0) r.note = QStringLiteral("lower limit");
        out.push_back(r);
    }
    return out;
}

QWidget* buildAbundanceTable(const std::vector<AbundanceRow>& rows)
{
    auto* w  = new QWidget;
    auto* gl = new QGridLayout(w);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setHorizontalSpacing(18);
    gl->setVerticalSpacing(3);

    const QStringList headers{"Element", "log n(X)", "[X/H]", ""};
    for (int c = 0; c < headers.size(); ++c) {
        if (headers[c].isEmpty()) continue;
        gl->addWidget(makeRowLabel(headers[c]), 0, c);
    }

    int r = 1;
    for (const AbundanceRow& row : rows) {
        auto* name = new QLabel(row.element);
        name->setStyleSheet(QString("font-size: 12px; font-weight: 600; "
                                    "color: %1; background: transparent; "
                                    "border: none;")
                                .arg(valueColor().name()));
        gl->addWidget(name, r, 0);

        auto* raw = new QuantityLabel(row.raw);
        raw->setTextPixelSize(12);
        raw->setColors(valueColor(), labelColor());
        gl->addWidget(raw, r, 1);

        if (row.hasSolar) {
            auto* sol = new QuantityLabel(row.solar);
            sol->setTextPixelSize(12);
            sol->setColors(valueColor(), labelColor());
            gl->addWidget(sol, r, 2);
        }

        if (!row.note.isEmpty()) {
            auto* note = new QLabel(row.note);
            note->setStyleSheet(QString("font-size: 11px; font-style: italic; "
                                        "color: %1; background: transparent; "
                                        "border: none;")
                                    .arg(labelColor().name()));
            gl->addWidget(note, r, 3);
        }
        ++r;
    }
    gl->setColumnStretch(3, 1);
    return w;
}

QString plainDumpOf(const std::vector<AbundanceRow>& rows)
{
    QString out;
    for (const AbundanceRow& r : rows) {
        out += r.element;
        out += ": ";
        out += QuantityFormat::plainText(r.raw);
        if (r.hasSolar)
            out += "  [X/H] = " + QuantityFormat::plainText(r.solar);
        if (!r.note.isEmpty())
            out += "  (" + r.note + ")";
        out += '\n';
    }
    return out;
}

/// Human label for a provenance key of the archive metadata blob.
QString originMetaLabel(const QString& key)
{
    if (key == QLatin1String("archive"))     return QStringLiteral("Archive");
    if (key == QLatin1String("collection"))  return QStringLiteral("Collection");
    if (key == QLatin1String("snr"))         return QStringLiteral("S/N (archive)");
    if (key == QLatin1String("R"))           return QStringLiteral("R (archive)");
    if (key == QLatin1String("url"))         return QStringLiteral("Download URL");
    if (key == QLatin1String("barycorrKms"))
        return QStringLiteral("Barycentric shift");
    return key;
}

} // namespace

// ============================================================================

SpectrumDetailsDialog::SpectrumDetailsDialog(const QString& title,
                                             const QString& subtitle,
                                             QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setSizeGripEnabled(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    auto* head = new QLabel(title);
    head->setStyleSheet(QString("font-size: 14px; font-weight: 700; color: %1; "
                                "background: transparent;")
                            .arg(valueColor().name()));
    outer->addWidget(head);

    if (!subtitle.isEmpty()) {
        auto* sub = new QLabel(subtitle);
        sub->setWordWrap(true);
        sub->setStyleSheet(QString("font-size: 11px; color: %1; "
                                    "background: transparent;")
                               .arg(labelColor().name()));
        outer->addWidget(sub);
    }

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    _body = new QVBoxLayout(content);
    _body->setContentsMargins(0, 0, 0, 0);
    _body->setSpacing(8);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
}

void SpectrumDetailsDialog::finish(const QString& header)
{
    _body->addStretch(1);

    auto* buttons = new QHBoxLayout;
    auto* copyAll = new QPushButton("Copy All");
    copyAll->setToolTip("Copy every value on this page as plain text");
    const QString dump = header + "\n" + _plainDump;
    connect(copyAll, &QPushButton::clicked, this,
            [dump] { CopyToast::copy(dump); });
    buttons->addWidget(copyAll);
    buttons->addStretch(1);

    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);

    static_cast<QVBoxLayout*>(layout())->addLayout(buttons);

    resize(620, 640);
    WindowSizing::fitToScreen(this);
}

// ── Spectrum ────────────────────────────────────────────────────────────────

void SpectrumDetailsDialog::buildSpectrum(const std::shared_ptr<Spectrum>& spec,
                                          DatabaseManager* dbm)
{
    auto addSection = [this](const QString& title, const std::vector<Row>& rows) {
        if (rows.empty()) return;
        _body->addWidget(makeSection(title, buildGrid(rows)));
        _plainDump += "[" + title + "]\n" + plainDumpOf(rows) + "\n";
    };

    // ── Identification ──
    {
        std::vector<Row> rows;
        addText(rows, "Spectrum ID", spec->getId());
        if (!spec->getFile().isEmpty()) {
            const QFileInfo fi(spec->getFile());
            addText(rows, "Source file", fi.fileName(), spec->getFile());
            addText(rows, "Source folder", fi.absolutePath());
        }
        addText(rows, "Data cache", spec->getDataFile());
        addBool(rows, "Flagged as bad", spec->isFlagged());
        addInt(rows, "Stored fits", (long long)spec->getSpectralFits().size());
        if (auto best = spec->getBestFit())
            addText(rows, "Best fit",
                    best->modelId.isEmpty() ? best->getId() : best->modelId);
        addSection("Identification", rows);
    }

    // ── Timing ──
    {
        std::vector<Row> rows;
        const Time& t = spec->time();
        if (t.isValid())
            addText(rows, "Stored as",
                    QString("%1 %2").arg(Time::scaleToString(t.nativeScale()))
                        .arg(t.nativeValue(), 0, 'f', 6));

        if (auto mjd = t.mjd()) {
            addQ(rows, "MJD", *mjd, AsymErr::unset, 6, QString(), AsymErr::unset,
                 AsymErr::unset, "\\mathrm{MJD}");
            addText(rows, "UTC",
                    mjdToUtc(*mjd).toString("yyyy-MM-dd hh:mm:ss") + " UTC");
        }
        if (auto bjd = t.bjd())
            addQ(rows, "BJD (TDB)", *bjd, AsymErr::unset, 6, QString(),
                 AsymErr::unset, AsymErr::unset, "\\mathrm{BJD}");
        if (auto hjd = t.hjd())
            addQ(rows, "HJD (UTC)", *hjd, AsymErr::unset, 6, QString(),
                 AsymErr::unset, AsymErr::unset, "\\mathrm{HJD}");
        if (t.hasExposureTime())
            addQ(rows, "Exposure time", t.exposureTimeSec(), AsymErr::unset, 1,
                 "s");
        addBool(rows, "Barycentrically corrected",
                spec->isBarycentricallyCorrected());
        addSection("Timing", rows);
    }

    // ── Instrument ──
    {
        std::vector<Row> rows;
        addText(rows, "Instrument", spec->getInstrument());
        addText(rows, "Mode key", spec->getModeKey());

        std::shared_ptr<Instrument> inst;
        if (dbm && !spec->getInstrumentId().isEmpty())
            inst = dbm->getInstrumentById(spec->getInstrumentId());
        if (inst) {
            addText(rows, "Configured as", inst->getFullName().isEmpty()
                                               ? inst->getName()
                                               : inst->getFullName());
            if (const InstrumentMode* mode = inst->mode(spec->getModeKey())) {
                addText(rows, "Mode", mode->displayName());
                if (mode->hasSpectralProperties()) {
                    const auto& sp = mode->spectral();
                    if (sp.wavelengthMin > 0.0 && sp.wavelengthMax > 0.0) {
                        const double mid =
                            0.5 * (sp.wavelengthMin + sp.wavelengthMax);
                        const double R = mode->resolutionAt(mid);
                        if (R > 0.0)
                            addQ(rows, "Resolution R", R, AsymErr::unset, 0);
                        addQ(rows, "Mode range (min)", sp.wavelengthMin,
                             AsymErr::unset, 1, "Å");
                        addQ(rows, "Mode range (max)", sp.wavelengthMax,
                             AsymErr::unset, 1, "Å");
                    }
                    if (!sp.disperser.isEmpty())
                        addText(rows, "Disperser", sp.disperser);
                }
            }
            if (inst->isSpaceBased()) {
                addText(rows, "Site", "Space based");
            } else if (inst->hasLocation()) {
                addQ(rows, "Site latitude",  inst->getLatitude(),
                     AsymErr::unset, 5, "°");
                addQ(rows, "Site longitude", inst->getLongitude(),
                     AsymErr::unset, 5, "°");
                addQ(rows, "Site altitude",  inst->getAltitude(),
                     AsymErr::unset, 0, "m");
            }
        }
        addText(rows, "Instrument ID", spec->getInstrumentId());
        addSection("Instrument", rows);
    }

    // ── Data ──
    {
        std::vector<Row> rows;
        const auto wl = spec->getWavelengths();
        if (!wl.empty()) {
            const auto [lo, hi] = std::minmax_element(wl.begin(), wl.end());
            addInt(rows, "Pixels", (long long)wl.size());
            addQ(rows, "λ min", *lo, AsymErr::unset, 2, "Å");
            addQ(rows, "λ max", *hi, AsymErr::unset, 2, "Å");
            if (wl.size() > 1)
                addQ(rows, "Mean Δλ", (*hi - *lo) / double(wl.size() - 1),
                     AsymErr::unset, 4, "Å");

            const auto flux = spec->getFluxes();
            const auto err  = spec->getFluxErrors();
            std::vector<double> snr;
            snr.reserve(std::min(flux.size(), err.size()));
            for (size_t i = 0; i < flux.size() && i < err.size(); ++i)
                if (std::isfinite(flux[i]) && std::isfinite(err[i]) && err[i] > 0.0)
                    snr.push_back(std::abs(flux[i]) / err[i]);
            if (!snr.empty())
                addQ(rows, "Median S/N", Stats::median(std::move(snr)),
                     AsymErr::unset, 1);
        } else {
            addText(rows, "Flux data", "Not loaded");
        }
        addSection("Data", rows);
    }

    // ── Archive provenance ──
    if (spec->isFetched()) {
        std::vector<Row> rows;
        addText(rows, "Origin", spec->getOrigin());
        addText(rows, "Origin ID", spec->getOriginId());
        addText(rows, "Product", spec->isCoadd() ? "Co-added"
                                                 : "Single exposure");

        const QJsonObject meta =
            QJsonDocument::fromJson(spec->getOriginMeta().toUtf8()).object();
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
            const QString label = originMetaLabel(it.key());
            if (it.value().isDouble()) {
                const double v = it.value().toDouble();
                const QString unit = it.key() == QLatin1String("barycorrKms")
                                         ? QStringLiteral("km/s")
                                         : QString();
                // The blob mixes counts (a resolving power) with measurements
                // (a signal-to-noise ratio, a velocity), so the precision
                // follows the value rather than one blanket choice.
                const bool integral = std::isfinite(v) && std::abs(v) < 1e12 &&
                                      v == std::floor(v);
                addQ(rows, label, v, AsymErr::unset, integral ? 0 : 3, unit);
            } else if (it.value().isBool()) {
                addBool(rows, label, it.value().toBool());
            } else {
                addText(rows, label, it.value().toVariant().toString());
            }
        }
        addSection("Archive provenance", rows);
    }
}

// ── Spectral fit ────────────────────────────────────────────────────────────

void SpectrumDetailsDialog::buildFit(const std::shared_ptr<SpectralFit>& fit)
{
    auto addSection = [this](const QString& title, const std::vector<Row>& rows) {
        if (rows.empty()) return;
        _body->addWidget(makeSection(title, buildGrid(rows)));
        _plainDump += "[" + title + "]\n" + plainDumpOf(rows) + "\n";
    };

    // ── Identification ──
    {
        std::vector<Row> rows;
        addText(rows, "Fit ID", fit->getId());
        addText(rows, "Model grid", fit->modelId);
        if (fit->creationDate.isValid())
            addText(rows, "Created",
                    fit->creationDate.toString("yyyy-MM-dd hh:mm:ss"));
        addBool(rows, "Best fit", fit->isBestFit);
        addBool(rows, "Flagged as bad", fit->isFlagged);
        addInt(rows, "Components", fit->nComponents);
        addText(rows, "Model data file", fit->getModelDataFile());
        addBool(rows, "Model curves loaded", fit->hasData());
        addSection("Fit", rows);
    }

    // ── Solver ──
    {
        std::vector<Row> rows;
        if (std::isfinite(fit->chi2) && fit->chi2 != 0.0)
            addQ(rows, "χ²", fit->chi2, AsymErr::unset, 3, QString(),
                 AsymErr::unset, AsymErr::unset, "\\chi^2");
        const double red = fit->reducedChi2();
        if (std::isfinite(red))
            addQ(rows, "χ²/dof", red, AsymErr::unset, 4, QString(),
                 AsymErr::unset, AsymErr::unset, "\\chi^2_\\nu");
        if (fit->nDataPoints > 0) {
            addInt(rows, "Data points", fit->nDataPoints);
            addInt(rows, "Free parameters", fit->nFreeParameters);
            addInt(rows, "Degrees of freedom",
                   fit->nDataPoints - fit->nFreeParameters);
        }
        addBool(rows, "Converged", fit->converged);
        if (fit->iterations > 0)
            addInt(rows, "Iterations", fit->iterations);
        addSection("Solver", rows);
    }

    // ── Component 1 ──
    {
        std::vector<Row> rows;
        addQ(rows, "T_eff", fit->teff, fit->teffError, 0, "K",
             fit->teffErrorUp, fit->teffErrorDown, "T_{\\rm eff}");
        addQ(rows, "log g", fit->logg, fit->loggError, 3, "dex",
             fit->loggErrorUp, fit->loggErrorDown, "\\log g");
        addQ(rows, "log(He/H)", fit->he, fit->heError, 3, QString(),
             fit->heErrorUp, fit->heErrorDown, "\\log(He/H)");
        addQ(rows, "v sin i", fit->vsini, fit->vsiniError, 2, "km/s",
             fit->vsiniErrorUp, fit->vsiniErrorDown, "v\\sin i");
        addQ(rows, "RV", fit->radialVelocity, fit->radialVelocityError, 2,
             "km/s", fit->radialVelocityErrorUp, fit->radialVelocityErrorDown,
             "v_{\\rm rad}");
        addQ(rows, "[M/H]", fit->metallicity, fit->metallicityError, 3, "dex",
             fit->metallicityErrorUp, fit->metallicityErrorDown, "[M/H]");
        addQ(rows, "ζ (macroturbulence)", fit->macroturbulence,
             fit->macroturbulenceError, 2, "km/s", fit->macroturbulenceErrorUp,
             fit->macroturbulenceErrorDown, "\\zeta");
        addQ(rows, "ξ (microturbulence)", fit->microturbulence,
             fit->microturbulenceError, 2, "km/s", fit->microturbulenceErrorUp,
             fit->microturbulenceErrorDown, "\\xi");
        addSection(fit->nComponents >= 2 ? "Component 1" : "Fitted parameters",
                   rows);
    }

    // ── Component 2 ──
    if (fit->nComponents >= 2) {
        std::vector<Row> rows;
        addQ(rows, "T_eff", fit->teff2, fit->teff2Error, 0, "K",
             AsymErr::unset, AsymErr::unset, "T_{\\rm eff,2}");
        addQ(rows, "log g", fit->logg2, fit->logg2Error, 3, "dex",
             AsymErr::unset, AsymErr::unset, "\\log g_2");
        addQ(rows, "log(He/H)", fit->he2, fit->he2Error, 3);
        addQ(rows, "v sin i", fit->vsini2, fit->vsini2Error, 2, "km/s");
        addQ(rows, "RV", fit->radialVelocity2, fit->radialVelocity2Error, 2,
             "km/s", AsymErr::unset, AsymErr::unset, "v_{\\rm rad,2}");
        addQ(rows, "[M/H]", fit->metallicity2, fit->metallicity2Error, 3, "dex");
        addQ(rows, "ζ (macroturbulence)", fit->macroturbulence2,
             fit->macroturbulence2Error, 2, "km/s");
        addQ(rows, "ξ (microturbulence)", fit->microturbulence2,
             fit->microturbulence2Error, 2, "km/s");
        addQ(rows, "Surface ratio", fit->surRatio, fit->surRatioError, 4,
             QString(), AsymErr::unset, AsymErr::unset, "A_2/A_1");
        addSection("Component 2", rows);
    }

    // ── Telluric ──
    if (fit->hasTelluric) {
        std::vector<Row> rows;
        addQ(rows, "Airmass", fit->telluricAirmass, fit->telluricAirmassError,
             3);
        addQ(rows, "Precipitable water", fit->telluricPwv,
             fit->telluricPwvError, 3, "mm");
        addQ(rows, "Barycentric shift", fit->telluricBarycorr, AsymErr::unset,
             3, "km/s");
        addSection("Telluric component", rows);
    }

    // ── Abundances ──
    auto addAbundances = [this](const QString& title,
                                const QMap<QString, FittedAbundance>& map) {
        const auto rows = abundanceRows(map);
        if (rows.empty()) return;
        _body->addWidget(makeSection(title, buildAbundanceTable(rows)));
        _plainDump += "[" + title + "]\n" + plainDumpOf(rows) + "\n";
    };

    const bool two = fit->nComponents >= 2 && !fit->abundances2.isEmpty();
    addAbundances(two ? "Element abundances - component 1"
                      : "Element abundances",
                  fit->abundances);
    if (two)
        addAbundances("Element abundances - component 2", fit->abundances2);
}

// ── Entry points ────────────────────────────────────────────────────────────

void SpectrumDetailsDialog::showSpectrum(const std::shared_ptr<Spectrum>& spec,
                                         DatabaseManager* dbm, QWidget* parent)
{
    if (!spec) return;

    QString title = spec->getInstrument().isEmpty()
                        ? QStringLiteral("Spectrum")
                        : spec->getInstrument();
    if (spec->getMJD() > 0)
        title += QString("  MJD %1").arg(spec->getMJD(), 0, 'f', 4);

    QString subtitle = spec->getFile().isEmpty()
                           ? spec->getId()
                           : QFileInfo(spec->getFile()).fileName();

    auto* dlg = new SpectrumDetailsDialog(title, subtitle, parent);
    dlg->buildSpectrum(spec, dbm);
    dlg->finish(QString("Spectrum: %1 (%2)").arg(title, subtitle));
    dlg->show();
}

void SpectrumDetailsDialog::showFit(const std::shared_ptr<Spectrum>& spec,
                                    const std::shared_ptr<SpectralFit>& fit,
                                    QWidget* parent)
{
    if (!fit) return;

    QString title = fit->modelId.isEmpty()
                        ? QString("Fit %1").arg(fit->getId().left(8))
                        : fit->modelId;

    QString subtitle;
    if (spec) {
        subtitle = spec->getInstrument();
        if (spec->getMJD() > 0) {
            if (!subtitle.isEmpty()) subtitle += "  ";
            subtitle += QString("MJD %1").arg(spec->getMJD(), 0, 'f', 4);
        }
        if (!subtitle.isEmpty())
            subtitle = "Fit of " + subtitle;
    }

    auto* dlg = new SpectrumDetailsDialog(title, subtitle, parent);
    dlg->buildFit(fit);
    dlg->finish(QString("Spectral fit: %1%2")
                    .arg(title, subtitle.isEmpty() ? QString()
                                                   : " (" + subtitle + ")"));
    dlg->show();
}
