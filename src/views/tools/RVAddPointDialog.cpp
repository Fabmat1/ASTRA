#include "RVAddPointDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/RadialVelocity.h"
#include "models/Time.h"
#include "db/DatabaseManager.h"

#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QSet>
#include <QUuid>

#include <cmath>

RVAddPointDialog::RVAddPointDialog(std::shared_ptr<Star> star,
                                   DatabaseManager* dbm,
                                   QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm)
{
    setWindowTitle("Add manual RV point");

    auto* form = new QFormLayout(this);

    auto mk = [](double mn, double mx, int dec, double step) {
        auto* s = new QDoubleSpinBox;
        s->setRange(mn, mx); s->setDecimals(dec); s->setSingleStep(step);
        s->setKeyboardTracking(false);
        return s;
    };

    _mjdSpin   = mk(0.0,    1.0e7, 6, 0.1);
    _rvSpin    = mk(-1.0e4, 1.0e4, 4, 0.1);
    _errFormal = mk(0.0,    1.0e4, 4, 0.1);
    _errSyst   = mk(0.0,    1.0e4, 4, 0.1);

    _instCombo = new QComboBox;
    _instCombo->addItem("(none)", QString());
    if (_dbm && _star) {
        QSet<QString> seen;
        for (const auto& s : _star->getSpectra()) {
            if (!s) continue;
            const QString id = s->getInstrumentId();
            if (id.isEmpty() || seen.contains(id)) continue;
            seen.insert(id);
            if (auto inst = _dbm->getInstrumentById(id))
                _instCombo->addItem(inst->getName(), id);
        }
    }

    form->addRow("MJD",            _mjdSpin);
    form->addRow("RV [km/s]",      _rvSpin);
    form->addRow("σ formal",       _errFormal);
    form->addRow("σ systematic",   _errSyst);
    form->addRow("Instrument",     _instCombo);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(_buttons);

    connect(_buttons, &QDialogButtonBox::accepted, this, &RVAddPointDialog::onAccept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RVAddPointDialog::onAccept()
{
    if (_mjdSpin->value() <= 0.0) {
        QMessageBox::warning(this, "Add manual RV point", "MJD must be > 0.");
        return;
    }

    auto p = std::make_shared<RadialVelocityPoint>();
    p->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    p->setSource("manual");
    p->setRVSource(RadialVelocityPoint::RVSource::Manual);

    p->setMJD(_mjdSpin->value());

    const double rv = _rvSpin->value();
    const double sf = _errFormal->value();
    const double ss = _errSyst->value();
    p->setRV(rv);
    p->setRVErrorFormal(sf);
    p->setRVErrorSystematic(ss);

    // Manual snapshot so the value survives any later "reset to fit" flow.
    p->setRVManual(rv);
    p->setRVManualErrorFormal(sf);
    p->setRVManualErrorSystematic(ss);

    const QString instId = _instCombo->currentData().toString();
    if (!instId.isEmpty() && _dbm) {
        if (auto inst = _dbm->getInstrumentById(instId)) {
            p->setInstrument(inst);
            if (_star) {
                const double ra  = _star->getRa();
                const double dec = _star->getDec();
                if (!std::isnan(ra) && !std::isnan(dec))
                    p->time().computeBJD(*inst, ra, dec);
            }
        }
    }

    _result = std::move(p);
    accept();
}