#include "rv/ui/RVImportPointsDialog.h"
#include "core/DelimitedTable.h"

#include "core/Star.h"
#include "spectra/Spectrum.h"
#include "core/Instrument.h"
#include "rv/RadialVelocity.h"
#include "rv/RVTableGuess.h"
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
#include <QProgressDialog>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <cmath>
#include <limits>

namespace {

// Order of the time-scale combo. ISO dates are read to MJD (UTC).
enum ScaleIndex { kMJD, kBJD, kJD, kHJD, kBTJD, kBKJD, kTCB, kISO };

int indexForScale(TimeScale s)
{
    switch (s) {
    case TimeScale::MJD:     return kMJD;
    case TimeScale::BJD:     return kBJD;
    case TimeScale::JD:      return kJD;
    case TimeScale::HJD:     return kHJD;
    case TimeScale::BTJD:    return kBTJD;
    case TimeScale::BKJD:    return kBKJD;
    case TimeScale::GaiaTCB: return kTCB;
    case TimeScale::Unknown: return kMJD;
    }
    return kMJD;
}

bool isBarycentric(TimeScale s)
{
    return s == TimeScale::BJD || s == TimeScale::BTJD ||
           s == TimeScale::BKJD || s == TimeScale::GaiaTCB;
}

QString crushName(const QString& s)
{
    return s.toLower().remove(' ').remove('-').remove('_').remove('/');
}

}   // namespace

// ════════════════════════════════════════════════════════════════
// Construction / UI
// ════════════════════════════════════════════════════════════════

RVImportPointsDialog::RVImportPointsDialog(std::shared_ptr<Star> star,
                                           DatabaseManager* dbm,
                                           QWidget* parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm)
{
    setWindowTitle("Import RV points from CSV");
    resize(720, 640);
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
    _headerCheck->setToolTip(
        "Detected from the file: a first row with text where the rows below "
        "hold numbers is a header. A '#'-commented line right above the data "
        "is used for column names when there is no header row.");
    optRow->addWidget(_headerCheck);
    optRow->addStretch();
    outer->addLayout(optRow);

    connect(_delimCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RVImportPointsDialog::onReloadFile);
    connect(_headerCheck, &QCheckBox::toggled, this, [this](bool) {
        _headerChosen = true;
        onReloadFile();
    });

    // ── Column mapping ───────────────────────────────────────────
    auto* colGroup = new QGroupBox("Column Mapping");
    auto* colGrid = new QGridLayout;
    int row = 0;

    colGrid->addWidget(new QLabel("Timestamp column:"), row, 0);
    _timeColCombo = new QComboBox;
    colGrid->addWidget(_timeColCombo, row, 1);
    _timeTypeCombo = new QComboBox;
    _timeTypeCombo->addItems({"MJD", "BJD", "JD", "HJD",
                              "BTJD (TESS)", "BKJD (Kepler)", "Gaia TCB",
                              "ISO date (UTC)"});
    _timeTypeCombo->setToolTip(
        "Scale of the timestamp column, guessed from its name and its values. "
        "MJD, JD, HJD and ISO dates are converted to BJD using the row's "
        "instrument and the star's coordinates.");
    colGrid->addWidget(_timeTypeCombo, row++, 2);

    colGrid->addWidget(new QLabel("Epoch offset:"), row, 0);
    _epochOffsetEdit = new QLineEdit;
    _epochOffsetEdit->setPlaceholderText(QStringLiteral("0"));
    _epochOffsetEdit->setToolTip(
        "Added to every timestamp before it is read on the scale above, for "
        "tables that tabulate a reduced Julian date such as HJD-2450000. "
        "Filled in automatically when the file states it or the values imply it.");
    colGrid->addWidget(_epochOffsetEdit, row++, 1);

    colGrid->addWidget(new QLabel("RV column [km/s]:"), row, 0);
    _rvColCombo = new QComboBox;
    colGrid->addWidget(_rvColCombo, row++, 1);

    colGrid->addWidget(new QLabel("RV error column:"), row, 0);
    _errColCombo = new QComboBox;
    colGrid->addWidget(_errColCombo, row, 1);
    _defaultErrEdit = new QLineEdit;
    _defaultErrEdit->setPlaceholderText("default error [km/s]");
    _defaultErrEdit->setToolTip(
        "RV error [km/s] given to rows that have none (no error column, or a "
        "blank or zero cell, and no systematic error). Needed only when such "
        "rows exist: a point without an error would get infinite weight in a fit.");
    colGrid->addWidget(_defaultErrEdit, row++, 2);

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

    colGrid->setColumnStretch(1, 1);
    colGroup->setLayout(colGrid);
    outer->addWidget(colGroup);

    for (auto* c : {_timeColCombo, _rvColCombo, _errColCombo, _sysErrColCombo,
                    _instColCombo, _compColCombo})
        fillColumnCombo(c, -1);

    connect(_timeColCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyScaleGuess(); refreshPreview(); });
    // A scale or offset the user picks replaces whatever the guess said.
    connect(_timeTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { _scaleNote.clear(); refreshPreview(); });
    connect(_epochOffsetEdit, &QLineEdit::textEdited,
            this, [this](const QString&) { _scaleNote.clear(); });
    connect(_epochOffsetEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { refreshPreview(); });
    connect(_defaultErrEdit, &QLineEdit::textChanged,
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
        "<i>Every point gets the instrument named in the instrument column, "
        "else the default instrument. MJD, JD, HJD and ISO timestamps need it, "
        "with the star's coordinates, to compute BJD; timestamps already on a "
        "barycentric scale (BJD, BTJD, BKJD, Gaia TCB) do not. Instruments can "
        "still be reassigned per point afterwards in the RV points table.</i>");
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
// File reading
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
    // The comment directly above the data: some tables put their header
    // there ("# BJD RV err") so that numpy.loadtxt skips it.
    QString commentAboveData;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        // Comment lines are not data, but they are not noise either: a VizieR
        // export states a column's epoch offset only there.
        if (line.startsWith('#')) {
            _metadataLines << line;
            if (lines.isEmpty()) commentAboveData = line;
            continue;
        }
        lines << line;
    }
    file.close();
    if (lines.isEmpty()) return false;

    // Sniff the delimiter on a data line when there is one: a header row of
    // plain words separated by single spaces says less than the numbers do.
    QChar delim = delimiter();
    if (delim == '\0')
        delim = DelimitedTable::detectDelimiter(lines.size() > 1 ? lines[1] : lines[0]);

    std::vector<QStringList> cells;
    cells.reserve(lines.size());
    for (const QString& l : lines)
        cells.push_back(DelimitedTable::splitLine(l, delim));

    if (!_headerChosen) {
        const std::vector<QStringList> below(
            cells.begin() + 1, cells.begin() + std::min<size_t>(cells.size(), 6));
        _hasHeader = RVTableGuess::looksLikeHeader(cells[0], below);
        QSignalBlocker block(_headerCheck);
        _headerCheck->setChecked(_hasHeader);
    } else {
        _hasHeader = _headerCheck->isChecked();
    }

    QStringList names;
    if (_hasHeader) {
        names = cells.front();
        _rows.assign(cells.begin() + 1, cells.end());
    } else {
        _rows = std::move(cells);
        if (!commentAboveData.isEmpty() && !_rows.empty()) {
            QString c = commentAboveData;
            c.remove(QRegularExpression(QStringLiteral("^#+")));
            const QStringList commentCells = DelimitedTable::splitLine(c.trimmed(), delim);
            const std::vector<QStringList> below(
                _rows.begin(), _rows.begin() + std::min<size_t>(_rows.size(), 5));
            if (commentCells.size() == _rows.front().size() &&
                RVTableGuess::looksLikeHeader(commentCells, below))
                names = commentCells;
        }
    }

    // Ragged tables: name every column any of the first rows reaches, and
    // give blank header cells (a pandas index column) a usable label.
    int width = names.size();
    for (size_t r = 0; r < std::min<size_t>(_rows.size(), 200); ++r)
        width = std::max<int>(width, _rows[r].size());
    for (int i = 0; i < width; ++i) {
        const QString n = i < names.size() ? names[i].trimmed() : QString();
        _columns << (n.isEmpty() ? QString("Column %1").arg(i + 1) : n);
    }
    return true;
}

void RVImportPointsDialog::fillColumnCombo(QComboBox* combo, int selected)
{
    QSignalBlocker block(combo);
    combo->clear();
    combo->addItem("(none)");
    combo->addItems(_columns);
    combo->setCurrentIndex(selected >= 0 && selected < _columns.size() ? selected + 1 : 0);
}

void RVImportPointsDialog::applyColumnGuess()
{
    // Placeholder names say nothing; a '#' header or a real one does.
    bool named = false;
    for (int i = 0; i < _columns.size(); ++i)
        if (_columns[i] != QString("Column %1").arg(i + 1)) { named = true; break; }

    const auto g = RVTableGuess::guessColumns(_columns, _rows, named);
    fillColumnCombo(_timeColCombo,   g.time);
    fillColumnCombo(_rvColCombo,     g.rv);
    fillColumnCombo(_errColCombo,    g.error);
    fillColumnCombo(_sysErrColCombo, g.sysError);
    fillColumnCombo(_instColCombo,   g.instrument);
    fillColumnCombo(_compColCombo,   g.component);
}

void RVImportPointsDialog::applyScaleGuess()
{
    _scaleNote.clear();
    const int col = _timeColCombo->currentIndex() - 1;
    if (col < 0 || col >= _columns.size()) return;

    const auto g = RVTableGuess::guessTimeScale(_columns[col], _metadataLines,
                                                _rows, col);
    {
        QSignalBlocker b1(_timeTypeCombo);
        QSignalBlocker b2(_epochOffsetEdit);
        _timeTypeCombo->setCurrentIndex(g.iso ? kISO : indexForScale(g.scale));
        _epochOffsetEdit->setText(g.offset == 0.0
                                      ? QString()
                                      : QString::number(g.offset, 'f', 1));
    }
    _scaleNote = g.note;
}

// ════════════════════════════════════════════════════════════════
// Instrument matching
// ════════════════════════════════════════════════════════════════

void RVImportPointsDialog::buildInstrumentLookup()
{
    _instByKey.clear();
    if (!_dbm) return;

    for (const auto& inst : _dbm->getAllInstruments()) {
        if (!inst) continue;
        if (!inst->getId().isEmpty())
            _instByKey.insert(inst->getId(), inst);
        const QString name = inst->getName();
        if (!name.isEmpty()) {
            _instByKey.insert(name.trimmed().toLower(), inst);
            _instByKey.insert(crushName(name), inst);
        }
    }
}

std::shared_ptr<Instrument> RVImportPointsDialog::matchInstrument(
    const QString& rawToken) const
{
    const QString token = rawToken.trimmed();
    if (token.isEmpty()) return nullptr;
    auto it = _instByKey.find(token.toLower());
    if (it != _instByKey.end()) return it.value();
    it = _instByKey.find(crushName(token));
    if (it != _instByKey.end()) return it.value();
    // Also match against an "Instrument/Mode" prefix.
    it = _instByKey.find(token.section('/', 0, 0).trimmed().toLower());
    if (it != _instByKey.end()) return it.value();
    return nullptr;
}

std::shared_ptr<Instrument> RVImportPointsDialog::defaultInstrument() const
{
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
    // A new file gets its header detected again.
    _headerChosen = false;
    onReloadFile();
}

void RVImportPointsDialog::onReloadFile()
{
    if (_fileEdit->text().trimmed().isEmpty()) return;

    if (!loadFile()) {
        QMessageBox::warning(this, "Load Error",
            "Could not read the selected file, or it holds no data rows.");
        _status->setText("Failed to load file.");
        _buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    applyColumnGuess();
    applyScaleGuess();
    refreshPreview();
}

double RVImportPointsDialog::epochOffset() const
{
    bool ok = false;
    const double v = RVTableGuess::parseNumber(_epochOffsetEdit->text(), &ok);
    return ok ? v : 0.0;
}

double RVImportPointsDialog::defaultError() const
{
    bool ok = false;
    const double v = RVTableGuess::parseNumber(_defaultErrEdit->text(), &ok);
    return (ok && v > 0.0) ? v : std::numeric_limits<double>::quiet_NaN();
}

TimeScale RVImportPointsDialog::selectedScale() const
{
    switch (_timeTypeCombo->currentIndex()) {
        case kMJD:  return TimeScale::MJD;
        case kBJD:  return TimeScale::BJD;
        case kJD:   return TimeScale::JD;
        case kHJD:  return TimeScale::HJD;
        case kBTJD: return TimeScale::BTJD;
        case kBKJD: return TimeScale::BKJD;
        case kTCB:  return TimeScale::GaiaTCB;
        case kISO:  return TimeScale::MJD;
        default:    return TimeScale::MJD;
    }
}

bool RVImportPointsDialog::isoDates() const
{
    return _timeTypeCombo->currentIndex() == kISO;
}

// ════════════════════════════════════════════════════════════════
// Row parsing (shared by the preview and the import)
// ════════════════════════════════════════════════════════════════

RVImportPointsDialog::ParseResult RVImportPointsDialog::parseRows() const
{
    ParseResult res;
    const int timeCol = _timeColCombo->currentIndex() - 1;
    const int rvCol   = _rvColCombo->currentIndex() - 1;
    if (timeCol < 0 || rvCol < 0) return res;

    const int errCol    = _errColCombo->currentIndex() - 1;
    const int sysErrCol = _sysErrColCombo->currentIndex() - 1;
    const int instCol   = _instColCombo->currentIndex() - 1;
    const int compCol   = _compColCombo->currentIndex() - 1;
    const TimeScale scale = selectedScale();
    const bool   iso    = isoDates();
    const double offset = iso ? 0.0 : epochOffset();
    const double defErr = defaultError();
    const auto   defInst = defaultInstrument();

    auto cell = [](const QStringList& row, int col) {
        return (col >= 0 && col < row.size()) ? row[col] : QString();
    };
    auto number = [&](const QStringList& row, int col, bool* ok) {
        return RVTableGuess::parseNumber(cell(row, col), ok);
    };

    QSet<QString> unknown;
    res.rows.reserve(_rows.size());
    for (const QStringList& row : _rows) {
        bool okT = false, okV = false;
        double t = 0.0;
        if (iso) okT = RVTableGuess::parseIsoDateMjd(cell(row, timeCol), &t);
        else     t = number(row, timeCol, &okT) + offset;
        const double rv = number(row, rvCol, &okV);
        if (!okT || !okV) { ++res.unparsed; continue; }

        // A value that cannot be a timestamp on the chosen scale is a missing
        // epoch offset, not an epoch. Converting it anyway is what put an MJD
        // of -2391132 in the database, so it is counted and reported instead.
        if (!iso && !Time::isPlausibleFor(t, scale)) {
            if (res.implausible++ == 0) res.firstImplausible = t;
            continue;
        }

        ParsedRow p;
        p.value = t;
        p.rv    = rv;

        bool ok = false;
        double v = number(row, errCol, &ok);
        if (ok && v > 0.0) p.errFormal = v;
        v = number(row, sysErrCol, &ok);
        if (ok && v > 0.0) p.errSys = v;
        if (p.errFormal <= 0.0 && p.errSys <= 0.0) {
            ++res.missingError;
            if (!std::isnan(defErr)) { p.errFormal = defErr; p.errDefaulted = true; }
        }

        const QString comp = cell(row, compCol).trimmed().toLower();
        if (comp == "2" || comp == "2.0" || comp == "b" || comp == "s" ||
            comp == "sec" || comp == "secondary")
            p.component = 2;

        const QString token = cell(row, instCol).trimmed();
        p.inst = matchInstrument(token);
        if (!p.inst) {
            if (!token.isEmpty()) unknown.insert(token);
            p.inst = defInst;
        }
        if (!p.inst) ++res.withoutInstrument;

        res.rows.push_back(std::move(p));
    }
    res.unknownInstruments = unknown.values();
    std::sort(res.unknownInstruments.begin(), res.unknownInstruments.end());
    return res;
}

Time RVImportPointsDialog::makeTime(const ParsedRow& r) const
{
    // The Time ctor handles fixed-offset scales (JD↔MJD, BTJD/BKJD/Gaia TCB
    // → BJD). MJD/JD still need a barycentric correction, and HJD a
    // heliocentric one first, both performed via the row's instrument;
    // resolveScales walks HJD → MJD → BJD, and the first leg only needs the
    // coordinates, so an HJD row still yields an MJD without an instrument.
    Time t(r.value, selectedScale());
    const double ra  = _star ? _star->getRa()  : std::numeric_limits<double>::quiet_NaN();
    const double dec = _star ? _star->getDec() : std::numeric_limits<double>::quiet_NaN();
    if (!std::isnan(ra) && !std::isnan(dec))
        t.resolveScales(r.inst.get(), ra, dec);
    return t;
}

// ════════════════════════════════════════════════════════════════
// Preview
// ════════════════════════════════════════════════════════════════

void RVImportPointsDialog::refreshPreview()
{
    const int timeCol = _timeColCombo->currentIndex() - 1;
    const int rvCol   = _rvColCombo->currentIndex() - 1;
    const bool showComp = _compColCombo->currentIndex() > 0;

    _preview->clear();
    QStringList headers{"MJD", "BJD", "RV [km/s]", "σ [km/s]", "Instrument"};
    if (showComp) headers << "Comp.";
    _preview->setColumnCount(headers.size());
    _preview->setHorizontalHeaderLabels(headers);
    _preview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    auto* okBtn = _buttons->button(QDialogButtonBox::Ok);
    if (_rows.empty()) {
        _preview->setRowCount(0);
        okBtn->setEnabled(false);
        return;
    }
    if (timeCol < 0 || rvCol < 0) {
        _preview->setRowCount(0);
        okBtn->setEnabled(false);
        _status->setText(timeCol < 0 && rvCol < 0
            ? "Map the timestamp and RV columns."
            : timeCol < 0 ? "Map the timestamp column."
                          : "Map the RV column.");
        return;
    }

    const ParseResult res = parseRows();
    const int valid = static_cast<int>(res.rows.size());

    const int shown = std::min(50, valid);
    _preview->setRowCount(shown);
    for (int r = 0; r < shown; ++r) {
        const ParsedRow& p = res.rows[r];
        const Time t = makeTime(p);
        auto setCell = [&](int c, const QString& s, bool dim = false,
                           Qt::Alignment align = Qt::AlignRight | Qt::AlignVCenter) {
            auto* it = new QTableWidgetItem(s);
            it->setTextAlignment(align);
            if (dim) it->setForeground(Qt::gray);
            _preview->setItem(r, c, it);
        };
        const auto mjd = t.mjd();
        const auto bjd = t.bjd();
        setCell(0, mjd ? QString::number(*mjd, 'f', 6) : "-", !mjd);
        setCell(1, bjd ? QString::number(*bjd, 'f', 6) : "not calculated", !bjd);
        setCell(2, QString::number(p.rv, 'f', 4));
        const double err = std::sqrt(p.errFormal * p.errFormal + p.errSys * p.errSys);
        if (err > 0.0) {
            setCell(3, QString::number(err, 'f', 4) + (p.errDefaulted ? " (default)" : ""),
                    p.errDefaulted);
        } else {
            setCell(3, "missing", true);
        }
        setCell(4, p.inst ? p.inst->getName() : QString("-"), !p.inst,
                Qt::AlignLeft | Qt::AlignVCenter);
        if (showComp) setCell(5, QString::number(p.component));
    }

    QStringList msg;
    msg << QString("%1 of %2 rows parse as valid RV points.")
               .arg(valid).arg(_rows.size());
    if (res.unparsed > 0)
        msg << QString("%1 row(s) skipped: no readable time or RV.").arg(res.unparsed);
    if (!_scaleNote.isEmpty())
        msg << _scaleNote;

    bool blocked = valid == 0;
    if (res.implausible > 0) {
        blocked = true;
        msg << QString("%1 row(s) hold a value (%2) that cannot be a %3: "
                       "this is a reduced Julian date; set the epoch offset "
                       "(commonly 2450000) or correct the scale.")
                   .arg(res.implausible)
                   .arg(res.firstImplausible, 0, 'f', 4)
                   .arg(_timeTypeCombo->currentText());
    }
    if (res.missingError > 0) {
        const double defErr = defaultError();
        if (std::isnan(defErr)) {
            blocked = true;
            msg << QString("<b>%1 row(s) have no RV error: enter a default RV "
                           "error to import them.</b>").arg(res.missingError);
        } else {
            msg << QString("%1 row(s) use the default RV error of %2 km/s.")
                       .arg(res.missingError).arg(defErr);
        }
    }
    if (!res.unknownInstruments.isEmpty()) {
        QStringList names = res.unknownInstruments.mid(0, 5);
        if (res.unknownInstruments.size() > 5) names << "…";
        msg << QString("Instrument values not recognised (%1); those rows use "
                       "the default instrument.").arg(names.join(", "));
    }
    const bool barycentric = !isoDates() && isBarycentric(selectedScale());
    if (!barycentric && res.withoutInstrument > 0) {
        const bool haveCoords = _star && !std::isnan(_star->getRa()) &&
                                !std::isnan(_star->getDec());
        if (!haveCoords)
            msg << "Star has no coordinates: BJD cannot be computed for "
                   "non-barycentric timestamps.";
        else
            msg << QString("%1 row(s) have no instrument: pick a default "
                           "instrument to convert their timestamps to BJD (it "
                           "can also be assigned per point afterwards).")
                       .arg(res.withoutInstrument);
    }

    _status->setText(msg.join(' '));
    okBtn->setEnabled(!blocked);
}

// ════════════════════════════════════════════════════════════════
// Import
// ════════════════════════════════════════════════════════════════

void RVImportPointsDialog::onAccept()
{
    const int timeCol = _timeColCombo->currentIndex() - 1;
    const int rvCol   = _rvColCombo->currentIndex() - 1;
    if (timeCol < 0 || rvCol < 0) {
        QMessageBox::warning(this, "Missing Columns",
            "Please map at least the timestamp and RV columns.");
        return;
    }

    const ParseResult res = parseRows();

    if (res.implausible > 0) {
        // Refuse rather than import a subset: a table with a reduced epoch
        // column has it on every row, so "some rows were skipped" would mean
        // the offset is wrong, not that a few rows are odd.
        QMessageBox::warning(this, "Epoch offset needed",
            QString("%1 row(s) hold a value such as %2, which cannot be a %3: "
                    "a full Julian date is around 2.45 million.\n\n"
                    "This table tabulates a reduced Julian date. Set the epoch "
                    "offset (2450000 is the usual one) or pick the scale the "
                    "column is really on, then import again.")
                .arg(res.implausible)
                .arg(res.firstImplausible, 0, 'f', 4)
                .arg(_timeTypeCombo->currentText()));
        return;
    }
    if (res.missingError > 0 && std::isnan(defaultError())) {
        QMessageBox::warning(this, "RV error needed",
            QString("%1 row(s) have no RV error. Enter a default RV error "
                    "[km/s] for them, then import again.")
                .arg(res.missingError));
        _defaultErrEdit->setFocus();
        return;
    }
    if (res.rows.empty()) {
        QMessageBox::warning(this, "No Points",
            "No valid RV points were parsed from the table.");
        return;
    }

    // Barycentric corrections run per row; show progress when that takes a
    // moment on a long table.
    const int total = static_cast<int>(res.rows.size());
    QProgressDialog progress("Converting timestamps...", QString(), 0, total, this);
    progress.setWindowTitle("Import RV points");
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(300);

    std::vector<std::shared_ptr<RadialVelocityPoint>> out;
    out.reserve(total);
    for (int i = 0; i < total; ++i) {
        const ParsedRow& r = res.rows[i];

        auto p = std::make_shared<RadialVelocityPoint>();
        p->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
        p->setSource("csv_import");
        p->setRVSource(RadialVelocityPoint::RVSource::Manual);
        p->setComponent(r.component);
        p->setTime(makeTime(r));
        if (r.inst) p->setInstrument(r.inst);

        p->setRV(r.rv);
        p->setRVErrorFormal(r.errFormal);
        p->setRVErrorSystematic(r.errSys);
        // Manual snapshot so values survive any later "reset to fit".
        p->setRVManual(r.rv);
        p->setRVManualErrorFormal(r.errFormal);
        p->setRVManualErrorSystematic(r.errSys);

        out.push_back(std::move(p));
        if (i % 25 == 0) {
            progress.setValue(i);
            QCoreApplication::processEvents();
        }
    }
    progress.setValue(total);

    _results = std::move(out);
    accept();
}
