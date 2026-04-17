#include "CMDDialog.h"
#include "models/Star.h"
#include "db/DatabaseManager.h"
#include "utils/AppPaths.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

namespace {

constexpr double kMaxRelErrNormal = 0.20;  // >20% → upper limit
constexpr double kMaxRelErrSkip   = 1.00;  // >100% → spurious, skip

double absMagFromPlx(double m, double plx_mas) {
    return m + 5.0 * std::log10(plx_mas) - 10.0;
}
double absMagErr(double m_err, double plx, double e_plx) {
    double a = std::isnan(m_err) ? 0.0 : m_err;
    double b = (5.0 / (plx * std::log(10.0))) * e_plx;
    return std::sqrt(a * a + b * b);
}

QColor pickTrackColor(int idx) {
    static const QColor palette[] = {
        QColor("#E74C3C"), QColor("#3498DB"), QColor("#27AE60"),
        QColor("#F39C12"), QColor("#9B59B6"), QColor("#1ABC9C"),
        QColor("#E67E22"), QColor("#34495E"), QColor("#D35400"),
        QColor("#2980B9")
    };
    return palette[idx % (sizeof(palette) / sizeof(palette[0]))];
}

} // namespace

// ════════════════════════════════════════════════════════════
CMDDialog::CMDDialog(std::shared_ptr<Star> star,
                     std::vector<std::shared_ptr<Star>> projectStars,
                     const QString& projectId,
                     QWidget* parent)
    : QDialog(parent)
    , _star(star)
    , _projectStars(std::move(projectStars))
    , _projectId(projectId)
{
    setupUi();
    loadTrackConfig();
    computeStarPoint();
    loadProjectReferenceStars();

    _tracksList->blockSignals(true);
    for (size_t i = 0; i < _tracks.size(); ++i) {
        auto& t = _tracks[i];
        t.color = pickTrackColor(int(i));
        auto* item = new QListWidgetItem(t.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(t.plotByDefault ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, int(i));
        item->setForeground(QBrush(t.color));
        _tracksList->addItem(item);
        t.enabled = t.plotByDefault;
        if (t.enabled) loadTrackData(t);
    }
    _tracksList->blockSignals(false);

    updatePlot();
    LOG_INFO("Tools", QString("CMD dialog opened for star %1").arg(_star->getSourceId()));
}

CMDDialog::~CMDDialog() = default;

// ── UI ─────────────────────────────────────────────────────
void CMDDialog::setupUi()
{
    setWindowTitle(QString("Colour-Magnitude Diagram — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1100, 750);

    auto* main = new QHBoxLayout(this);

    _plot = new QCustomPlot(this);
    _plot->xAxis->setLabel("BP − RP [mag]");
    _plot->yAxis->setLabel("M_G [mag]");
    _plot->yAxis->setRangeReversed(true);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    _plot->legend->setVisible(true);
    _plot->legend->setFont(QFont(font().family(), 8));
    _plot->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignRight);
    main->addWidget(_plot, 1);

    main->addWidget(buildControlPanel(), 0);
    applyPlotTheme();
}

QWidget* CMDDialog::buildControlPanel()
{
    auto* panel = new QWidget(this);
    panel->setFixedWidth(290);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(6, 6, 6, 6);

    auto* bgGroup = new QGroupBox("Background reference", panel);
    auto* bgLay   = new QVBoxLayout(bgGroup);
    _bgProjectRadio = new QRadioButton("Project stars", bgGroup);
    _bgGaiaRadio    = new QRadioButton("Gaia reference CMD", bgGroup);
    _bgProjectRadio->setChecked(true);
    bgLay->addWidget(_bgProjectRadio);
    bgLay->addWidget(_bgGaiaRadio);
    connect(_bgProjectRadio, &QRadioButton::toggled, this, &CMDDialog::onBackgroundModeChanged);
    connect(_bgGaiaRadio,    &QRadioButton::toggled, this, &CMDDialog::onBackgroundModeChanged);
    v->addWidget(bgGroup);

    auto* trGroup = new QGroupBox("Tracks", panel);
    auto* trLay   = new QVBoxLayout(trGroup);
    _tracksList = new QListWidget(trGroup);
    _tracksList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(_tracksList, &QListWidget::itemChanged, this, &CMDDialog::onTrackItemChanged);
    trLay->addWidget(_tracksList);

    auto* btnRow = new QHBoxLayout;
    _addTrackBtn    = new QPushButton("Add…", trGroup);
    _renameTrackBtn = new QPushButton("Rename", trGroup);
    _removeTrackBtn = new QPushButton("Remove", trGroup);
    btnRow->addWidget(_addTrackBtn);
    btnRow->addWidget(_renameTrackBtn);
    btnRow->addWidget(_removeTrackBtn);
    trLay->addLayout(btnRow);
    connect(_addTrackBtn,    &QPushButton::clicked, this, &CMDDialog::onAddTrack);
    connect(_renameTrackBtn, &QPushButton::clicked, this, &CMDDialog::onRenameTrack);
    connect(_removeTrackBtn, &QPushButton::clicked, this, &CMDDialog::onRemoveTrack);

    _labelTracksCb = new QCheckBox("Label tracks", trGroup);
    _labelTracksCb->setChecked(true);
    connect(_labelTracksCb, &QCheckBox::toggled, this, &CMDDialog::onLabelTracksToggled);
    trLay->addWidget(_labelTracksCb);

    v->addWidget(trGroup, 1);

    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, panel);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(close);

    return panel;
}

// ── Tracks persistence ─────────────────────────────────────
QString CMDDialog::tracksDir() const
{
    QString dir = AppPaths::root() + "/projects/" + _projectId + "/cmd_tracks";
    QDir().mkpath(dir);
    return dir;
}

QString CMDDialog::tracksConfigPath() const { return tracksDir() + "/tracks.json"; }

void CMDDialog::loadTrackConfig()
{
    _tracks.clear();
    QFile f(tracksConfigPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray()) return;
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        CMDTrack t;
        t.name          = o["name"].toString();
        t.filename      = o["filename"].toString();
        t.plotByDefault = o["plotByDefault"].toBool();
        if (!t.filename.isEmpty()) _tracks.push_back(std::move(t));
    }
}

void CMDDialog::saveTrackConfig()
{
    QJsonArray arr;
    for (const auto& t : _tracks) {
        QJsonObject o;
        o["name"]          = t.name;
        o["filename"]      = t.filename;
        o["plotByDefault"] = t.plotByDefault;
        arr.append(o);
    }
    QFile f(tracksConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_ERROR("Tools", "Failed writing tracks config: " + f.fileName());
        return;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.close();
}

bool CMDDialog::loadTrackData(CMDTrack& track)
{
    if (track.dataLoaded) return !track.mags.isEmpty();
    track.mags.clear();
    track.colors.clear();

    QFile f(tracksDir() + "/" + track.filename);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        LOG_ERROR("Tools", "Cannot open track file: " + f.fileName());
        track.dataLoaded = true;
        return false;
    }
    QTextStream ts(&f);
    static const QRegularExpression sep("[,;\\s\\t]+");

    // ── Match column-name aliases (case-insensitive, symbol-stripped) ──
    auto norm = [](QString s) {
        s = s.toLower();
        s.remove(QRegularExpression("[\\s_\\-()\\[\\]]"));
        return s;
    };
    static const QSet<QString> magAliases = {
        "gmag", "g", "mg", "absg", "absgmag", "mag", "magnitude", "gabs", "M_G"
    };
    static const QSet<QString> colAliases = {
        "bprp", "color", "colour", "bminusrp", "gbpgrp", "bp-rp"
    };

    int magCol = -1, colCol = -1;
    bool headerResolved = false;

    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//"))
            continue;
        auto parts = line.split(sep, Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;

        if (!headerResolved) {
            // Try to parse as data: if both first two tokens numeric,
            // assume no header → default column order (Gmag, BP-RP).
            bool a, b;
            double v0 = parts[0].toDouble(&a);
            double v1 = parts[1].toDouble(&b);
            if (a && b && parts.size() == 2) {
                magCol = 0; colCol = 1;
                headerResolved = true;
                track.mags.append(v0);
                track.colors.append(v1);
                continue;
            }

            // Otherwise treat as header — resolve columns by alias.
            for (int i = 0; i < parts.size(); ++i) {
                QString n = norm(parts[i]);
                if (magCol < 0 && magAliases.contains(n)) magCol = i;
                if (colCol < 0 && colAliases.contains(n)) colCol = i;
            }

            if (magCol < 0 || colCol < 0) {
                // Unrecognised header — fall back to first two columns
                // in default order only if exactly two columns exist.
                if (parts.size() == 2 && a && b) {
                    magCol = 0; colCol = 1;
                    track.mags.append(v0);
                    track.colors.append(v1);
                } else {
                    LOG_WARNING("Tools",
                        QString("Track '%1': could not identify Gmag/BP-RP columns from header")
                        .arg(track.name));
                    track.dataLoaded = true;
                    return false;
                }
            }
            headerResolved = true;
            continue;
        }

        // Data row
        if (magCol >= parts.size() || colCol >= parts.size()) continue;
        bool okM, okC;
        double m = parts[magCol].toDouble(&okM);
        double c = parts[colCol].toDouble(&okC);
        if (!okM || !okC) continue;
        track.mags.append(m);
        track.colors.append(c);
    }

    track.dataLoaded = true;
    return !track.mags.isEmpty();
}

// ── Reference data loaders ─────────────────────────────────
void CMDDialog::loadProjectReferenceStars()
{
    if (_projLoaded) return;
    _projLoaded = true;
    _projBpRp.clear();
    _projGabs.clear();

    for (const auto& s : _projectStars) {
        if (!s || s->getId() == _star->getId()) continue;
        double plx = s->getPlx(), ePlx = s->getEPlx();
        double g = s->getGmag(), bp = s->getBp(), rp = s->getRp();
        if (std::isnan(plx) || plx <= 0) continue;
        if (std::isnan(ePlx) || ePlx / plx > kMaxRelErrNormal) continue;
        if (std::isnan(g) || std::isnan(bp) || std::isnan(rp)) continue;
        _projBpRp.append(bp - rp);
        _projGabs.append(absMagFromPlx(g, plx));
    }
}

void CMDDialog::loadGaiaReferenceStars()
{
    if (_gaiaLoaded) return;
    _gaiaLoaded = true;
    QFile f(":/data/gaia_cmd.csv");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        LOG_WARNING("Tools", "Bundled Gaia CMD reference not found (:/data/gaia_cmd.csv)");
        return;
    }
    QTextStream ts(&f);
    bool header = true;
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (header) { header = false; continue; }
        auto p = line.split(',');
        if (p.size() < 2) continue;
        bool okA, okB;
        double c = p[0].toDouble(&okA);
        double m = p[1].toDouble(&okB);
        if (okA && okB) {
            _gaiaBpRp.append(c);
            _gaiaGabs.append(m);
        }
    }
    LOG_INFO("Tools", QString("Loaded %1 Gaia reference stars").arg(_gaiaBpRp.size()));
}

// ── Star point computation ─────────────────────────────────
void CMDDialog::computeStarPoint()
{
    _starMode = StarMode::Skip;
    double g   = _star->getGmag(), bp  = _star->getBp(),  rp  = _star->getRp();
    double eG  = _star->getEGmag(), eBp = _star->getEBp(), eRp = _star->getERp();
    double plx = _star->getPlx(),  ePlx = _star->getEPlx();

    LOG_INFO("Tools", QString("CMD star values: G=%1 BP=%2 RP=%3 plx=%4 e_plx=%5")
             .arg(g).arg(bp).arg(rp).arg(plx).arg(ePlx));

    if (std::isnan(g) || std::isnan(bp) || std::isnan(rp)) {
        LOG_WARNING("Tools", "CMD: star skipped — missing G/BP/RP photometry");
        return;
    }
    double color  = bp - rp;
    double eColor = std::sqrt((std::isnan(eBp) ? 0 : eBp*eBp) +
                              (std::isnan(eRp) ? 0 : eRp*eRp));

    if (std::isnan(plx) || plx <= 0) {
        LOG_WARNING("Tools", "CMD: star skipped — parallax missing or non-positive");
        return;
    }
    if (std::isnan(ePlx)) {
        LOG_WARNING("Tools", "CMD: star skipped — parallax error missing");
        return;
    }
    double rel = ePlx / plx;
    if (rel > kMaxRelErrSkip) {
        LOG_WARNING("Tools", QString("CMD: star skipped — spurious parallax (%1% error)")
                    .arg(rel * 100, 0, 'f', 1));
        return;
    }

    _starColor    = color;
    _starColorErr = eColor;

    if (rel > kMaxRelErrNormal) {
        double plxMin = std::max(plx - ePlx, 1e-6);
        _starMag    = absMagFromPlx(g, plxMin);
        _starMagErr = 0.0;
        _starMode   = StarMode::UpperLimit;
    } else {
        _starMag    = absMagFromPlx(g, plx);
        _starMagErr = absMagErr(eG, plx, ePlx);
        _starMode   = StarMode::Normal;
    }
    LOG_INFO("Tools", QString("CMD: star plotted at (BP-RP=%1, M_G=%2) mode=%3")
             .arg(_starColor, 0, 'f', 3).arg(_starMag, 0, 'f', 3)
             .arg(_starMode == StarMode::UpperLimit ? "upper-limit" : "normal"));
}

// ── Slots ──────────────────────────────────────────────────
void CMDDialog::onBackgroundModeChanged()
{
    if (_bgGaiaRadio->isChecked()) loadGaiaReferenceStars();
    updatePlot();
}

void CMDDialog::onAddTrack()
{
    QString src = QFileDialog::getOpenFileName(this, "Select track CSV",
                                               QString(),
                                               "CSV files (*.csv);;All files (*)");
    if (src.isEmpty()) return;

    bool ok = false;
    QString name = QInputDialog::getText(this, "Track name", "Name for this track:",
                                         QLineEdit::Normal,
                                         QFileInfo(src).baseName(), &ok);
    if (!ok || name.isEmpty()) return;

    QString dest = QString("%1_%2.csv")
                       .arg(qint64(QDateTime::currentMSecsSinceEpoch()))
                       .arg(QFileInfo(src).baseName());
    if (!QFile::copy(src, tracksDir() + "/" + dest)) {
        QMessageBox::warning(this, "Add track", "Failed to copy track file.");
        return;
    }

    CMDTrack t;
    t.name          = name;
    t.filename      = dest;
    t.plotByDefault = true;
    t.enabled       = true;
    t.color         = pickTrackColor(int(_tracks.size()));
    _tracks.push_back(t);
    loadTrackData(_tracks.back());

    if (_tracks.back().mags.isEmpty()) {
        QMessageBox::warning(this, "Track import failed",
            QString("Could not parse any data points from '%1'.\n\n"
                    "Expected format: CSV with columns named e.g. "
                    "'Gmag' and 'BP-RP' (case-insensitive), or two "
                    "unlabeled columns in that order.")
            .arg(QFileInfo(src).fileName()));
        QFile::remove(tracksDir() + "/" + dest);
        _tracks.pop_back();
        return;
    }

    saveTrackConfig();

    _tracksList->blockSignals(true);
    auto* item = new QListWidgetItem(name);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    item->setData(Qt::UserRole, int(_tracks.size()) - 1);
    item->setForeground(QBrush(_tracks.back().color));
    _tracksList->addItem(item);
    _tracksList->blockSignals(false);

    updatePlot();
}

void CMDDialog::onRemoveTrack()
{
    auto* item = _tracksList->currentItem();
    if (!item) return;
    int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= int(_tracks.size())) return;

    if (QMessageBox::question(this, "Remove track",
            QString("Remove track '%1' and delete its file?").arg(_tracks[idx].name))
        != QMessageBox::Yes) return;

    QFile::remove(tracksDir() + "/" + _tracks[idx].filename);
    _tracks.erase(_tracks.begin() + idx);
    saveTrackConfig();

    _tracksList->blockSignals(true);
    _tracksList->clear();
    for (size_t i = 0; i < _tracks.size(); ++i) {
        auto& t = _tracks[i];
        t.color = pickTrackColor(int(i));
        auto* it = new QListWidgetItem(t.name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(t.enabled ? Qt::Checked : Qt::Unchecked);
        it->setData(Qt::UserRole, int(i));
        it->setForeground(QBrush(t.color));
        _tracksList->addItem(it);
    }
    _tracksList->blockSignals(false);
    updatePlot();
}

void CMDDialog::onRenameTrack()
{
    auto* item = _tracksList->currentItem();
    if (!item) return;
    int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= int(_tracks.size())) return;
    bool ok = false;
    QString nn = QInputDialog::getText(this, "Rename track", "New name:",
                                       QLineEdit::Normal, _tracks[idx].name, &ok);
    if (!ok || nn.isEmpty()) return;
    _tracks[idx].name = nn;
    item->setText(nn);
    saveTrackConfig();
    updatePlot();
}

void CMDDialog::onTrackItemChanged(QListWidgetItem* item)
{
    int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= int(_tracks.size())) return;
    bool on = item->checkState() == Qt::Checked;
    _tracks[idx].enabled       = on;
    _tracks[idx].plotByDefault = on;      // toggling also updates default
    if (on) loadTrackData(_tracks[idx]);
    saveTrackConfig();
    updatePlot();
}

void CMDDialog::onLabelTracksToggled(bool) { updatePlot(); }

void CMDDialog::plotBackgroundAsDensity(const QVector<double>& xs,
                                        const QVector<double>& ys)
{
    if (xs.isEmpty()) return;

    const double xMin = -0.5, xMax = 5.0;
    const double yMin = -5.0, yMax = 18.0;
    const int    nx   = 250,  ny   = 350;

    auto* cm = new QCPColorMap(_plot->xAxis, _plot->yAxis);
    cm->setLayer("background");
    cm->setInterpolate(false);
    cm->data()->setSize(nx, ny);
    cm->data()->setRange(QCPRange(xMin, xMax), QCPRange(yMin, yMax));
    cm->data()->fill(0);

    for (int i = 0; i < xs.size(); ++i) {
        int kx, ky;
        cm->data()->coordToCell(xs[i], ys[i], &kx, &ky);
        if (kx >= 0 && kx < nx && ky >= 0 && ky < ny) {
            cm->data()->setCell(kx, ky, cm->data()->cell(kx, ky) + 1);
        }
    }

    QCPColorGradient grad(QCPColorGradient::gpGrayscale);
    if (!isDarkTheme()) grad = grad.inverted();
    grad.setLevelCount(100);
    QColor bg = _plot->axisRect()->backgroundBrush().color();
    grad.setColorStopAt(0.0, bg);
    cm->setGradient(grad);
    cm->setDataScaleType(QCPAxis::stLogarithmic);
    cm->rescaleDataRange(true);
    cm->removeFromLegend();
}

// ── Main plot routine ──────────────────────────────────────
void CMDDialog::updatePlot()
{
    _plot->clearPlottables();
    _plot->clearItems();

    // Background
    const QVector<double>* bgX = nullptr;
    const QVector<double>* bgY = nullptr;
    QString bgName;
    if (_bgGaiaRadio->isChecked()) {
        bgX = &_gaiaBpRp; bgY = &_gaiaGabs;
        bgName = QString("Gaia reference (%1)").arg(_gaiaBpRp.size());
    } else {
        bgX = &_projBpRp; bgY = &_projGabs;
        bgName = QString("Project stars (%1)").arg(_projBpRp.size());
    }

    constexpr int kDensityThreshold = 15000;
    if (bgX && bgX->size() > kDensityThreshold) {
        plotBackgroundAsDensity(*bgX, *bgY);
    } else if (bgX && !bgX->isEmpty()) {
        auto* g = _plot->addGraph();
        g->setData(*bgX, *bgY);
        g->setLineStyle(QCPGraph::lsNone);
        QColor dot = isDarkTheme() ? QColor(210, 210, 210, 70)
                                : QColor(60, 60, 60, 70);
        g->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, dot, dot, 3));
        g->setAdaptiveSampling(true);
        g->setAntialiasedScatters(false);
        g->setName(bgName);
    }

    // Tracks
    for (auto& t : _tracks) {
        if (!t.enabled) continue;
        if (!t.dataLoaded && !loadTrackData(t)) continue;
        if (t.colors.isEmpty()) continue;

        auto* g = _plot->addGraph();
        g->setData(t.colors, t.mags);
        g->setPen(QPen(t.color, 2));
        g->setLineStyle(QCPGraph::lsLine);
        g->setName(t.name);

        if (_labelTracksCb->isChecked()) {
            auto* lbl = new QCPItemText(_plot);
            lbl->position->setType(QCPItemPosition::ptPlotCoords);
            lbl->position->setCoords(t.colors.last(), t.mags.last());
            lbl->setText(t.name);
            lbl->setColor(t.color);
            lbl->setFont(QFont(font().family(), 8, QFont::Bold));
            lbl->setPositionAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            lbl->setPadding(QMargins(4, 0, 0, 0));
        }
    }

    // Star point
    if (_starMode != StarMode::Skip) {
        QVector<double> sx{ _starColor }, sy{ _starMag };
        auto* sg = _plot->addGraph();
        sg->setData(sx, sy);
        sg->setLineStyle(QCPGraph::lsNone);
        QColor sc("#E74C3C");
        sg->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, sc, sc, 10));
        sg->setName(_star->getAlias().isEmpty() ? _star->getSourceId()
                                                : _star->getAlias());

        if (_starColorErr > 0) {
            auto* hErr = new QCPErrorBars(_plot->xAxis, _plot->yAxis);
            hErr->removeFromLegend();
            hErr->setDataPlottable(sg);
            hErr->setErrorType(QCPErrorBars::etKeyError);
            hErr->setPen(QPen(sc, 1.5));
            QVector<double> e{ _starColorErr };
            hErr->setData(e);
        }

        if (_starMode == StarMode::Normal && _starMagErr > 0) {
            auto* vErr = new QCPErrorBars(_plot->xAxis, _plot->yAxis);
            vErr->removeFromLegend();
            vErr->setDataPlottable(sg);
            vErr->setErrorType(QCPErrorBars::etValueError);
            vErr->setPen(QPen(sc, 1.5));
            QVector<double> e{ _starMagErr };
            vErr->setData(e);
        } else if (_starMode == StarMode::UpperLimit) {
            // Arrow pointing "down" on plot (= toward larger M_G = fainter)
            auto* arrow = new QCPItemLine(_plot);
            arrow->start->setType(QCPItemPosition::ptPlotCoords);
            arrow->end->setType(QCPItemPosition::ptPlotCoords);
            arrow->start->setCoords(_starColor, _starMag);
            arrow->end  ->setCoords(_starColor, _starMag + 1.5);
            arrow->setHead(QCPLineEnding(QCPLineEnding::esSpikeArrow, 10));
            arrow->setPen(QPen(sc, 2.0));
        }
    }

    // Auto-range from background data, 1st–99th percentile
    auto percentileRange = [](QVector<double> v, double lo, double hi)
                        -> std::pair<double,double> {
        if (v.isEmpty()) return {0.0, 1.0};
        std::sort(v.begin(), v.end());
        int n = v.size();
        int iLo = std::clamp(int(lo * n), 0, n - 1);
        int iHi = std::clamp(int(hi * n), 0, n - 1);
        return { v[iLo], v[iHi] };
    };

    const QVector<double>& bgXref = _bgGaiaRadio->isChecked() ? _gaiaBpRp : _projBpRp;
    const QVector<double>& bgYref = _bgGaiaRadio->isChecked() ? _gaiaGabs : _projGabs;

    if (!bgXref.isEmpty() && !bgYref.isEmpty()) {
        auto [xLo, xHi] = percentileRange(bgXref, 0.001, 0.999);
        auto [yLo, yHi] = percentileRange(bgYref, 0.001, 0.999);
        double xp = (xHi - xLo) * 0.1;
        double yp = (yHi - yLo) * 0.1;
        _plot->xAxis->setRange(xLo - xp, xHi + xp);
        _plot->yAxis->setRange(yLo - yp, yHi + yp);
    } else {
        _plot->xAxis->setRange(-1, 4);
        _plot->yAxis->setRange(-2, 15);
    }

    _plot->replot();
}

// ── Theme ──────────────────────────────────────────────────
bool CMDDialog::isDarkTheme() const
{
    return qApp->property("isDarkTheme").toBool();
}

void CMDDialog::applyPlotTheme()
{
    bool isDark = isDarkTheme();

    QColor bgColor      = isDark ? QColor(42, 42, 42)    : QColor(255, 255, 255);
    QColor textColor    = isDark ? QColor(210, 210, 210) : QColor(30, 30, 30);
    QColor gridColor    = isDark ? QColor(80, 80, 80)    : QColor(200, 200, 200);
    QColor subGridColor = isDark ? QColor(55, 55, 55)    : QColor(225, 225, 225);

    // Take the app-wide window color (set by ThemeManager)
    QColor appBg = qApp->palette().color(QPalette::Window);
    if (appBg.alpha() == 255) {
        bgColor = appBg;
    }

    _plot->setStyleSheet("");
    _plot->setBackground(QBrush(bgColor));
    _plot->axisRect()->setBackground(QBrush(bgColor));

    for (auto* axis : {_plot->xAxis, _plot->xAxis2, _plot->yAxis, _plot->yAxis2}) {
        axis->setBasePen(QPen(textColor, 1));
        axis->setTickPen(QPen(textColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(gridColor, 0.8));
        axis->grid()->setSubGridVisible(false);
    }

    _plot->xAxis2->setVisible(true);
    _plot->yAxis2->setVisible(true);
    _plot->xAxis2->setTickLabels(false);
    _plot->yAxis2->setTickLabels(false);

    _plot->legend->setBorderPen(QPen(gridColor));
    _plot->legend->setBrush(QBrush(bgColor));
    _plot->legend->setTextColor(textColor);
}