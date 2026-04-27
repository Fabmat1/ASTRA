#include "RVInspectorDialog.h"

#include "models/Star.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/Time.h"
#include "db/DatabaseManager.h"
#include "views/panels/RVPanel.h"
#include "views/panels/DetailPanel.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QListWidget>
#include <QTableView>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QFont>
#include <QMenu>
#include <QAction>
#include <QUuid>
#include <QDateTime>

#include <cmath>

// ═════════════════════════════════════════════════════════════════════════════
//   RVPointsTableModel
// ═════════════════════════════════════════════════════════════════════════════

RVPointsTableModel::RVPointsTableModel(std::shared_ptr<Star> star,
                                       std::shared_ptr<RadialVelocityCurve> curve,
                                       DatabaseManager* dbm,
                                       QObject* parent)
    : QAbstractTableModel(parent)
    , _star(std::move(star))
    , _curve(std::move(curve))
    , _dbm(dbm)
{
    reload();
}

void RVPointsTableModel::reload()
{
    beginResetModel();
    _points = _curve ? _curve->getRVPoints()
                     : std::vector<std::shared_ptr<RadialVelocityPoint>>{};
    _spectraById.clear();
    if (_star) {
        for (const auto& s : _star->getSpectra())
            if (s) _spectraById.insert(s->getId(), s);
    }
    computeMissingBJDs();
    endResetModel();
}

void RVPointsTableModel::computeMissingBJDs()
{
    if (!_star || !_curve) return;
    double ra  = _star->getRa();
    double dec = _star->getDec();
    if (std::isnan(ra) || std::isnan(dec)) return;

    int computed = 0;
    for (auto& p : _points) {
        if (!p) continue;
        double bjd = p->getBJD();
        if (bjd > 0.0 && !std::isnan(bjd)) continue;

        double mjd = p->getMJD();
        if (mjd <= 0.0 || std::isnan(mjd)) continue;

        auto inst = resolveInstrumentObject(p);
        if (!inst) continue;

        p->time().computeBJD(*inst, ra, dec);
        if (p->getBJD() > 0.0) {
            _curve->persistPoint(p);
            ++computed;
        }
    }
    if (computed > 0) {
        LOG_INFO("Tools", QString("RV Inspector: computed BJD for %1 point(s)")
                 .arg(computed));
    }
}

int RVPointsTableModel::rowCount(const QModelIndex& p) const
{ return p.isValid() ? 0 : static_cast<int>(_points.size()); }

int RVPointsTableModel::columnCount(const QModelIndex& p) const
{ return p.isValid() ? 0 : ColCount; }

QVariant RVPointsTableModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (role != Qt::DisplayRole || o != Qt::Horizontal) return {};
    switch (section) {
        case ColMJD:           return "MJD";
        case ColBJD:           return "BJD";
        case ColRV:            return "RV [km/s]";
        case ColErrFormal:     return "σ_formal";
        case ColErrSystematic: return "σ_systematic";
        case ColInstrument:    return "Instrument";
        case ColSource:        return "Source";
        case ColFlagged:       return "Flag";
    }
    return {};
}

Qt::ItemFlags RVPointsTableModel::flags(const QModelIndex& idx) const
{
    if (!idx.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = QAbstractTableModel::flags(idx);
    switch (idx.column()) {
        case ColRV:
        case ColErrFormal:
        case ColErrSystematic: f |= Qt::ItemIsEditable;     break;
        case ColFlagged:       f |= Qt::ItemIsUserCheckable; break;
        default: break;
    }
    return f;
}

std::shared_ptr<Spectrum> RVPointsTableModel::linkedSpectrum(
    const std::shared_ptr<RadialVelocityPoint>& p) const
{
    if (!p) return nullptr;
    if (auto sp = p->getSourceSpectrum().lock()) return sp;
    auto it = _spectraById.find(p->getSpectrumId());
    return it != _spectraById.end() ? it.value() : nullptr;
}

std::shared_ptr<Instrument> RVPointsTableModel::resolveInstrumentObject(
    const std::shared_ptr<RadialVelocityPoint>& p) const
{
    if (!p) return nullptr;
    if (auto inst = p->getInstrument()) return inst;
    if (auto sp = linkedSpectrum(p)) {
        if (_dbm && !sp->getInstrumentId().isEmpty())
            return _dbm->getInstrumentById(sp->getInstrumentId());
        if (_dbm && !sp->getInstrument().isEmpty())
            return _dbm->getInstrumentByName(sp->getInstrument());
    }
    return nullptr;
}

QString RVPointsTableModel::resolveInstrumentName(
    const std::shared_ptr<RadialVelocityPoint>& p) const
{
    if (auto inst = resolveInstrumentObject(p)) return inst->getName();
    if (auto sp = linkedSpectrum(p)) {
        QString s = sp->getInstrument();
        if (!s.isEmpty()) return s;
    }
    return {};
}

QVariant RVPointsTableModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= static_cast<int>(_points.size())) return {};
    const auto& p = _points[idx.row()];
    if (!p) return {};

    if (role == Qt::CheckStateRole && idx.column() == ColFlagged)
        return p->isFlagged() ? Qt::Checked : Qt::Unchecked;

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (idx.column()) {
            case ColMJD: {
                double m = p->getMJD();
                return (m > 0.0 && !std::isnan(m))
                    ? QString::number(m, 'f', 6) : QString("—");
            }
            case ColBJD: {
                double b = p->getBJD();
                return (b > 0.0 && !std::isnan(b))
                    ? QString::number(b, 'f', 6)
                    : QString("Not calculated");
            }
            case ColRV:            return QString::number(p->getRV(), 'f', 4);
            case ColErrFormal:     return QString::number(p->getRVErrorFormal(), 'f', 4);
            case ColErrSystematic: return QString::number(p->getRVErrorSystematic(), 'f', 4);
            case ColInstrument: {
                QString n = resolveInstrumentName(p);
                return n.isEmpty() ? QString("—") : n;
            }
            case ColSource:
                return p->getRVSource() == RadialVelocityPoint::RVSource::FromFit
                       ? QString("fit") : QString("manual");
            case ColFlagged: return QVariant();
        }
    }

    if (role == Qt::FontRole &&
        p->getRVSource() == RadialVelocityPoint::RVSource::Manual) {
        QFont f; f.setItalic(true);
        return f;
    }
    if (role == Qt::TextAlignmentRole) {
        switch (idx.column()) {
            case ColMJD: case ColBJD:
            case ColRV: case ColErrFormal: case ColErrSystematic:
                return int(Qt::AlignRight | Qt::AlignVCenter);
            case ColSource: case ColFlagged:
                return int(Qt::AlignCenter);
            default:
                return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    return {};
}

bool RVPointsTableModel::setData(const QModelIndex& idx, const QVariant& value, int role)
{
    if (!idx.isValid() || idx.row() >= static_cast<int>(_points.size())) return false;
    auto& p = _points[idx.row()];
    if (!p) return false;

    auto promote = [&p]() {
        if (p->getRVSource() == RadialVelocityPoint::RVSource::FromFit)
            p->setRVSource(RadialVelocityPoint::RVSource::Manual);
    };
    bool changed = false;

    if (role == Qt::CheckStateRole && idx.column() == ColFlagged) {
        bool nf = (value.toInt() == Qt::Checked);
        if (p->isFlagged() == nf) return true;
        p->setFlagged(nf);
        if (auto sp = linkedSpectrum(p)) {
            for (auto& f : sp->getSpectralFits()) {
                if (f && f->getId() == p->getSpectralFitId()) {
                    f->setFlagged(nf);
                    sp->notifyFitChanged(f);
                    if (_dbm) _dbm->updateSpectralFitFlag(f->getId(), nf);
                    break;
                }
            }
        }
        changed = true;
    }
    else if (role == Qt::EditRole) {
        bool ok = false;
        double v = value.toDouble(&ok);
        if (!ok) return false;
        switch (idx.column()) {
            case ColRV:
                if (p->getRV() != v) {
                    promote(); p->setRV(v); p->setRVManual(v); changed = true;
                } break;
            case ColErrFormal:
                if (p->getRVErrorFormal() != v) {
                    promote(); p->setRVErrorFormal(v);
                    p->setRVManualErrorFormal(v); changed = true;
                } break;
            case ColErrSystematic:
                if (p->getRVErrorSystematic() != v) {
                    p->setRVErrorSystematic(v);
                    p->setRVManualErrorSystematic(v); changed = true;
                } break;
            default: return false;
        }
    } else {
        return false;
    }

    if (changed) {
        if (_curve) _curve->persistPoint(p);
        if (_star)  _star->markSummaryDirty();
        emit dataChanged(index(idx.row(), 0), index(idx.row(), ColCount - 1));
        emit pointEdited(idx);
    }
    return true;
}

bool RVPointsTableModel::canResetToFit(int row) const
{
    if (row < 0 || row >= static_cast<int>(_points.size())) return false;
    const auto& p = _points[row];
    if (!p) return false;
    auto sp = linkedSpectrum(p);
    if (!sp) return false;
    if (!p->getSpectralFitId().isEmpty()) {
        for (const auto& f : sp->getSpectralFits())
            if (f && f->getId() == p->getSpectralFitId()) return true;
    }
    return sp->getBestFit() != nullptr;
}

void RVPointsTableModel::resetToFit(int row)
{
    if (!canResetToFit(row)) return;
    auto& p = _points[row];
    auto sp = linkedSpectrum(p);
    std::shared_ptr<SpectralFit> fit;
    if (!p->getSpectralFitId().isEmpty()) {
        for (auto& f : sp->getSpectralFits())
            if (f && f->getId() == p->getSpectralFitId()) { fit = f; break; }
    }
    if (!fit) fit = sp->getBestFit();
    if (!fit) return;

    p->setRVSource(RadialVelocityPoint::RVSource::FromFit);
    p->applyFromFit(*fit);
    if (_curve) _curve->persistPoint(p);
    if (_star)  _star->markSummaryDirty();
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
    emit pointEdited(index(row, 0));
}

// ═════════════════════════════════════════════════════════════════════════════
//   RVSolutionsWidget
// ═════════════════════════════════════════════════════════════════════════════

RVSolutionsWidget::RVSolutionsWidget(std::shared_ptr<Star> star,
                                     DatabaseManager* dbm,
                                     QWidget* parent)
    : QWidget(parent)
    , _star(std::move(star))
    , _dbm(dbm)
{
    buildUi();
    reload();
}

void RVSolutionsWidget::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* box = new QGroupBox("RV Solutions");
    outer->addWidget(box, 1);

    auto* v = new QVBoxLayout(box);

    _list = new QListWidget;
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(_list, 1);

    auto* btnRow = new QHBoxLayout;
    _addBtn  = new QPushButton("Add");
    _delBtn  = new QPushButton("Delete");
    _bestBtn = new QPushButton("Set as Best");
    btnRow->addWidget(_addBtn);
    btnRow->addWidget(_delBtn);
    btnRow->addWidget(_bestBtn);
    v->addLayout(btnRow);

    auto* paramBox = new QGroupBox("Parameters");
    auto* form = new QFormLayout(paramBox);

    auto mkSpin = [](double mn, double mx, int dec, double step = 0.0) {
        auto* s = new QDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        if (step > 0) s->setSingleStep(step);
        s->setKeyboardTracking(false);
        return s;
    };

    _periodSpin = mkSpin(0.0,    1.0e7, 6, 0.001);
    _kSpin      = mkSpin(-1.0e4, 1.0e4, 4, 0.1);
    _gammaSpin  = mkSpin(-1.0e4, 1.0e4, 4, 0.1);
    _phiSpin    = mkSpin(0.0,    1.0,   6, 0.001);
    _eccCheck   = new QCheckBox("Eccentric orbit");
    _eccSpin    = mkSpin(0.0,    0.999, 4, 0.01);
    _omegaSpin  = mkSpin(0.0,    360.0, 2, 1.0);

    _eccSpin  ->setEnabled(false);
    _omegaSpin->setEnabled(false);

    form->addRow("Period [d]",            _periodSpin);
    form->addRow("K [km/s]",              _kSpin);
    form->addRow("γ [km/s]",              _gammaSpin);
    form->addRow("φ (phase at first pt)", _phiSpin);
    form->addRow(_eccCheck);
    form->addRow("e",                     _eccSpin);
    form->addRow("ω [°]",                 _omegaSpin);

    v->addWidget(paramBox);

    _statsLabel = new QLabel;
    _statsLabel->setStyleSheet("color: gray; font-style: italic;");
    _statsLabel->setWordWrap(true);
    v->addWidget(_statsLabel);

    auto* applyRow = new QHBoxLayout;
    _revertBtn = new QPushButton("Revert");
    _applyBtn  = new QPushButton("Save");
    applyRow->addStretch();
    applyRow->addWidget(_revertBtn);
    applyRow->addWidget(_applyBtn);
    v->addLayout(applyRow);

    connect(_list, &QListWidget::currentRowChanged,
            this, &RVSolutionsWidget::onSelectionChanged);
    connect(_addBtn,    &QPushButton::clicked, this, &RVSolutionsWidget::onAddSolution);
    connect(_delBtn,    &QPushButton::clicked, this, &RVSolutionsWidget::onDeleteSolution);
    connect(_bestBtn,   &QPushButton::clicked, this, &RVSolutionsWidget::onSetAsBest);
    connect(_applyBtn,  &QPushButton::clicked, this, &RVSolutionsWidget::onApply);
    connect(_revertBtn, &QPushButton::clicked, this, &RVSolutionsWidget::onRevert);
    connect(_eccCheck,  &QCheckBox::toggled,   this, &RVSolutionsWidget::onEccentricToggled);

    for (auto* s : {_periodSpin, _kSpin, _gammaSpin, _phiSpin,
            _eccSpin, _omegaSpin}) {
    connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, &RVSolutionsWidget::onParamChanged);
    }
}

void RVSolutionsWidget::reload()
{
    rebuildList();
    if (_list->count() > 0) {
        // prefer the best fit; else first
        auto curve = _star ? _star->getRVCurve() : nullptr;
        int bestRow = 0;
        if (curve) {
            auto fits = curve->getRVFits();
            for (int i = 0; i < static_cast<int>(fits.size()); ++i)
                if (fits[i] && fits[i]->isBestFit()) { bestRow = i; break; }
        }
        _list->setCurrentRow(bestRow);
    } else {
        loadIntoEditor(nullptr);
        _displayed.reset();
        emit displayedFitChanged(nullptr);
    }
}

void RVSolutionsWidget::rebuildList()
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    int prevRow = _list->currentRow();

    _list->blockSignals(true);
    _list->clear();
    if (curve) {
        for (const auto& fit : curve->getRVFits()) {
            if (!fit) continue;
            QString star = fit->isBestFit() ? "★ " : "  ";
            QString label = QString("%1%2 — P=%3 d  K=%4 km/s")
                .arg(star)
                .arg(fit->getFitMethod().isEmpty() ? "fit" : fit->getFitMethod())
                .arg(fit->getPeriod(), 0, 'f', 4)
                .arg(fit->getK(),      0, 'f', 2);
            _list->addItem(label);
        }
    }
    if (prevRow >= 0 && prevRow < _list->count())
        _list->setCurrentRow(prevRow);
    _list->blockSignals(false);
}

std::shared_ptr<RVFit> RVSolutionsWidget::currentFit() const
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    if (!curve) return nullptr;
    int row = _list->currentRow();
    auto fits = curve->getRVFits();
    if (row < 0 || row >= static_cast<int>(fits.size())) return nullptr;
    return fits[row];
}

void RVSolutionsWidget::onSelectionChanged()
{
    auto fit = currentFit();
    _displayed = fit;
    loadIntoEditor(fit);
    takeSnapshot(fit);
    _delBtn->setEnabled(fit != nullptr);
    _bestBtn->setEnabled(fit && !fit->isBestFit());
    _applyBtn->setEnabled(fit != nullptr);
    _revertBtn->setEnabled(fit != nullptr);
    emit displayedFitChanged(fit);
}

void RVSolutionsWidget::loadIntoEditor(std::shared_ptr<RVFit> fit)
{
    _suppressSignals = true;
    if (fit) {
        _periodSpin->setValue(fit->getPeriod());
        _kSpin     ->setValue(fit->getK());
        _gammaSpin ->setValue(fit->getGamma());
        _phiSpin   ->setValue(fit->getPhi());
        _eccCheck  ->setChecked(fit->isEccentric());
        _eccSpin   ->setValue(fit->getEccentricity());
        _omegaSpin ->setValue(fit->getOmega());
        _eccSpin   ->setEnabled(fit->isEccentric());
        _omegaSpin ->setEnabled(fit->isEccentric());
    }
    _suppressSignals = false;
    updateStatsLabel(fit);
}

void RVSolutionsWidget::writeBackToFit(std::shared_ptr<RVFit> fit)
{
    if (!fit) return;
    fit->setPeriod(_periodSpin->value());
    fit->setK     (_kSpin     ->value());
    fit->setGamma (_gammaSpin ->value());
    fit->setPhi   (_phiSpin   ->value());
    const bool ecc = _eccCheck->isChecked();
    fit->setEccentric(ecc);
    if (ecc) {
        fit->setEccentricity(_eccSpin->value());
        fit->setOmega       (_omegaSpin->value());
    }
    // Back-compat: keep the (derived) _t0 field in sync for any external
    // readers that still consult RVFit::getT0().
    const double t0 = fit->getT0BJD();
    if (!std::isnan(t0)) fit->setT0(t0);
}

void RVSolutionsWidget::recomputeStats(std::shared_ptr<RVFit> fit)
{
    if (!fit) return;
    auto curve = _star ? _star->getRVCurve() : nullptr;
    if (!curve) return;
    fit->updateStatistics(curve->getRVPoints());
}

void RVSolutionsWidget::updateStatsLabel(std::shared_ptr<RVFit> fit)
{
    if (!fit) { _statsLabel->setText("No solution selected."); return; }

    auto fmt = [](double v, int prec) {
        return (std::isnan(v) || v == 0.0)
            ? QString("—")
            : QString::number(v, 'f', prec);
    };
    _statsLabel->setText(QString(
        "T₀ (BJD) = %1\n"
        "T₀ (MJD) = %2\n"
        "χ²       = %3\n"
        "RMS      = %4 km/s\n"
        "method:    %5")
        .arg(fmt(fit->getT0BJD(), 5),
             fmt(fit->getT0MJD(), 5),
             fmt(fit->getChi2(),  3),
             fmt(fit->getRms(),   3),
             fit->getFitMethod().isEmpty() ? "—" : fit->getFitMethod()));
}


void RVSolutionsWidget::takeSnapshot(std::shared_ptr<RVFit> fit)
{
    if (!fit) { _snapshot = {}; return; }
    _snapshot.id    = fit->getId();
    _snapshot.P     = fit->getPeriod();
    _snapshot.K     = fit->getK();
    _snapshot.gamma = fit->getGamma();
    _snapshot.phi   = fit->getPhi();
    _snapshot.e     = fit->getEccentricity();
    _snapshot.omega = fit->getOmega();
    _snapshot.ecc   = fit->isEccentric();
}

void RVSolutionsWidget::onParamChanged()
{
    if (_suppressSignals) return;
    auto fit = currentFit();
    if (!fit) return;
    writeBackToFit(fit);
    recomputeStats(fit);
    updateStatsLabel(fit);
    emit displayedFitChanged(fit);
}

void RVSolutionsWidget::onEccentricToggled(bool on)
{
    _eccSpin  ->setEnabled(on);
    _omegaSpin->setEnabled(on);
    if (_suppressSignals) return;
    onParamChanged();
}

void RVSolutionsWidget::onApply()
{
    auto fit = currentFit();
    if (!fit) return;
    writeBackToFit(fit);
    recomputeStats(fit);
    auto curve = _star ? _star->getRVCurve() : nullptr;
    if (_dbm && curve) _dbm->saveRVFit(fit, curve->getId());
    takeSnapshot(fit);
    rebuildList();
    if (_star && fit->isBestFit()) _star->markSummaryDirty();
    updateStatsLabel(fit);
    emit fitsChanged();
    emit displayedFitChanged(fit);
}

void RVSolutionsWidget::onRevert()
{
    auto fit = currentFit();
    if (!fit || fit->getId() != _snapshot.id) return;
    fit->setPeriod (_snapshot.P);
    fit->setK      (_snapshot.K);
    fit->setGamma  (_snapshot.gamma);
    fit->setPhi    (_snapshot.phi);
    fit->setEccentric(_snapshot.ecc);
    if (_snapshot.ecc) {
        fit->setEccentricity(_snapshot.e);
        fit->setOmega(_snapshot.omega);
    }
    recomputeStats(fit);
    loadIntoEditor(fit);
    emit displayedFitChanged(fit);
}

void RVSolutionsWidget::onAddSolution()
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    if (!curve) return;

    auto fit = std::make_shared<RVFit>();
    fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    fit->setCurveId(curve->getId());
    fit->setCreationDate(QDateTime::currentDateTime());
    fit->setFitMethod("manual");

    const double minRV = curve->getMinRV();
    const double maxRV = curve->getMaxRV();
    const double mean  = curve->getMeanRV();
    const double span  = curve->getTimeSpan();

    fit->setK(std::isnan(maxRV - minRV)
                  ? 50.0
                  : std::max(1.0, (maxRV - minRV) * 0.5));
    fit->setGamma (std::isnan(mean) ? 0.0 : mean);
    fit->setPeriod(span > 0 ? std::max(0.1, span * 0.1) : 1.0);
    fit->setPhi(0.0);            // first datapoint at phase 0
    fit->setEccentric(false);
    fit->setEccentricity(0.0);
    fit->setOmega(0.0);
    fit->setBestFit(false);

    curve->addRVFit(fit);                 // auto-binds reference time
    fit->updateStatistics(curve->getRVPoints());
    if (_dbm) _dbm->saveRVFit(fit, curve->getId());

    rebuildList();
    _list->setCurrentRow(_list->count() - 1);
    emit fitsChanged();
}


void RVSolutionsWidget::onDeleteSolution()
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    auto fit = currentFit();
    if (!curve || !fit) return;

    bool wasBest = fit->isBestFit();
    QString fitId = fit->getId();
    curve->removeRVFit(fitId);

    // TODO: persist deletion — DatabaseManager::deleteRVFit not yet exposed.
    // The fit will reappear on next reload until that's added.
    LOG_WARNING("Tools",
        "RV Inspector: removed fit in-memory only — DB delete not implemented.");

    rebuildList();
    if (_list->count() > 0) _list->setCurrentRow(0);
    else { _displayed.reset(); emit displayedFitChanged(nullptr); }

    if (wasBest && _star) _star->markSummaryDirty();
    emit fitsChanged();
}

void RVSolutionsWidget::onSetAsBest()
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    auto fit = currentFit();
    if (!curve || !fit) return;

    curve->setBestFit(fit->getId());
    if (_dbm) {
        for (auto& f : curve->getRVFits())
            if (f) _dbm->saveRVFit(f, curve->getId());
    }
    rebuildList();
    if (_star) _star->markSummaryDirty();
    _bestBtn->setEnabled(false);
    emit fitsChanged();
    emit displayedFitChanged(fit);
}

// ═════════════════════════════════════════════════════════════════════════════
//   RVInspectorDialog
// ═════════════════════════════════════════════════════════════════════════════

RVInspectorDialog::RVInspectorDialog(std::shared_ptr<Star> star,
                                     DatabaseManager* dbm,
                                     ApplicationController* controller,
                                     const QString& projectId,
                                     QWidget* parent)
    : QDialog(parent)
    , _star(std::move(star))
    , _dbm(dbm)
    , _controller(controller)
    , _projectId(projectId)
{
    setupUi();
    LOG_INFO("Tools", QString("RV Inspector opened for star %1")
        .arg(_star->getSourceId()));
}

RVInspectorDialog::~RVInspectorDialog()
{
    // Restore Star's curve change-callback (RVPanel hijacks it on construct).
    if (_star) {
        if (auto curve = _star->getRVCurve()) {
            std::weak_ptr<Star> ws = _star;
            curve->setChangeCallback([ws]() {
                if (auto s = ws.lock()) s->markSummaryDirty();
            });
        }
    }
}

void RVInspectorDialog::setupUi()
{
    setWindowTitle(QString("RV Inspector — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 900);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);

    // Main horizontal split: [ left col (plot+table) | solutions full height ]
    auto* mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->setOpaqueResize(false);

    // Left: vertical split (plot above, table below)
    auto* leftSplit = new QSplitter(Qt::Vertical);
    leftSplit->setOpaqueResize(false);

    DetailPanel::Context ctx { _star, _dbm, _controller, _projectId };
    _plotPanel = new RVPanel(ctx);
    leftSplit->addWidget(_plotPanel);

    auto* tableBox = new QGroupBox("RV Points");
    auto* tableLay = new QVBoxLayout(tableBox);

    auto curve = _star ? _star->getRVCurve() : nullptr;
    _pointsModel = new RVPointsTableModel(_star, curve, _dbm, this);
    _pointsTable = new QTableView;
    _pointsTable->setModel(_pointsModel);
    _pointsTable->setAlternatingRowColors(true);
    _pointsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _pointsTable->setEditTriggers(QAbstractItemView::DoubleClicked
                                | QAbstractItemView::SelectedClicked
                                | QAbstractItemView::EditKeyPressed);
    _pointsTable->verticalHeader()->setVisible(false);
    _pointsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _pointsTable->horizontalHeader()->setStretchLastSection(true);
    _pointsTable->setSortingEnabled(false);
    _pointsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(_pointsTable, &QWidget::customContextMenuRequested,
            this, &RVInspectorDialog::onTableContextMenu);
    tableLay->addWidget(_pointsTable);
    leftSplit->addWidget(tableBox);

    leftSplit->setStretchFactor(0, 3);
    leftSplit->setStretchFactor(1, 2);

    mainSplit->addWidget(leftSplit);

    // Right: solutions widget, full height
    _solutions = new RVSolutionsWidget(_star, _dbm);
    mainSplit->addWidget(_solutions);
    mainSplit->setStretchFactor(0, 4);
    mainSplit->setStretchFactor(1, 1);

    outer->addWidget(mainSplit, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    // ── Wiring ───────────────────────────────────────────────────────────
    connect(_pointsModel, &RVPointsTableModel::pointEdited,
            this, [this](const QModelIndex&) {
        if (_plotPanel) _plotPanel->refresh();
    });

    connect(_solutions, &RVSolutionsWidget::displayedFitChanged,
            this, [this](std::shared_ptr<RVFit> fit) {
        if (_plotPanel) _plotPanel->setDisplayedFit(fit);
    });

    connect(_solutions, &RVSolutionsWidget::fitsChanged,
            this, [this]() {
        if (_pointsModel) _pointsModel->reload();
        if (_plotPanel)   _plotPanel->refresh();
    });

    // Initialise plot with whatever the solutions widget chose to display
    if (_solutions && _plotPanel)
        _plotPanel->setDisplayedFit(_solutions->displayedFit());
}

void RVInspectorDialog::onTableContextMenu(const QPoint& pos)
{
    QModelIndex idx = _pointsTable->indexAt(pos);
    if (!idx.isValid()) return;
    int row = idx.row();

    QMenu menu(this);
    QAction* resetAct = menu.addAction("Reset RV to fit value");
    resetAct->setEnabled(_pointsModel->canResetToFit(row));
    QAction* chosen = menu.exec(_pointsTable->viewport()->mapToGlobal(pos));
    if (chosen == resetAct) _pointsModel->resetToFit(row);
}