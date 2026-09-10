#include "rv/ui/RVImportPointsDialog.h"
#include "core/DelimitedTable.h"

#include "core/Star.h"
#include "spectra/Spectrum.h"
#include "core/Instrument.h"
#include "rv/RadialVelocity.h"
#include "core/Time.h"
#include "db/DatabaseManager.h"

#include <algorithm>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <cmath>

// ════════════════════════════════════════════════════════════════
// Construction / UI
// ════════════════════════════════════════════════════════════════

RVImportPointsDialog::RVImportPointsDialog(std::shared_ptr<Star> star,
                                           DatabaseManager* dbm,
                                           QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm)
{
    setWindowTitle("Import RV points from CSV");
    resize(700, 600);
    setupUi();
}

void RVImportPointsDialog::setupUi()
{
    auto* outer = new QVBoxLayout(this);

    // ── File selection ───────────────────────────────────────────
    auto* fileRow = new QHBoxLayout;
    _fileEdit = new QLineEdit;
    _fileEdit->setPlaceholderText("Select RV table file (.csv, .txt, .dat, .tsv)…");
    _fileEdit->setReadOnly(true);
    fileRow->addWidget(_fileEdit);
    auto* browseBtn = new QPushButton("Browse…");
    connect(browseBtn, &QPushButton::clicked, this, &RVImportPointsDialog::onBrowse);
    fileRow->addWidget(browseBtn);
    outer->addLayout(fileRow);

    // ── Parsing options ──────────────────────────────────────────
    auto* optRow = new QHBoxLayout;
    optRow->addWidget(new QLabel("Delimiter:"));
    _delimCombo = new QComboBox;
    _delimCombo->addItems({"Auto-detect", "Comma (,)", "Tab", "Space", "Semicolon (;)"});
    optRow->addWidget(_delimCombo);
    _headerCheck = new QCheckBox("First row is header");
    _headerCheck->setChecked(true);
    optRow->addWidget(_headerCheck);
    optRow->addStretch();
    outer->addLayout(optRow);

    connect(_delimCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RVImportPointsDialog::onReloadFile);
    connect(_headerCheck, &QCheckBox::toggled,
            this, &RVImportPointsDialog::onReloadFile);

    // ── Column mapping ───────────────────────────────────────────
    auto* colGroup = new QGroupBox("Column Mapping");
    auto* colGrid = new QGridLayout;
    int row = 0;

    colGrid->addWidget(new QLabel("Timestamp column:"), row, 0);
    _timeColCombo = new QComboBox;
    colGrid->addWidget(_timeColCombo, row, 1);
    _timeTypeCombo = new QComboBox;
    _timeTypeCombo->addItems({"MJD", "BJD", "JD", "HJD",
                              "BTJD (TESS)", "BKJD (Kepler)", "Gaia TCB"});
    _timeTypeCombo->setToolTip(
        "Scale of the timestamp column. HJD is converted to MJD and BJD using "
        "the row's instrument and the star's coordinates.");
    colGrid->addWidget(_timeTypeCombo, row++, 2);

    colGrid->addWidget(new QLabel("Epoch offset:"), row, 0);
    _epochOffsetEdit = new QLineEdit;
    _epochOffsetEdit->setPlaceholderText(QStringLiteral("0"));
    _epochOffsetEdit->setToolTip(
        "Added to every timestamp before it is read on the scale above, for "
        "tables that tabulate a reduced Julian date such as HJD-2450000. "
        "Filled in automatically when the file states it.");
    colGrid->addWidget(_epochOffsetEdit, row++, 1);

    colGrid->addWidget(new QLabel("RV column [km/s]:"), row, 0);
    _rvColCombo = new QComboBox;
    colGrid->addWidget(_rvColCombo, row++, 1);

    colGrid->addWidget(new QLabel("RV error column:"), row, 0);
    _errColCombo = new QComboBox;
    colGrid->addWidget(_errColCombo, row++, 1);

    colGrid->addWidget(new QLabel("Systematic RV error column:"), row, 0);
    _sysErrColCombo = new QComboBox;
    colGrid->addWidget(_sysErrColCombo, row++, 1);

    colGrid->addWidget(new QLabel("Instrument column:"), row, 0);
    _instColCombo = new QComboBox;
    colGrid->addWidget(_instColCombo, row++, 1);

    colGrid->addWidget(new QLabel("Component column (1/2):"), row, 0);
    _compColCombo = new QComboBox;
    _compColCombo->setToolTip(
        "Optional stellar component per row for SB2 systems: "
        "1 = primary, 2 = secondary. Rows without a value are primary.");
    colGrid->addWidget(_compColCombo, row++, 1);

    colGroup->setLayout(colGrid);
    outer->addWidget(colGroup);

    connect(_timeColCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshPreview(); });
    connect(_timeTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshPreview(); });
    connect(_epochOffsetEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { refreshPreview(); });
    for (auto* c : {_rvColCombo, _errColCombo, _sysErrColCombo, _instColCombo,
                    _compColCombo})
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { refreshPreview(); });

    // ── Instrument (for BJD conversion) ──────────────────────────
    buildInstrumentLookup();

    auto* instGroup = new QGroupBox("Time Conversion");
    auto* instLay = new QVBoxLayout;
    auto* instRow = new QHBoxLayout;
    instRow->addWidget(new QLabel("Default instrument:"));
    _instCombo = new QComboBox;
    _instCombo->addItem("(none)", QString());

    // Prefer instruments already linked to this star's spectra, then fall
    // back to the full instrument database.
    QSet<QString> seen;
    if (_dbm && _star) {
        for (const auto& s : _star->getSpectra()) {
            if (!s) continue;
            const QString id = s->getInstrumentId();
            if (id.isEmpty() || seen.contains(id)) continue;
            seen.insert(id);
            if (auto inst = _dbm->getInstrumentById(id))
                _instCombo->addItem(inst->getName(), id);
        }
        for (const auto& inst : _dbm->getAllInstruments()) {
            if (!inst || seen.contains(inst->getId())) continue;
            seen.insert(inst->getId());
            _instCombo->addItem(inst->getName(), inst->getId());
        }
    }
    instRow->addWidget(_instCombo, 1);
    instLay->addLayout(instRow);

    auto* note = new QLabel(
        "<i>BJD is computed from MJD/JD using each point's instrument "
        "(matched from the instrument column above, else the default "
        "instrument) together with the star's coordinates. Timestamps "
        "already in a barycentric scale (BJD, BTJD, BKJD, Gaia TCB) need no "
        "instrument. Instruments can still be reassigned per point afterwards "
        "in the RV points table.</i>");
    note->setWordWrap(true);
    instLay->addWidget(note);
    instGroup->setLayout(instLay);
    outer->addWidget(instGroup);

    connect(_instCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshPreview(); });

    // ── Preview ──────────────────────────────────────────────────
    auto* previewGroup = new QGroupBox("Preview");
    auto* previewLay = new QVBoxLayout;
    _preview = new QTableWidget;
    _preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _preview->setSelectionMode(QAbstractItemView::NoSelection);
    _preview->setAlternatingRowColors(true);
    _preview->verticalHeader()->setVisible(false);
    previewLay->addWidget(_preview);
    previewGroup->setLayout(previewLay);
    outer->addWidget(previewGroup, 1);

    _status = new QLabel("Select a CSV/ASCII file to import RV points.");
    _status->setWordWrap(true);
    outer->addWidget(_status);

    _buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    _buttons->button(QDialogButtonBox::Ok)->setText("Import Points");
    _buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(_buttons, &QDialogButtonBox::accepted, this, &RVImportPointsDialog::onAccept);
    connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(_buttons);
}

// ════════════════════════════════════════════════════════════════
// CSV helpers
// ════════════════════════════════════════════════════════════════

QChar RVImportPointsDialog::delimiter() const
{
    switch (_delimCombo->currentIndex()) {
        case 1: return ',';
        case 2: return '\t';
        case 3: return ' ';
        case 4: return ';';
        default: return '\0';  // auto-detect
    }
}



bool RVImportPointsDialog::loadFile()
{
    _columns.clear();
    _rows.clear();
    _metadataLines.clear();

    const QString path = _fileEdit->text().trimmed();
    if (path.isEmpty()) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    QStringList lines;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        // Comment lines are not data, but they are not noise either: a VizieR
        // export states a column's epoch offset only there.
        if (line.startsWith('#')) { _metadataLines << line; continue; }
        lines << line;
    }
    file.close();
    if (lines.isEmpty()) return false;

    QChar delim = delimiter();
    if (delim == '\0') delim = DelimitedTable::detectDelimiter(lines.first());

    int startRow = 0;
    if (_headerCheck->isChecked()) {
        _columns = DelimitedTable::splitLine(lines[0], delim);
        startRow = 1;
    } else {
        int ncols = DelimitedTable::splitLine(lines[0], delim).size();
        for (int i = 0; i < ncols; ++i)
            _columns << QString("Column_%1").arg(i);
    }
    for (int i = startRow; i < lines.size(); ++i)
        _rows.push_back(DelimitedTable::splitLine(lines[i], delim));

    return true;
}

void RVImportPointsDialog::populateColumnCombo(QComboBox* combo,
                                               const QStringList& patterns)
{
    combo->blockSignals(true);
    combo->clear();
    combo->addItem("(none)");
    combo->addItems(_columns);

    int bestIdx = 0;
    for (int i = 0; i < _columns.size(); ++i) {
        QString col = _columns[i].toLower();
        for (const QString& pat : patterns) {
            if (col == pat || col.contains(pat)) { bestIdx = i + 1; break; }
        }
        if (bestIdx > 0) break;
    }
    combo->setCurrentIndex(bestIdx);
    combo->blockSignals(false);
}

// ════════════════════════════════════════════════════════════════
// Instrument matching
// ════════════════════════════════════════════════════════════════

void RVImportPointsDialog::buildInstrumentLookup()
{
    _instByKey.clear();
    if (!_dbm) return;

    auto crush = [](const QString& s) {
        return s.toLower().remove(' ').remove('-').remove('_').remove('/');
    };
    for (const auto& inst : _dbm->getAllInstruments()) {
        if (!inst) continue;
        if (!inst->getId().isEmpty())
            _instByKey.insert(inst->getId(), inst);
        const QString name = inst->getName();
        if (!name.isEmpty()) {
            _instByKey.insert(name.trimmed().toLower(), inst);
            _instByKey.insert(crush(name), inst);
        }
    }
}

std::shared_ptr<Instrument> RVImportPointsDialog::resolveRowInstrument(
    const QStringList& row, int instCol) const
{
    if (instCol >= 0 && instCol < row.size()) {
        const QString token = row[instCol].trimmed();
        if (!token.isEmpty()) {
            auto it = _instByKey.find(token.toLower());
            if (it != _instByKey.end()) return it.value();
            QString crushed = token.toLower().remove(' ').remove('-')
                                   .remove('_').remove('/');
            it = _instByKey.find(crushed);
            if (it != _instByKey.end()) return it.value();
            // Also match against an "Instrument/Mode" prefix.
            it = _instByKey.find(token.section('/', 0, 0).trimmed().toLower());
            if (it != _instByKey.end()) return it.value();
        }
    }
    // Fallback: the default instrument selector.
    const QString defId = _instCombo->currentData().toString();
    if (!defId.isEmpty() && _dbm)
        return _dbm->getInstrumentById(defId);
    return nullptr;
}

// ════════════════════════════════════════════════════════════════
// Slots
// ════════════════════════════════════════════════════════════════

void RVImportPointsDialog::onBrowse()
{
    QString file = QFileDialog::getOpenFileName(
        this, "Select RV Table File", QString(),
        "Data Files (*.csv *.txt *.dat *.tsv);;All Files (*)");
    if (file.isEmpty()) return;
    _fileEdit->setText(file);
    onReloadFile();
}

void RVImportPointsDialog::onReloadFile()
{
    if (_fileEdit->text().trimmed().isEmpty()) return;

    if (!loadFile()) {
        QMessageBox::warning(this, "Load Error",
            "Could not read or parse the selected file.");
        _status->setText("Failed to load file.");
        _buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    populateColumnCombo(_timeColCombo,
        {"mjd", "bjd", "time", "jd", "timestamp", "epoch"});
    populateColumnCombo(_rvColCombo,
        {"rv", "vrad", "radial_velocity", "v_rad", "radvel", "velocity"});
    populateColumnCombo(_errColCombo,
        {"rv_err", "rv_error", "e_rv", "vrad_err", "vrad_error", "e_vrad",
         "sigma_rv", "err"});
    populateColumnCombo(_sysErrColCombo,
        {"sys", "systematic", "sys_err", "sys_error", "rv_sys", "sigma_sys"});
    populateColumnCombo(_instColCombo,
        {"instrument", "inst", "telescope", "spectrograph", "detector"});
    populateColumnCombo(_compColCombo,
        {"component", "comp", "star_component"});

    // Auto-detect the time scale from the chosen column's name.
    int timeIdx = _timeColCombo->currentIndex() - 1;
    if (timeIdx >= 0 && timeIdx < _columns.size()) {
        const QString tn = _columns[timeIdx].toLower();
        // Longest names first: "btjd" also contains "jd", and "hjd" has to be
        // recognised before the bare-JD fallback claims it.
        if      (tn.contains("btjd")) _timeTypeCombo->setCurrentIndex(4);
        else if (tn.contains("bkjd")) _timeTypeCombo->setCurrentIndex(5);
        else if (tn.contains("tcb"))  _timeTypeCombo->setCurrentIndex(6);
        else if (tn.contains("bjd"))  _timeTypeCombo->setCurrentIndex(1);
        else if (tn.contains("hjd"))  _timeTypeCombo->setCurrentIndex(3);
        else if (tn.contains("jd") && !tn.contains("mjd"))
                                      _timeTypeCombo->setCurrentIndex(2);
        else                          _timeTypeCombo->setCurrentIndex(0);

        // ... and the epoch offset from whatever the file says about that
        // column. A VizieR table names the column plain "HJD" and mentions the
        // reduction only in its '#Column' description.
        _epochOffsetEdit->setText(QString::number(
            Time::epochOffsetFor(_columns[timeIdx], _metadataLines), 'f', 1));
    }

    refreshPreview();
}

double RVImportPointsDialog::epochOffset() const
{
    bool ok = false;
    const double v = _epochOffsetEdit->text().trimmed().toDouble(&ok);
    return ok ? v : 0.0;
}

TimeScale RVImportPointsDialog::selectedScale() const
{
    switch (_timeTypeCombo->currentIndex()) {
        case 0: return TimeScale::MJD;
        case 1: return TimeScale::BJD;
        case 2: return TimeScale::JD;
        case 3: return TimeScale::HJD;
        case 4: return TimeScale::BTJD;
        case 5: return TimeScale::BKJD;
        case 6: return TimeScale::GaiaTCB;
        default: return TimeScale::MJD;
    }
}

void RVImportPointsDialog::refreshPreview()
{
    const int timeCol = _timeColCombo->currentIndex() - 1;
    const int rvCol   = _rvColCombo->currentIndex() - 1;

    _preview->clear();
    _preview->setColumnCount(5);
    _preview->setHorizontalHeaderLabels(
        {"MJD", "BJD", "RV [km/s]", "σ", "Instrument"});
    _preview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    const bool ready = timeCol >= 0 && rvCol >= 0 && !_rows.empty();
    _buttons->button(QDialogButtonBox::Ok)->setEnabled(ready);
    if (!ready) {
        _preview->setRowCount(0);
        if (!_rows.empty())
            _status->setText("Map at least the timestamp and RV columns.");
        return;
    }

    const int errCol    = _errColCombo->currentIndex() - 1;
    const int sysErrCol = _sysErrColCombo->currentIndex() - 1;
    const int instCol   = _instColCombo->currentIndex() - 1;
    const TimeScale scale  = selectedScale();
    const double    offset = epochOffset();

    const double ra  = _star ? _star->getRa()  : std::numeric_limits<double>::quiet_NaN();
    const double dec = _star ? _star->getDec() : std::numeric_limits<double>::quiet_NaN();
    const bool haveCoords = !std::isnan(ra) && !std::isnan(dec);

    const int maxPreview = std::min<int>(50, static_cast<int>(_rows.size()));
    _preview->setRowCount(maxPreview);

    int valid = 0;
    int implausible = 0;
    double firstImplausible = 0.0;
    bool needsInstrument = false;
    for (int r = 0; r < static_cast<int>(_rows.size()); ++r) {
        const QStringList& row = _rows[r];
        if (timeCol >= row.size() || rvCol >= row.size()) continue;
        bool okT, okV;
        double tVal = row[timeCol].toDouble(&okT) + offset;
        double rv   = row[rvCol].toDouble(&okV);
        if (!okT || !okV) continue;

        // A value that cannot be a timestamp on the chosen scale is a missing
        // epoch offset, not an epoch. Converting it anyway is what put an MJD
        // of -2391132 in the database, so it is counted and reported instead.
        if (!Time::isPlausibleFor(tVal, scale)) {
            if (implausible++ == 0) firstImplausible = tVal;
            continue;
        }

        auto inst = resolveRowInstrument(row, instCol);

        Time t(tVal, scale);
        // resolveScales walks HJD → MJD → BJD; the first leg only needs the
        // coordinates, so an HJD row still yields an MJD without an instrument.
        if (haveCoords) t.resolveScales(inst.get(), ra, dec);
        if (!t.hasBjd()) needsInstrument = true;

        if (r < maxPreview) {
            auto setCell = [&](int c, const QString& s, bool dim = false,
                               Qt::Alignment align = Qt::AlignRight | Qt::AlignVCenter) {
                auto* it = new QTableWidgetItem(s);
                it->setTextAlignment(align);
                if (dim) it->setForeground(Qt::gray);
                _preview->setItem(r, c, it);
            };
            auto mjd = t.mjd();
            auto bjd = t.bjd();
            setCell(0, mjd ? QString::number(*mjd, 'f', 6) : "-", !mjd);
            setCell(1, bjd ? QString::number(*bjd, 'f', 6) : "not calculated", !bjd);
            setCell(2, QString::number(rv, 'f', 4));

            double err = 0.0;
            if (errCol >= 0 && errCol < row.size()) err = row[errCol].toDouble();
            if (sysErrCol >= 0 && sysErrCol < row.size()) {
                double s = row[sysErrCol].toDouble();
                err = std::sqrt(err * err + s * s);
            }
            setCell(3, QString::number(err, 'f', 4));
            setCell(4, inst ? inst->getName() : QString("-"), !inst,
                    Qt::AlignLeft | Qt::AlignVCenter);
        }
        ++valid;
    }

    QString msg = QString("%1 of %2 rows parse as valid RV points.")
                      .arg(valid).arg(_rows.size());
    if (implausible > 0) {
        msg += QString(" %1 row(s) hold a value (%2) that cannot be a %3 - "
                       "this is a reduced Julian date; set the epoch offset "
                       "(commonly 2450000) or correct the scale.")
                   .arg(implausible)
                   .arg(firstImplausible, 0, 'f', 4)
                   .arg(_timeTypeCombo->currentText());
    }
    if (needsInstrument) {
        if (!haveCoords)
            msg += " Star has no coordinates – BJD cannot be computed for "
                   "non-barycentric timestamps.";
        else
            msg += " Map an instrument column or pick a default instrument to "
                   "convert MJD/JD/HJD timestamps to BJD (can also be assigned "
                   "per point afterwards).";
    }
    _status->setText(msg);
}

void RVImportPointsDialog::onAccept()
{
    const int timeCol   = _timeColCombo->currentIndex() - 1;
    const int rvCol     = _rvColCombo->currentIndex() - 1;
    const int errCol    = _errColCombo->currentIndex() - 1;
    const int sysErrCol = _sysErrColCombo->currentIndex() - 1;

    if (timeCol < 0 || rvCol < 0) {
        QMessageBox::warning(this, "Missing Columns",
            "Please map at least the timestamp and RV columns.");
        return;
    }

    const int instCol = _instColCombo->currentIndex() - 1;
    const int compCol = _compColCombo->currentIndex() - 1;
    const TimeScale scale  = selectedScale();
    const double    offset = epochOffset();

    const double ra  = _star ? _star->getRa()  : std::numeric_limits<double>::quiet_NaN();
    const double dec = _star ? _star->getDec() : std::numeric_limits<double>::quiet_NaN();
    const bool haveCoords = !std::isnan(ra) && !std::isnan(dec);

    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    int implausible = 0;
    double firstImplausible = 0.0;
    for (const QStringList& row : _rows) {
        if (timeCol >= row.size() || rvCol >= row.size()) continue;
        bool okT, okV;
        double tVal = row[timeCol].toDouble(&okT) + offset;
        double rv   = row[rvCol].toDouble(&okV);
        if (!okT || !okV) continue;

        // Same guard as the preview, and this is the one that matters: a
        // reduced Julian date read as a full one writes an epoch that is wrong
        // by the offset and looks like a number, so it has to stop here rather
        // than reach the database.
        if (!Time::isPlausibleFor(tVal, scale)) {
            if (implausible++ == 0) firstImplausible = tVal;
            continue;
        }

        auto inst = resolveRowInstrument(row, instCol);

        auto p = std::make_shared<RadialVelocityPoint>();
        p->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        p->setSource("csv_import");
        p->setRVSource(RadialVelocityPoint::RVSource::Manual);
        if (compCol >= 0 && compCol < row.size()) {
            bool okC; const int c = row[compCol].trimmed().toInt(&okC);
            if (okC && c == 2) p->setComponent(2);
        }

        // Build the timestamp; the Time ctor handles fixed-offset scales
        // (JD↔MJD, BTJD/BKJD/Gaia TCB → BJD). MJD/JD still need a
        // barycentric correction, and HJD a heliocentric one first, both
        // performed via the row's instrument.
        Time t(tVal, scale);
        p->setTime(t);
        if (haveCoords)
            p->time().resolveScales(inst.get(), ra, dec);
        if (inst) p->setInstrument(inst);

        double errFormal = 0.0;
        if (errCol >= 0 && errCol < row.size()) {
            bool ok; double v = row[errCol].toDouble(&ok);
            if (ok) errFormal = v;
        }
        double errSys = 0.0;
        if (sysErrCol >= 0 && sysErrCol < row.size()) {
            bool ok; double v = row[sysErrCol].toDouble(&ok);
            if (ok && v > 0.0) errSys = v;
        }

        p->setRV(rv);
        p->setRVErrorFormal(errFormal);
        p->setRVErrorSystematic(errSys);
        // Manual snapshot so values survive any later "reset to fit".
        p->setRVManual(rv);
        p->setRVManualErrorFormal(errFormal);
        p->setRVManualErrorSystematic(errSys);

        out.push_back(std::move(p));
    }

    if (implausible > 0) {
        // Refuse rather than import a subset: a table with a reduced epoch
        // column has it on every row, so "some rows were skipped" would mean
        // the offset is wrong, not that a few rows are odd.
        QMessageBox::warning(this, "Epoch offset needed",
            QString("%1 row(s) hold a value such as %2, which cannot be a %3 - "
                    "a full Julian date is around 2.45 million.\n\n"
                    "This table tabulates a reduced Julian date. Set the epoch "
                    "offset (2450000 is the usual one) or pick the scale the "
                    "column is really on, then import again.")
                .arg(implausible)
                .arg(firstImplausible, 0, 'f', 4)
                .arg(_timeTypeCombo->currentText()));
        return;
    }

    if (out.empty()) {
        QMessageBox::warning(this, "No Points",
            "No valid RV points were parsed from the table.");
        return;
    }

    _results = std::move(out);
    accept();
}
