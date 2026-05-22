#include "ImportLightcurve.h"

#include "db/DatabaseManager.h"
#include "models/Star.h"
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>
#include <limits>

namespace {

bool looksNumeric(const QString& s)
{
    const QString t = s.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    t.toDouble(&ok);
    return ok;
}

bool rowLooksLikeHeader(const QStringList& tokens)
{
    int nonNumeric = 0, total = 0;
    for (const QString& t : tokens) {
        const QString s = t.trimmed();
        if (s.isEmpty()) continue;
        ++total;
        if (!looksNumeric(s)) ++nonNumeric;
    }
    if (total == 0) return false;
    return nonNumeric * 2 >= total;   // ≥ half non-numeric → header
}

QChar guessDelimiter(const QString& line)
{
    const int comma = line.count(',');
    const int semi  = line.count(';');
    const int tab   = line.count('\t');
    if (comma >= semi && comma >= tab && comma > 0) return ',';
    if (semi  >= tab  && semi > 0)                  return ';';
    if (tab > 0)                                    return '\t';
    return ',';
}

QStringList splitRow(const QString& line, QChar delim)
{
    if (delim == ' ' || delim == '\t')
        return line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList out = line.split(delim);
    for (auto& s : out) s = s.trimmed();
    return out;
}

// Quality cut operators
struct CutOpEntry { const char* label; const char* tag; };
static const QList<CutOpEntry> kCutOps = {
    {"<",  "lt"},
    {"≤",  "le"},
    {"=",  "eq"},
    {"≠",  "ne"},
    {"≥",  "ge"},
    {">",  "gt"},
};

bool evalCutOp(const QString& tag, double v, double rhs)
{
    if (tag == "lt") return v <  rhs;
    if (tag == "le") return v <= rhs;
    if (tag == "eq") return v == rhs;
    if (tag == "ne") return v != rhs;
    if (tag == "ge") return v >= rhs;
    if (tag == "gt") return v >  rhs;
    return true;
}

struct TimeScaleEntry { TimeScale ts; const char* label; };
static const QList<TimeScaleEntry> kTimeScales = {
    { TimeScale::JD,      "JD"      },
    { TimeScale::MJD,     "MJD"     },
    { TimeScale::BJD,     "BJD"     },
    { TimeScale::HJD,     "HJD"     },
    { TimeScale::BTJD,    "BTJD (TESS, BJD − 2457000)" },
    { TimeScale::BKJD,    "BKJD (Kepler, BJD − 2454833)" },
    { TimeScale::GaiaTCB, "Gaia TCB (BJD − 2455197.5)"   },
};

} // anon

// ─────────────────────────────────────────────────────────────────────
ImportLightcurveDialog::ImportLightcurveDialog(std::shared_ptr<Star> star,
                                               DatabaseManager*      dbm,
                                               QWidget*              parent)
    : QDialog(parent)
    , _star(std::move(star))
    , _dbm(dbm)
{
    setWindowTitle(tr("Import lightcurve from CSV"));
    resize(900, 720);
    buildUi();
    populateInstruments();
}

void ImportLightcurveDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ── File picker ──
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("CSV file:")));
        _filePath = new QLineEdit;
        _filePath->setPlaceholderText(tr("Path to a .csv / .txt file…"));
        _browseBtn = new QPushButton(tr("Browse…"));
        row->addWidget(_filePath, 1);
        row->addWidget(_browseBtn);
        root->addLayout(row);
        connect(_browseBtn, &QPushButton::clicked, this, &ImportLightcurveDialog::onBrowse);
        connect(_filePath,  &QLineEdit::editingFinished,
                this, &ImportLightcurveDialog::reloadFile);
    }

    // ── Instrument / mode ──
    {
        auto* box = new QGroupBox(tr("Instrument"));
        auto* form = new QFormLayout(box);
        _instrumentCombo = new QComboBox;
        _modeCombo       = new QComboBox;
        form->addRow(tr("Instrument:"), _instrumentCombo);
        form->addRow(tr("Photometric mode:"), _modeCombo);
        root->addWidget(box);
        connect(_instrumentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ImportLightcurveDialog::onInstrumentChanged);
    }

    // ── Columns / time scale / default filter ──
    {
        auto* box = new QGroupBox(tr("Columns"));
        auto* grid = new QGridLayout(box);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);

        _timeColCombo    = new QComboBox;
        _fluxColCombo    = new QComboBox;
        _fluxErrColCombo = new QComboBox;
        _filterColCombo  = new QComboBox;
        _timeScaleCombo  = new QComboBox;
        _defaultFilter   = new QLineEdit;
        _defaultFilter->setPlaceholderText(tr("e.g. V, R, g, TESS…"));

        for (const auto& t : kTimeScales)
            _timeScaleCombo->addItem(t.label, int(t.ts));
        _timeScaleCombo->setCurrentIndex(1);   // default MJD

        grid->addWidget(new QLabel(tr("Time column:")),     0, 0);
        grid->addWidget(_timeColCombo,                       0, 1);
        grid->addWidget(new QLabel(tr("Time scale:")),       0, 2);
        grid->addWidget(_timeScaleCombo,                     0, 3);

        grid->addWidget(new QLabel(tr("Flux column:")),      1, 0);
        grid->addWidget(_fluxColCombo,                       1, 1);
        grid->addWidget(new QLabel(tr("Flux error column:")),1, 2);
        grid->addWidget(_fluxErrColCombo,                    1, 3);

        grid->addWidget(new QLabel(tr("Filter column:")),    2, 0);
        grid->addWidget(_filterColCombo,                     2, 1);
        grid->addWidget(new QLabel(tr("Default filter:")),   2, 2);
        grid->addWidget(_defaultFilter,                      2, 3);

        root->addWidget(box);
    }

    // ── Quality cuts ──
    {
        auto* box = new QGroupBox(tr("Quality cuts"));
        auto* v = new QVBoxLayout(box);
        auto* hint = new QLabel(tr(
            "Rows where any cut evaluates to <i>false</i> are dropped. "
            "Cuts compare a column value (parsed as a number) against a "
            "fixed number. Rows where the column can't be parsed are also dropped."));
        hint->setWordWrap(true);
        hint->setStyleSheet("color: gray;");
        v->addWidget(hint);

        _cutsTable = new QTableWidget(0, 3);
        _cutsTable->setHorizontalHeaderLabels({tr("Column"), tr("Op"), tr("Value")});
        _cutsTable->horizontalHeader()->setStretchLastSection(true);
        _cutsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        _cutsTable->verticalHeader()->setVisible(false);
        _cutsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        _cutsTable->setSelectionMode(QAbstractItemView::SingleSelection);
        v->addWidget(_cutsTable);

        auto* btns = new QHBoxLayout;
        _addCutBtn    = new QPushButton(tr("Add cut"));
        _removeCutBtn = new QPushButton(tr("Remove"));
        btns->addWidget(_addCutBtn);
        btns->addWidget(_removeCutBtn);
        btns->addStretch();
        v->addLayout(btns);
        root->addWidget(box);

        connect(_addCutBtn,    &QPushButton::clicked, this, &ImportLightcurveDialog::onAddCut);
        connect(_removeCutBtn, &QPushButton::clicked, this, &ImportLightcurveDialog::onRemoveCut);
    }

    // ── Preview ──
    {
        auto* box = new QGroupBox(tr("Preview (first lines)"));
        auto* v   = new QVBoxLayout(box);
        _previewView = new QPlainTextEdit;
        _previewView->setReadOnly(true);
        _previewView->setMaximumHeight(140);
        _previewView->setStyleSheet("font-family: monospace;");
        v->addWidget(_previewView);
        root->addWidget(box);
    }

    // ── Status + action buttons ──
    _statusLabel = new QLabel;
    _statusLabel->setStyleSheet("color: gray;");
    _statusLabel->setWordWrap(true);
    root->addWidget(_statusLabel);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Cancel);
    _importBtn = bb->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    root->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_importBtn, &QPushButton::clicked, this, &ImportLightcurveDialog::onImport);
}

// ─────────────────────────────────────────────────────────────────────
void ImportLightcurveDialog::populateInstruments()
{
    _instrumentCombo->clear();
    if (!_dbm) return;

    auto all = _dbm->getAllInstruments();
    std::sort(all.begin(), all.end(),
        [](const auto& a, const auto& b){ return a->getName() < b->getName(); });

    for (const auto& inst : all) {
        if (inst->photometricModes().isEmpty()) continue;
        _instrumentCombo->addItem(inst->getName(), inst->getId());
    }
    if (_instrumentCombo->count() > 0) {
        _instrumentCombo->setCurrentIndex(0);
        onInstrumentChanged(0);
    } else {
        _statusLabel->setStyleSheet("color: #c46060;");
        _statusLabel->setText(tr("No instruments with a photometric mode are "
                                 "registered in ASTRA."));
    }
}

void ImportLightcurveDialog::onInstrumentChanged(int)
{
    populateModes();
}

void ImportLightcurveDialog::populateModes()
{
    _modeCombo->clear();
    if (!_dbm) return;
    const QString instId = _instrumentCombo->currentData().toString();
    auto inst = _dbm->getInstrumentById(instId);
    if (!inst) return;

    QString defaultFilter;
    for (const auto* m : inst->photometricModes()) {
        _modeCombo->addItem(m->displayName(), m->key());
        if (defaultFilter.isEmpty()
            && m->hasPhotometricProperties()
            && !m->photometric().filters.isEmpty()) {
            defaultFilter = m->photometric().filters.first();
        }
    }
    if (_defaultFilter->text().isEmpty())
        _defaultFilter->setText(defaultFilter.isEmpty()
                                ? inst->getName() : defaultFilter);
}

// ─────────────────────────────────────────────────────────────────────
void ImportLightcurveDialog::onBrowse()
{
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Choose a CSV lightcurve file"),
        QString(),
        tr("Lightcurve files (*.csv *.txt *.tsv *.dat);;All files (*)"));
    if (fn.isEmpty()) return;
    _filePath->setText(fn);
    reloadFile();
}

void ImportLightcurveDialog::reloadFile()
{
    _headers.clear();
    _dataRows.clear();
    _hasHeader = false;

    const QString path = _filePath->text().trimmed();
    if (path.isEmpty()) { rebuildColumnCombos(); return; }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        _statusLabel->setStyleSheet("color: #c46060;");
        _statusLabel->setText(tr("Could not open file: %1").arg(path));
        rebuildColumnCombos();
        return;
    }

    QTextStream in(&f);
    QStringList rawLines;
    while (!in.atEnd()) rawLines << in.readLine();

    // Strip comments / empties.
    QStringList lines;
    lines.reserve(rawLines.size());
    for (const QString& l : rawLines) {
        const QString t = l.trimmed();
        if (t.isEmpty()) continue;
        if (t.startsWith('#') || t.startsWith("//")) continue;
        lines << t;
    }
    if (lines.isEmpty()) {
        _statusLabel->setStyleSheet("color: #c46060;");
        _statusLabel->setText(tr("File appears to be empty."));
        rebuildColumnCombos();
        return;
    }

    _delim = guessDelimiter(lines.first());
    QStringList firstTokens = splitRow(lines.first(), _delim);
    _hasHeader = rowLooksLikeHeader(firstTokens);

    if (_hasHeader) {
        _headers = firstTokens;
        for (int i = 1; i < lines.size(); ++i)
            _dataRows << splitRow(lines[i], _delim);
    } else {
        const int n = firstTokens.size();
        _headers.clear();
        for (int i = 0; i < n; ++i) _headers << QString("col%1").arg(i);
        for (const QString& l : lines)
            _dataRows << splitRow(l, _delim);
    }

    // Preview
    QString prev;
    if (_hasHeader) prev += _headers.join(QString(_delim)) + "\n";
    for (int i = 0; i < std::min<int>(8, _dataRows.size()); ++i)
        prev += _dataRows[i].join(QString(_delim)) + "\n";
    _previewView->setPlainText(prev);

    _statusLabel->setStyleSheet("color: gray;");
    _statusLabel->setText(tr("Loaded %1 data row(s) from \"%2\" "
                             "(delimiter '%3'%4).")
                          .arg(_dataRows.size())
                          .arg(QFileInfo(path).fileName())
                          .arg(_delim == '\t' ? QString("\\t") : QString(_delim))
                          .arg(_hasHeader ? tr(", header detected")
                                          : tr(", no header detected")));
    rebuildColumnCombos();
}

void ImportLightcurveDialog::rebuildColumnCombos()
{
    auto fill = [this](QComboBox* c, bool allowNone) {
        const QString prev = c->currentText();
        c->clear();
        if (allowNone) c->addItem(tr("(none)"), -1);
        for (int i = 0; i < _headers.size(); ++i)
            c->addItem(_headers[i], i);
        const int idx = c->findText(prev);
        if (idx >= 0) c->setCurrentIndex(idx);
    };

    fill(_timeColCombo,    false);
    fill(_fluxColCombo,    false);
    fill(_fluxErrColCombo, true);
    fill(_filterColCombo,  true);

    // Best-effort autodetection by common names.
    auto findCol = [this](const QStringList& candidates) -> int {
        for (int i = 0; i < _headers.size(); ++i) {
            const QString lc = _headers[i].toLower();
            for (const QString& cand : candidates)
                if (lc.contains(cand)) return i;
        }
        return -1;
    };

    if (!_headers.isEmpty()) {
        const int tIdx = findCol({"bjd", "mjd", "hjd", "jd", "time", "btjd"});
        const int fIdx = findCol({"flux", "fnu", "mag"});
        const int eIdx = findCol({"err", "sigma", "uncertainty"});
        const int filtIdx = findCol({"filter", "band", "passband"});

        if (tIdx >= 0) _timeColCombo->setCurrentIndex(tIdx);
        if (fIdx >= 0) _fluxColCombo->setCurrentIndex(fIdx);
        if (eIdx >= 0) _fluxErrColCombo->setCurrentIndex(eIdx + 1); // +1 for "(none)"
        if (filtIdx >= 0) _filterColCombo->setCurrentIndex(filtIdx + 1);

        // Best guess for time scale from header text
        for (int i = 0; i < _headers.size(); ++i) {
            const QString lc = _headers[i].toLower();
            for (int k = 0; k < kTimeScales.size(); ++k) {
                const QString tag = QString(kTimeScales[k].label).section(' ', 0, 0).toLower();
                if (lc.contains(tag)) {
                    _timeScaleCombo->setCurrentIndex(k);
                    break;
                }
            }
        }
    }

    updateCutColumnCombos();
}

// ─────────────────────────────────────────────────────────────────────
void ImportLightcurveDialog::onAddCut()
{
    if (_headers.isEmpty()) {
        QMessageBox::information(this, tr("Add cut"),
            tr("Load a CSV file first."));
        return;
    }

    const int row = _cutsTable->rowCount();
    _cutsTable->insertRow(row);

    auto* colCb = new QComboBox;
    for (int i = 0; i < _headers.size(); ++i)
        colCb->addItem(_headers[i], i);
    _cutsTable->setCellWidget(row, 0, colCb);

    auto* opCb = new QComboBox;
    for (const auto& op : kCutOps)
        opCb->addItem(op.label, op.tag);
    _cutsTable->setCellWidget(row, 1, opCb);

    auto* spin = new QDoubleSpinBox;
    spin->setDecimals(6);
    spin->setRange(-1e15, 1e15);
    spin->setValue(0.0);
    _cutsTable->setCellWidget(row, 2, spin);
}

void ImportLightcurveDialog::onRemoveCut()
{
    const int row = _cutsTable->currentRow();
    if (row < 0) return;
    _cutsTable->removeRow(row);
}

void ImportLightcurveDialog::updateCutColumnCombos()
{
    // When headers change, refresh each existing cut row's column combo.
    for (int r = 0; r < _cutsTable->rowCount(); ++r) {
        auto* cb = qobject_cast<QComboBox*>(_cutsTable->cellWidget(r, 0));
        if (!cb) continue;
        const QString prev = cb->currentText();
        cb->clear();
        for (int i = 0; i < _headers.size(); ++i)
            cb->addItem(_headers[i], i);
        const int idx = cb->findText(prev);
        if (idx >= 0) cb->setCurrentIndex(idx);
    }
}

// ─────────────────────────────────────────────────────────────────────
void ImportLightcurveDialog::onImport()
{
    _imported = false;
    _points.clear();

    if (_dataRows.isEmpty()) {
        QMessageBox::warning(this, tr("Import"),
            tr("No data rows loaded. Choose a CSV file first."));
        return;
    }
    if (_instrumentCombo->count() == 0) {
        QMessageBox::warning(this, tr("Import"),
            tr("No photometric instrument selected."));
        return;
    }

    const QString instId = _instrumentCombo->currentData().toString();
    auto inst = _dbm ? _dbm->getInstrumentById(instId) : nullptr;
    if (!inst) {
        QMessageBox::warning(this, tr("Import"),
            tr("Could not resolve selected instrument."));
        return;
    }
    const QString instName = inst->getName();
    _sourceKey = instName;
    _timeScale = static_cast<TimeScale>(_timeScaleCombo->currentData().toInt());

    const int tCol = _timeColCombo->currentData().toInt();
    const int fCol = _fluxColCombo->currentData().toInt();
    const int eCol = _fluxErrColCombo->currentData().toInt();
    const int filtCol = _filterColCombo->currentData().toInt();

    if (tCol < 0 || fCol < 0) {
        QMessageBox::warning(this, tr("Import"),
            tr("Please pick at least the time and flux columns."));
        return;
    }

    // Snapshot cuts.
    struct Cut { int col; QString tag; double rhs; };
    QList<Cut> cuts;
    for (int r = 0; r < _cutsTable->rowCount(); ++r) {
        auto* cb  = qobject_cast<QComboBox*>(_cutsTable->cellWidget(r, 0));
        auto* op  = qobject_cast<QComboBox*>(_cutsTable->cellWidget(r, 1));
        auto* spn = qobject_cast<QDoubleSpinBox*>(_cutsTable->cellWidget(r, 2));
        if (!cb || !op || !spn) continue;
        cuts.append({ cb->currentData().toInt(), op->currentData().toString(), spn->value() });
    }

    const QString defaultFilter = _defaultFilter->text().trimmed();

    int dropped = 0, badNum = 0;
    std::vector<LightcurvePoint> pts;
    pts.reserve(_dataRows.size());

    for (const QStringList& row : _dataRows) {
        if (row.size() <= tCol || row.size() <= fCol) { ++badNum; continue; }

        // Quality cuts first
        bool keep = true;
        for (const Cut& c : cuts) {
            if (c.col < 0 || c.col >= row.size()) { keep = false; break; }
            bool ok = false;
            const double v = row[c.col].trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(v)) { keep = false; break; }
            if (!evalCutOp(c.tag, v, c.rhs))    { keep = false; break; }
        }
        if (!keep) { ++dropped; continue; }

        bool tOk = false, fOk = false;
        const double t = row[tCol].trimmed().toDouble(&tOk);
        const double y = row[fCol].trimmed().toDouble(&fOk);
        if (!tOk || !fOk || !std::isfinite(t) || !std::isfinite(y)) {
            ++badNum;
            continue;
        }

        double e = 0.0;
        if (eCol >= 0 && eCol < row.size()) {
            bool eOk = false;
            const double ev = row[eCol].trimmed().toDouble(&eOk);
            if (eOk && std::isfinite(ev)) e = ev;
        }

        QString flt;
        if (filtCol >= 0 && filtCol < row.size())
            flt = row[filtCol].trimmed();
        if (flt.isEmpty()) flt = defaultFilter;

        LightcurvePoint pt;
        pt.time      = Time(t, _timeScale);
        pt.flux      = y;
        pt.fluxError = e;
        pt.filter    = flt;
        pts.push_back(std::move(pt));
    }

    if (pts.empty()) {
        QMessageBox::warning(this, tr("Import"),
            tr("No valid rows after applying cuts.\n"
               "Dropped by cuts: %1\nParse failures: %2")
            .arg(dropped).arg(badNum));
        return;
    }

    // ── BJD computation (mirrors the fetch path) ────────────────────────
    const bool haveCoords =
        Star::isSet(_star->getRa()) && Star::isSet(_star->getDec());
    const bool nativeIsBjd =
        (_timeScale == TimeScale::BJD  ||
         _timeScale == TimeScale::BTJD ||
         _timeScale == TimeScale::BKJD ||
         _timeScale == TimeScale::GaiaTCB);

    int converted = 0;
    if (!nativeIsBjd && haveCoords) {
        for (auto& pt : pts) {
            if (pt.time.hasBjd()) continue;
            pt.time.setAutoConvertInfo(inst, _star->getRa(), _star->getDec());
            if (pt.time.bjd().has_value()) ++converted;
        }
    }

    // ── Attach to the star and persist ─────────────────────────────────
    auto phot = _star->getPhotometry();
    if (!phot) {
        phot = std::make_shared<Photometry>();
        _star->setPhotometry(phot);
    }

    auto result = phot->mergeLightcurve(_sourceKey, pts);
    QString verb;
    switch (result) {
        case Photometry::MergeResult::Identical: verb = tr("identical to existing"); break;
        case Photometry::MergeResult::Replaced:  verb = tr("replaced existing");     break;
        case Photometry::MergeResult::Merged:    verb = tr("merged into existing"); break;
        case Photometry::MergeResult::Added:     verb = tr("added as new");          break;
    }

    if (_dbm && !_dbm->saveLightcurveForStar(_star->getId(), _sourceKey, phot.get())) {
        QMessageBox::warning(this, tr("Import"),
            tr("Imported points in memory but FAILED to save them to the database."));
    }

    _points   = std::move(pts);
    _imported = true;

    LOG_INFO("ImportLC",
        QString("Imported %1 CSV points for star %2 under source \"%3\" "
                "(cuts dropped %4, parse failures %5, BJD computed for %6)")
            .arg(_points.size())
            .arg(_star->getId())
            .arg(_sourceKey)
            .arg(dropped)
            .arg(badNum)
            .arg(converted));

    _statusLabel->setStyleSheet("color: #7dbd5e;");
    _statusLabel->setText(tr("Imported %1 points (%2). Dropped: %3 by cuts, "
                             "%4 parse failures.")
                          .arg(_points.size()).arg(verb).arg(dropped).arg(badNum));

    accept();
}