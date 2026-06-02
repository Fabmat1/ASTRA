#include "RVInspectorDialog.h"

#include "RVAddFitDialog.h"
#include "RVAddPointDialog.h"
#include "db/DatabaseManager.h"
#include "models/Instrument.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "models/Time.h"
#include "utils/Logger.h"
#include "views/panels/DetailPanel.h"
#include "views/panels/RVPanel.h"
#include "views/widgets/PreciseDoubleSpinBox.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QUuid>
#include <QVBoxLayout>

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
    if (_star) _curve = _star->getRVCurve();   // pick up newly created curves
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

    // Pasteable + full precision (15 significant digits, no rounding):
    // do NOT call setDecimals or the stored value gets truncated on paste.
    auto mkPrecise = [](double mn, double mx, double step = 0.0) {
        auto *s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        if (step > 0)
            s->setSingleStep(step);
        return s; // keyboardTracking off, decimals=15 from ctor
    };
    // Pasteable but limited display decimals (bounded angle / eccentricity).
    auto mkSpin = [](double mn, double mx, int dec, double step = 0.0) {
        auto *s = new PreciseDoubleSpinBox;
        s->setRange(mn, mx);
        s->setDecimals(dec);
        if (step > 0)
            s->setSingleStep(step);
        return s;
    };

    _periodSpin = mkPrecise(0.0, 1.0e7, 0.001);
    _kSpin      = mkPrecise(-1.0e4, 1.0e4, 0.1);
    _gammaSpin  = mkPrecise(-1.0e4, 1.0e4, 0.1);
    _phiSpin    = mkPrecise(0.0, 1.0, 0.001);
    _t0Spin     = mkPrecise(0.0, 1.0e7, 0.001);
    _useT0Check = new QCheckBox("Edit T₀ (BJD) instead of phase");
    _eccCheck   = new QCheckBox("Eccentric orbit");
    _eccSpin    = mkSpin(0.0, 0.999, 4, 0.01);
    _omegaSpin  = mkSpin(0.0, 360.0, 2, 1.0);

    _eccSpin->setEnabled(false);
    _omegaSpin->setEnabled(false);
    _t0Spin->setEnabled(false); // start in phase mode

    form->addRow("Period [d]", _periodSpin);
    form->addRow("K [km/s]", _kSpin);
    form->addRow("γ [km/s]", _gammaSpin);
    form->addRow(_useT0Check);
    form->addRow("φ (phase at first pt)", _phiSpin);
    form->addRow("T₀ [BJD]", _t0Spin);
    form->addRow(_eccCheck);
    form->addRow("e", _eccSpin);
    form->addRow("ω [°]", _omegaSpin);

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
    connect(_useT0Check, &QCheckBox::toggled, this, [this](bool on) {
        _t0Spin->setEnabled(on);
        _phiSpin->setEnabled(!on);
        if (_suppressSignals)
            return;
        onParamChanged(); 
    });

    for (auto *s : {_periodSpin, _kSpin, _gammaSpin, _phiSpin, _t0Spin,
                    _eccSpin, _omegaSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &RVSolutionsWidget::onParamChanged);
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
        _phiSpin->setValue(fit->getPhi());
        const double t0 = fit->getT0BJD();
        _t0Spin->setValue(std::isnan(t0) ? 0.0 : t0);
        _eccCheck  ->setChecked(fit->isEccentric());
        _eccSpin   ->setValue(fit->getEccentricity());
        _omegaSpin ->setValue(fit->getOmega());
        _eccSpin   ->setEnabled(fit->isEccentric());
        _omegaSpin ->setEnabled(fit->isEccentric());
    }
    _suppressSignals = false;
    updateStatsLabel(fit);
}

double RVSolutionsWidget::phiFromT0BJD(const std::shared_ptr<RVFit> &fit,
                                       double targetT0, double P) const {
    if (!fit || !(P > 0.0) || std::isnan(targetT0))
        return fit ? fit->getPhi() : 0.0;

    const double savedPhi = fit->getPhi();

    auto wrap01 = [](double x) {
        x = std::fmod(x, 1.0);
        if (x < 0.0)
            x += 1.0;
        return x;
    };
    // Distance to nearest period multiple (T0 is only defined mod P).
    auto modErr = [&](double t) {
        double d = std::fmod(t - targetT0, P);
        if (d < 0.0)
            d += P;
        return std::min(d, P - d);
    };

    fit->setPhi(0.0);
    const double A0 = fit->getT0BJD(); // T0BJD at φ = 0
    if (std::isnan(A0)) {
        fit->setPhi(savedPhi);
        return savedPhi;
    }

    const double cand1 = wrap01((A0 - targetT0) / P); // slope −P
    const double cand2 = wrap01((targetT0 - A0) / P); // slope +P

    fit->setPhi(cand1);
    const double e1 = modErr(fit->getT0BJD());
    fit->setPhi(cand2);
    const double e2 = modErr(fit->getT0BJD());

    fit->setPhi(savedPhi); // restore; caller sets final φ
    return (e1 <= e2) ? cand1 : cand2;
}

void RVSolutionsWidget::writeBackToFit(std::shared_ptr<RVFit> fit) {
    if (!fit)
        return;
    const double P = _periodSpin->value();
    fit->setPeriod(P);
    fit->setK(_kSpin->value());
    fit->setGamma(_gammaSpin->value());

    if (_useT0Check && _useT0Check->isChecked()) {
        fit->setPhi(phiFromT0BJD(fit, _t0Spin->value(), P));
    } else {
        fit->setPhi(_phiSpin->value());
    }

    const bool ecc = _eccCheck->isChecked();
    fit->setEccentric(ecc);
    if (ecc) {
        fit->setEccentricity(_eccSpin->value());
        fit->setOmega(_omegaSpin->value());
    }
    // Back-compat: keep the (derived) _t0 field in sync.
    const double t0 = fit->getT0BJD();
    if (!std::isnan(t0))
        fit->setT0(t0);
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

void RVSolutionsWidget::onParamChanged() {
    if (_suppressSignals)
        return;
    auto fit = currentFit();
    if (!fit)
        return;
    writeBackToFit(fit);
    recomputeStats(fit);

    // Mirror the value into the non-edited phase/T0 box for display.
    _suppressSignals = true;
    if (_useT0Check && _useT0Check->isChecked()) {
        _phiSpin->setValue(fit->getPhi());
    } else {
        const double t0 = fit->getT0BJD();
        if (!std::isnan(t0))
            _t0Spin->setValue(t0);
    }
    _suppressSignals = false;

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

    RVAddFitDialog dlg(_star, curve, _dbm, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const auto fits = dlg.resultFits();
    if (fits.isEmpty()) return;

    int firstNewRow = -1;
    for (const auto& fit : fits) {
        if (!fit) continue;
        curve->addRVFit(fit);                         // auto-binds reference time
        fit->updateStatistics(curve->getRVPoints());
        if (_dbm) _dbm->saveRVFit(fit, curve->getId());
        if (firstNewRow < 0) firstNewRow = (int)curve->getRVFits().size() - 1;
    }

    rebuildList();
    if (firstNewRow >= 0 && firstNewRow < _list->count())
        _list->setCurrentRow(firstNewRow);

    LOG_INFO("Tools", QString("RV Inspector: added %1 solution(s)").arg(fits.size()));
    emit fitsChanged();
}


void RVSolutionsWidget::onDeleteSolution()
{
    auto curve = _star ? _star->getRVCurve() : nullptr;
    auto fit = currentFit();
    if (!curve || !fit) return;

    bool wasBest = fit->isBestFit();
    QString fitId = fit->getId();

    if (_dbm && !_dbm->deleteRVFit(fitId)) {
        LOG_ERROR("Tools", QString("Failed to delete RV fit %1 from database").arg(fitId));
        return;
    }

    curve->removeRVFit(fitId);

    rebuildList();
    if (_list->count() > 0) _list->setCurrentRow(0);
    else { _displayed.reset(); emit displayedFitChanged(nullptr); }

    if (wasBest && _star) _star->markSummaryDirty();
    LOG_INFO("Tools", QString("RV Inspector: deleted fit %1").arg(fitId));
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

RVInspectorDialog::~RVInspectorDialog() = default;

void RVInspectorDialog::setupUi()
{
    setWindowTitle(QString("RV Inspector — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 900);

    if (_star) _star->ensureRVCurveSynced();
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

    auto* pointBtnRow = new QHBoxLayout;
    _addPointBtn = new QPushButton("Add manual point…");
    _actionBtn   = new QPushButton("Remove selected");
    _actionBtn->setEnabled(false);
    pointBtnRow->addStretch();
    pointBtnRow->addWidget(_addPointBtn);
    pointBtnRow->addWidget(_actionBtn);
    tableLay->addLayout(pointBtnRow);

    connect(_addPointBtn, &QPushButton::clicked,
            this, &RVInspectorDialog::onAddManualPoint);
    connect(_actionBtn,   &QPushButton::clicked,
            this, &RVInspectorDialog::onPointActionClicked);
    connect(_pointsTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection&, const QItemSelection&) {
                onPointSelectionChanged();
            });

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

    for (auto* btn : findChildren<QPushButton*>())
        btn->setAutoDefault(false);
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

bool RVPointsTableModel::canRemove(int row) const
{
    if (row < 0 || row >= static_cast<int>(_points.size())) return false;
    const auto& p = _points[row];
    return p
        && p->getRVSource() == RadialVelocityPoint::RVSource::Manual
        && p->getSpectrumId().isEmpty()      // truly free-standing
        && p->getSpectralFitId().isEmpty();
}

void RVPointsTableModel::removePoint(int row)
{
    if (!canRemove(row) || !_curve) return;
    auto p = _points[row];
    const QString id = p->getId();

    if (_dbm) _dbm->deleteRadialVelocityPoint(id);
    _curve->removeRVPoint(id);
    if (_star) _star->markSummaryDirty();
    reload();
}

void RVPointsTableModel::appendPoint(std::shared_ptr<RadialVelocityPoint> p)
{
    if (!p || !_star) return;
    _star->ensureRVCurveSynced();      // creates curve if missing
    _curve = _star->getRVCurve();
    if (!_curve) return;

    _curve->addRVPoint(p);             // assigns id + curveId, dedup-safe
    _curve->persistPoint(p);
    _star->markSummaryDirty();
    reload();
}

void RVInspectorDialog::onAddManualPoint()
{
    RVAddPointDialog dlg(_star, _dbm, this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto p = dlg.result();
    if (!p) return;

    _pointsModel->appendPoint(p);
    if (_plotPanel) _plotPanel->refresh();
}

void RVInspectorDialog::onPointSelectionChanged()
{
    const auto rows = _pointsTable->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        _actionBtn->setText("Remove selected");
        _actionBtn->setEnabled(false);
        return;
    }
    const int row = rows.first().row();
    if (_pointsModel->canRemove(row)) {
        _actionBtn->setText("Remove selected");
        _actionBtn->setEnabled(true);
    } else if (_pointsModel->canResetToFit(row)) {
        _actionBtn->setText("Reset to fit value");
        _actionBtn->setEnabled(true);
    } else {
        _actionBtn->setText("Remove selected");
        _actionBtn->setEnabled(false);
    }
}

void RVInspectorDialog::onPointActionClicked()
{
    const auto rows = _pointsTable->selectionModel()->selectedRows();
    if (rows.size() != 1) return;
    const int row = rows.first().row();

    if (_pointsModel->canRemove(row)) {
        _pointsModel->removePoint(row);
    } else if (_pointsModel->canResetToFit(row)) {
        _pointsModel->resetToFit(row);
    }
    if (_plotPanel) _plotPanel->refresh();
    onPointSelectionChanged();
}