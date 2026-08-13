#include "AddSpectraDialog.h"

#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "models/Spectrum.h"
#include "utils/Logger.h"
#include "utils/WheelGuard.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Column {
    ColFile = 0,
    ColInstrument,
    ColMode,
    ColScale,
    ColTime,
    ColSource,
    ColumnCount
};

// Only these survive a round-trip through the database, which stores a
// spectrum's time as MJD and BJD.
struct ScaleChoice { const char* label; TimeScale scale; };
const ScaleChoice kScaleChoices[] = {
    { "MJD", TimeScale::MJD },
    { "JD",  TimeScale::JD  },
    { "BJD", TimeScale::BJD },
};

/// First number in `path`, skipping comment lines and non-numeric tokens, so
/// that both a bare "58123.4567" and "MJD = 58123.4567" are understood.
std::optional<double> firstNumberIn(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;

        static const QRegularExpression sep(QStringLiteral("[\\s,;=]+"));
        const QStringList tokens = line.split(sep, Qt::SkipEmptyParts);
        for (const QString& tok : tokens) {
            bool ok = false;
            const double v = tok.toDouble(&ok);
            if (ok) return v;
        }
    }
    return std::nullopt;
}

QString scaleLabel(TimeScale s)
{
    for (const auto& c : kScaleChoices)
        if (c.scale == s) return QString::fromLatin1(c.label);
    return Time::scaleToString(s);
}

struct SidecarToken { const char* token; TimeScale scale; };
const SidecarToken kSidecarTokens[] = {
    { "mjd", TimeScale::MJD },
    { "bjd", TimeScale::BJD },
    { "hjd", TimeScale::HJD },
    { "jd",  TimeScale::JD  },
};

/// Base names a sidecar may be built on, longest first. Sidecars are commonly
/// written once per exposure while the spectra are split per order or per
/// extraction, so `0248_44_gaia_33773_930_01.txt` belongs to
/// `0248_44_gaia_33773_930_mjd.txt`. Only purely numeric trailing segments are
/// peeled off, and at most two of them, so unrelated files stay unrelated.
QStringList candidateStems(const QString& base)
{
    QStringList stems { base };

    QString stem = base;
    for (int i = 0; i < 2; ++i) {
        const int cut = stem.lastIndexOf('_');
        if (cut <= 0) break;

        const QString tail = stem.mid(cut + 1);
        const bool numeric = !tail.isEmpty() &&
            std::all_of(tail.cbegin(), tail.cend(),
                        [](QChar c) { return c.isDigit(); });
        if (!numeric) break;

        stem.truncate(cut);
        stems << stem;
    }
    return stems;
}

} // namespace

// =====================================================================
// Sidecar detection
// =====================================================================

bool AddSpectraDialog::isTimeSidecar(const QString& path)
{
    const QFileInfo info(path);
    const QString   suffix = info.suffix().toLower();
    const QString   base   = info.completeBaseName().toLower();

    for (const auto& cand : kSidecarTokens) {
        const QString token = QString::fromLatin1(cand.token);
        if (suffix == token) return true;
        if (base == token || base.endsWith('_' + token)) return true;
    }
    return false;
}

std::optional<AddSpectraDialog::DetectedTime>
AddSpectraDialog::detectSidecarTime(const QString& spectrumPath,
                                    const QStringList& pool)
{
    const QFileInfo info(spectrumPath);
    const QDir      dir  = info.absoluteDir();
    const QString   base = info.completeBaseName();
    if (base.isEmpty()) return std::nullopt;

    // Case-insensitive lookup tables, so `_MJD.TXT` is found on case-sensitive
    // file systems too. Sidecars the user picked explicitly are matched first
    // and may sit in a different directory than the spectrum; only if none of
    // them fits does the spectrum's own directory get searched.
    QHash<QString, QString> picked;
    picked.reserve(pool.size());
    for (const QString& p : pool) {
        const QFileInfo pi(p);
        const QString   key = pi.fileName().toLower();
        // Should the same name have been picked in two directories, the one
        // next to the spectrum wins.
        if (picked.contains(key) && pi.absolutePath() != dir.absolutePath())
            continue;
        picked.insert(key, p);
    }

    QHash<QString, QString> siblings;
    if (dir.exists()) {
        const auto names = dir.entryList(QDir::Files);
        siblings.reserve(names.size());
        for (const QString& n : names)
            siblings.insert(n.toLower(), dir.filePath(n));
    }

    for (const QString& stem : candidateStems(base)) {
        for (const auto& cand : kSidecarTokens) {
            const QString token = QString::fromLatin1(cand.token);
            const QStringList names = {
                stem + '_' + token + QStringLiteral(".txt"),
                stem + '_' + token + QStringLiteral(".dat"),
                stem + '_' + token,
                stem + '.' + token,
            };

            for (const QString& name : names) {
                const QString key = name.toLower();
                for (const auto* table : { &picked, &siblings }) {
                    const auto it = table->constFind(key);
                    if (it == table->constEnd()) continue;

                    const QString path  = *it;
                    const auto    value = firstNumberIn(path);
                    if (!value.has_value()) continue;

                    const QString actual = QFileInfo(path).fileName();
                    LOG_DEBUG("Tools",
                        QString("Found time sidecar %1 for %2 (%3 = %4)")
                            .arg(actual, info.fileName(),
                                 Time::scaleToString(cand.scale))
                            .arg(*value, 0, 'f', 6));
                    return DetectedTime{ *value, cand.scale, actual, path };
                }
            }
        }
    }
    return std::nullopt;
}

// =====================================================================
// Construction
// =====================================================================

AddSpectraDialog::AddSpectraDialog(
    std::vector<Entry> entries,
    std::vector<std::shared_ptr<Instrument>> instruments,
    QStringList sidecarPool,
    QWidget* parent)
    : QDialog(parent)
    , _entries(std::move(entries))
    , _instruments(std::move(instruments))
    , _sidecarPool(std::move(sidecarPool))
{
    setWindowTitle(_entries.size() > 1
        ? QString("Add %1 Spectra").arg(_entries.size())
        : QStringLiteral("Add Spectrum"));
    setupUi();
}

void AddSpectraDialog::setupUi()
{
    auto* v = new QVBoxLayout(this);

    _table = new QTableWidget(static_cast<int>(_entries.size()),
                             ColumnCount, this);
    _table->setHorizontalHeaderLabels(
        { "File", "Instrument", "Mode", "Scale", "Time", "Time from" });
    _table->verticalHeader()->setVisible(false);
    _table->setSelectionMode(QAbstractItemView::NoSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(_table, 1);

    for (int row = 0; row < static_cast<int>(_entries.size()); ++row)
        fillRow(row, _entries[row]);

    _table->resizeColumnsToContents();
    // resizeColumnsToContents() sizes to the items, not to the combo boxes
    // sitting in the cells, so the widest editor in each column sets the width.
    for (int col : { ColInstrument, ColMode, ColScale, ColTime }) {
        int width = _table->columnWidth(col);
        for (int row = 0; row < _table->rowCount(); ++row)
            if (auto* w = _table->cellWidget(row, col))
                width = std::max(width, w->sizeHint().width());
        _table->setColumnWidth(col, width + 12);
    }
    _table->horizontalHeader()->setSectionResizeMode(ColFile,
                                                     QHeaderView::Stretch);

    // Cell widgets are laid out inside the cell rect, but a combo box refuses
    // to shrink below its minimum height and then spills over the row below.
    // Give every row the height of the tallest editor in the table.
    int rowHeight = _table->verticalHeader()->defaultSectionSize();
    for (int row = 0; row < _table->rowCount(); ++row)
        for (int col = 0; col < ColumnCount; ++col)
            if (auto* w = _table->cellWidget(row, col))
                rowHeight = std::max(rowHeight, w->sizeHint().height());
    rowHeight += 6;   // a little air above and below the editors

    _table->verticalHeader()->setDefaultSectionSize(rowHeight);
    for (int row = 0; row < _table->rowCount(); ++row)
        _table->setRowHeight(row, rowHeight);

    auto* buttons = new QHBoxLayout;
    if (_entries.size() > 1) {
        auto* copyBtn = new QPushButton(
            QStringLiteral("Apply first row's instrument/scale to all"));
        connect(copyBtn, &QPushButton::clicked,
                this, &AddSpectraDialog::copyFirstRowToAll);
        buttons->addWidget(copyBtn);
    }
    buttons->addStretch();
    v->addLayout(buttons);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                    QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add"));
    connect(bb, &QDialogButtonBox::accepted, this, &AddSpectraDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &AddSpectraDialog::reject);
    v->addWidget(bb);

    resize(920, 380);
}

void AddSpectraDialog::fillRow(int row, const Entry& entry)
{
    const QFileInfo info(entry.path);
    const auto&     spec = entry.spectrum;

    auto* fileItem = new QTableWidgetItem(info.fileName());
    fileItem->setToolTip(entry.path);
    _table->setItem(row, ColFile, fileItem);

    // ── instrument / mode ───────────────────────────────────────────────
    auto* instCombo = new QComboBox;
    instCombo->addItem(QStringLiteral("(none)"), QString());
    for (const auto& inst : _instruments)
        instCombo->addItem(inst->getName(), inst->getId());
    astra::blockWheelScrolling(instCombo);
    _table->setCellWidget(row, ColInstrument, instCombo);

    auto* modeCombo = new QComboBox;
    modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    astra::blockWheelScrolling(modeCombo);
    _table->setCellWidget(row, ColMode, modeCombo);

    connect(instCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, row, instCombo](int) {
        populateModes(row, instCombo->currentData().toString(), QString());
    });

    // The caller has already run auto-detection, so whatever the spectrum
    // carries is the detected match.
    const int instIdx = instCombo->findData(spec->getInstrumentId());
    if (!spec->getInstrumentId().isEmpty() && instIdx >= 0)
        instCombo->setCurrentIndex(instIdx);
    populateModes(row, instCombo->currentData().toString(), spec->getModeKey());

    // ── time ────────────────────────────────────────────────────────────
    auto* scaleCombo = new QComboBox;
    for (const auto& c : kScaleChoices)
        scaleCombo->addItem(QString::fromLatin1(c.label),
                            static_cast<int>(c.scale));
    scaleCombo->setToolTip(
        "Scale of the value on the left. JD is stored as MJD.");
    astra::blockWheelScrolling(scaleCombo);
    _table->setCellWidget(row, ColScale, scaleCombo);

    auto* timeEdit = new QLineEdit;
    timeEdit->setPlaceholderText(QStringLiteral("not detected"));
    _table->setCellWidget(row, ColTime, timeEdit);

    QString source = QStringLiteral("not found");
    QString sourceTip;
    TimeScale scale = TimeScale::MJD;
    std::optional<double> value;

    const Time& t = spec->time();
    if (t.isValid() && t.nativeValue() != 0.0) {
        // Came out of the file itself (FITS header, mostly).
        scale  = t.nativeScale();
        value  = t.nativeValue();
        source = QStringLiteral("file header");
        // Scales the DB cannot hold are folded onto the ones it can.
        if (scale != TimeScale::MJD && scale != TimeScale::JD &&
            scale != TimeScale::BJD)
        {
            if (auto mjd = t.mjd())  { scale = TimeScale::MJD; value = *mjd; }
            else if (t.hasBjd())     { scale = TimeScale::BJD; value = t.bjdOr(0.0); }
        }
    }
    else if (const auto detected = detectSidecarTime(entry.path, _sidecarPool)) {
        scale     = detected->scale;
        value     = detected->value;
        source    = detected->sourceFile;
        sourceTip = detected->sourcePath;
    }

    const int scaleIdx = scaleCombo->findText(scaleLabel(scale));
    if (scaleIdx >= 0) {
        scaleCombo->setCurrentIndex(scaleIdx);
    } else if (value.has_value()) {
        // A scale a spectrum cannot be stored in (HJD, say). Say so instead of
        // quietly relabelling the number as an MJD.
        source += QString(" (%1 cannot be stored, pick a scale)")
                      .arg(Time::scaleToString(scale));
        sourceTip.clear();
    }
    if (value.has_value())
        timeEdit->setText(QString::number(*value, 'f', 6));

    auto* sourceItem = new QTableWidgetItem(source);
    sourceItem->setToolTip(sourceTip.isEmpty() ? source : sourceTip);
    _table->setItem(row, ColSource, sourceItem);
}

void AddSpectraDialog::populateModes(int row, const QString& instrumentId,
                                      const QString& preselectKey)
{
    auto* modeCombo = qobject_cast<QComboBox*>(_table->cellWidget(row, ColMode));
    if (!modeCombo) return;

    modeCombo->clear();
    modeCombo->addItem(QStringLiteral("(none)"), QString());

    for (const auto& inst : _instruments) {
        if (inst->getId() != instrumentId) continue;
        for (const InstrumentMode& m : inst->modes()) {
            if (m.dataType() != InstrumentMode::Spectroscopy) continue;
            modeCombo->addItem(m.displayName(), m.key());
        }
        break;
    }

    modeCombo->setEnabled(modeCombo->count() > 1);
    if (!preselectKey.isEmpty()) {
        const int i = modeCombo->findData(preselectKey);
        if (i >= 0) modeCombo->setCurrentIndex(i);
    }
}

void AddSpectraDialog::copyFirstRowToAll()
{
    if (_table->rowCount() < 2) return;

    auto* firstInst  = qobject_cast<QComboBox*>(_table->cellWidget(0, ColInstrument));
    auto* firstMode  = qobject_cast<QComboBox*>(_table->cellWidget(0, ColMode));
    auto* firstScale = qobject_cast<QComboBox*>(_table->cellWidget(0, ColScale));
    if (!firstInst || !firstMode || !firstScale) return;

    const QString instId  = firstInst->currentData().toString();
    const QString modeKey = firstMode->currentData().toString();
    const int     scale   = firstScale->currentIndex();

    for (int row = 1; row < _table->rowCount(); ++row) {
        if (auto* c = qobject_cast<QComboBox*>(_table->cellWidget(row, ColInstrument))) {
            const int i = c->findData(instId);
            if (i >= 0) c->setCurrentIndex(i);        // repopulates the modes
        }
        if (auto* c = qobject_cast<QComboBox*>(_table->cellWidget(row, ColMode))) {
            const int i = c->findData(modeKey);
            c->setCurrentIndex(i >= 0 ? i : 0);
        }
        if (auto* c = qobject_cast<QComboBox*>(_table->cellWidget(row, ColScale)))
            c->setCurrentIndex(scale);
    }
}

// =====================================================================
// Result
// =====================================================================

bool AddSpectraDialog::applyChoices()
{
    QStringList concerns;

    for (int row = 0; row < static_cast<int>(_entries.size()); ++row) {
        auto& spec = _entries[row].spectrum;

        auto* instCombo  = qobject_cast<QComboBox*>(_table->cellWidget(row, ColInstrument));
        auto* modeCombo  = qobject_cast<QComboBox*>(_table->cellWidget(row, ColMode));
        auto* scaleCombo = qobject_cast<QComboBox*>(_table->cellWidget(row, ColScale));
        auto* timeEdit   = qobject_cast<QLineEdit*>(_table->cellWidget(row, ColTime));
        if (!instCombo || !modeCombo || !scaleCombo || !timeEdit) continue;

        const QString instId  = instCombo->currentData().toString();
        const QString modeKey = modeCombo->currentData().toString();

        if (instId.isEmpty()) {
            spec->setInstrumentId(QString());
            spec->setModeKey(QString());
        } else {
            QString display = instCombo->currentText();
            if (!modeKey.isEmpty())
                display += QString(" (%1)").arg(modeCombo->currentText());
            spec->setInstrument(display);
            spec->setInstrumentId(instId);
            spec->setModeKey(modeKey);
        }

        const QString name = _table->item(row, ColFile)->text();

        const QString text = timeEdit->text().trimmed();
        if (text.isEmpty()) {
            concerns << QString("%1 - no observation time (radial velocities "
                                "and periodograms need one)").arg(name);
            continue;
        }

        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!ok) {
            QMessageBox::warning(this, "Add Spectra",
                QString("'%1' is not a number (row %2, %3).")
                    .arg(text)
                    .arg(row + 1)
                    .arg(name));
            timeEdit->setFocus();
            timeEdit->selectAll();
            return false;
        }

        const auto scale =
            static_cast<TimeScale>(scaleCombo->currentData().toInt());

        // An MJD has ~5 digits and a (B)JD ~2.4 million, so a value on the
        // wrong side of that is the classic scale mix-up.
        const bool looksLikeJd = value > 1.0e6;
        if ((scale == TimeScale::MJD) == looksLikeJd) {
            concerns << QString("%1 - %2 does not look like a %3")
                            .arg(name, text, scaleCombo->currentText());
        }

        Time t(value, scale);
        if (spec->time().hasExposureTime())
            t.setExposureTime(spec->time().exposureTimeSec());
        spec->setTime(t);
    }

    if (!concerns.isEmpty()) {
        const auto answer = QMessageBox::question(this, "Add Spectra",
            QString("Check these before adding:\n• %1\n\nAdd anyway?")
                .arg(concerns.join("\n• ")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return false;
    }

    return true;
}

void AddSpectraDialog::accept()
{
    if (!applyChoices()) return;
    QDialog::accept();
}
