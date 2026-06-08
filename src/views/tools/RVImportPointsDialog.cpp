#include "RVImportPointsDialog.h"

#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/Instrument.h"
#include "models/RadialVelocity.h"
#include "models/Time.h"
#include "db/DatabaseManager.h"

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
    _timeTypeCombo->addItems({"MJD", "BJD", "JD",
                              "BTJD (TESS)", "BKJD (Kepler)", "Gaia TCB"});
    colGrid->addWidget(_timeTypeCombo, row++, 2);

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

    colGroup->setLayout(colGrid);
    outer->addWidget(colGroup);

    connect(_timeColCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshPreview(); });
    connect(_timeTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshPreview(); });
    for (auto* c : {_rvColCombo, _errColCombo, _sysErrColCombo, _instColCombo})
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

QChar RVImportPointsDialog::detectDelimiter(const QString& line)
{
    int commas = line.count(',');
    int tabs   = line.count('\t');
    int semis  = line.count(';');
    int spaces = line.split(QRegularExpression("\\s+"),
                            Qt::SkipEmptyParts).size() - 1;
    int best = commas;
    QChar ch = ',';
    if (tabs > best)   { best = tabs;  ch = '\t'; }
    if (semis > best)  { best = semis; ch = ';'; }
    if (spaces > best) { ch = ' '; }
    return ch;
}

QStringList RVImportPointsDialog::parseLine(const QString& line, QChar delim)
{
    if (delim == ' ')
        return line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    QStringList result;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.length(); ++i) {
        QChar c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == delim && !inQuotes) {
            result << current.trimmed();
            current.clear();
        } else {
            current += c;
        }
    }
    result << current.trimmed();
    return result;
}

bool RVImportPointsDialog::loadFile()
{
    _columns.clear();
    _rows.clear();

    const QString path = _fileEdit->text().trimmed();
    if (path.isEmpty()) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    QStringList lines;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        lines << line;
    }
    file.close();
    if (lines.isEmpty()) return false;

    QChar delim = delimiter();
    if (delim == '\0') delim = detectDelimiter(lines.first());

    int startRow = 0;
    if (_headerCheck->isChecked()) {
        _columns = parseLine(lines[0], delim);
        startRow = 1;
    } else {
        int ncols = parseLine(lines[0], delim).size();
        for (int i = 0; i < ncols; ++i)
            _columns << QString("Column_%1").arg(i);
    }
    for (int i = startRow; i < lines.size(); ++i)
        _rows.push_back(parseLine(lines[i], delim));

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

    // Auto-detect the time scale from the chosen column's name.
    int timeIdx = _timeColCombo->currentIndex() - 1;
    if (timeIdx >= 0 && timeIdx < _columns.size()) {
        const QString tn = _columns[timeIdx].toLower();
        if      (tn.contains("btjd")) _timeTypeCombo->setCurrentIndex(3);
        else if (tn.contains("bkjd")) _timeTypeCombo->setCurrentIndex(4);
        else if (tn.contains("tcb"))  _timeTypeCombo->setCurrentIndex(5);
        else if (tn.contains("bjd"))  _timeTypeCombo->setCurrentIndex(1);
        else if (tn.contains("jd") && !tn.contains("mjd"))
                                      _timeTypeCombo->setCurrentIndex(2);
        else                          _timeTypeCombo->setCurrentIndex(0);
    }

    refreshPreview();
}

TimeScale RVImportPointsDialog::selectedScale() const
{
    switch (_timeTypeCombo->currentIndex()) {
        case 0: return TimeScale::MJD;
        case 1: return TimeScale::BJD;
        case 2: return TimeScale::JD;
        case 3: return TimeScale::BTJD;
        case 4: return TimeScale::BKJD;
        case 5: return TimeScale::GaiaTCB;
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
    const TimeScale scale = selectedScale();

    const double ra  = _star ? _star->getRa()  : std::numeric_limits<double>::quiet_NaN();
    const double dec = _star ? _star->getDec() : std::numeric_limits<double>::quiet_NaN();
    const bool haveCoords = !std::isnan(ra) && !std::isnan(dec);

    const int maxPreview = std::min<int>(50, static_cast<int>(_rows.size()));
    _preview->setRowCount(maxPreview);

    int valid = 0;
    bool needsInstrument = false;
    for (int r = 0; r < static_cast<int>(_rows.size()); ++r) {
        const QStringList& row = _rows[r];
        if (timeCol >= row.size() || rvCol >= row.size()) continue;
        bool okT, okV;
        double tVal = row[timeCol].toDouble(&okT);
        double rv   = row[rvCol].toDouble(&okV);
        if (!okT || !okV) continue;

        auto inst = resolveRowInstrument(row, instCol);

        Time t(tVal, scale);
        if (!t.hasBjd()) {
            if (inst && haveCoords) t.computeBJD(*inst, ra, dec);
            else                    needsInstrument = true;
        }

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
    if (needsInstrument) {
        if (!haveCoords)
            msg += " Star has no coordinates – BJD cannot be computed for "
                   "non-barycentric timestamps.";
        else
            msg += " Map an instrument column or pick a default instrument to "
                   "convert MJD/JD timestamps to BJD (can also be assigned per "
                   "point afterwards).";
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
    const TimeScale scale = selectedScale();

    const double ra  = _star ? _star->getRa()  : std::numeric_limits<double>::quiet_NaN();
    const double dec = _star ? _star->getDec() : std::numeric_limits<double>::quiet_NaN();
    const bool haveCoords = !std::isnan(ra) && !std::isnan(dec);

    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    for (const QStringList& row : _rows) {
        if (timeCol >= row.size() || rvCol >= row.size()) continue;
        bool okT, okV;
        double tVal = row[timeCol].toDouble(&okT);
        double rv   = row[rvCol].toDouble(&okV);
        if (!okT || !okV) continue;

        auto inst = resolveRowInstrument(row, instCol);

        auto p = std::make_shared<RadialVelocityPoint>();
        p->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        p->setSource("csv_import");
        p->setRVSource(RadialVelocityPoint::RVSource::Manual);

        // Build the timestamp; the Time ctor handles fixed-offset scales
        // (JD↔MJD, BTJD/BKJD/Gaia TCB → BJD). MJD/JD still need a
        // barycentric correction, performed via the row's instrument.
        Time t(tVal, scale);
        p->setTime(t);
        if (inst && haveCoords && !p->time().hasBjd())
            p->time().computeBJD(*inst, ra, dec);
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

    if (out.empty()) {
        QMessageBox::warning(this, "No Points",
            "No valid RV points were parsed from the table.");
        return;
    }

    _results = std::move(out);
    accept();
}
