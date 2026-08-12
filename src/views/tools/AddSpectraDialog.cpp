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
#include <QLabel>
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

} // namespace

// =====================================================================
// Sidecar detection
// =====================================================================

std::optional<AddSpectraDialog::DetectedTime>
AddSpectraDialog::detectSidecarTime(const QString& spectrumPath)
{
    const QFileInfo info(spectrumPath);
    const QDir      dir  = info.absoluteDir();
    const QString   base = info.completeBaseName();
    if (base.isEmpty() || !dir.exists()) return std::nullopt;

    // Case-insensitive lookup table of the directory, so `_MJD.TXT` is found on
    // case-sensitive file systems too.
    QHash<QString, QString> byLowerName;
    const auto siblings = dir.entryList(QDir::Files);
    byLowerName.reserve(siblings.size());
    for (const QString& n : siblings)
        byLowerName.insert(n.toLower(), n);

    struct Candidate { const char* token; TimeScale scale; };
    static const Candidate kCandidates[] = {
        { "mjd", TimeScale::MJD },
        { "bjd", TimeScale::BJD },
        { "hjd", TimeScale::HJD },
        { "jd",  TimeScale::JD  },
    };

    for (const auto& cand : kCandidates) {
        const QString token = QString::fromLatin1(cand.token);
        const QStringList names = {
            base + '_' + token + QStringLiteral(".txt"),
            base + '_' + token + QStringLiteral(".dat"),
            base + '_' + token,
            base + '.' + token,
        };

        for (const QString& name : names) {
            const auto it = byLowerName.constFind(name.toLower());
            if (it == byLowerName.constEnd()) continue;

            const QString actual = *it;
            const auto value = firstNumberIn(dir.filePath(actual));
            if (!value.has_value()) continue;

            LOG_DEBUG("Tools", QString("Found time sidecar %1 for %2 (%3 = %4)")
                          .arg(actual, info.fileName(),
                               Time::scaleToString(cand.scale))
                          .arg(*value, 0, 'f', 6));
            return DetectedTime{ *value, cand.scale, actual };
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
    QWidget* parent)
    : QDialog(parent)
    , _entries(std::move(entries))
    , _instruments(std::move(instruments))
{
    setWindowTitle(_entries.size() > 1
        ? QString("Add %1 Spectra").arg(_entries.size())
        : QStringLiteral("Add Spectrum"));
    setupUi();
}

void AddSpectraDialog::setupUi()
{
    auto* v = new QVBoxLayout(this);

    auto* hint = new QLabel(
        "Check the instrument and the observation time of every file. Times "
        "found in a FITS header or in a sidecar file (e.g. <name>_mjd.txt) are "
        "pre-filled; leave the time empty to add the spectrum without one.");
    hint->setWordWrap(true);
    v->addWidget(hint);

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

    QString source = QStringLiteral("—");
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
    else if (const auto detected = detectSidecarTime(entry.path)) {
        scale  = detected->scale;
        value  = detected->value;
        source = detected->sourceFile;
    }

    const int scaleIdx = scaleCombo->findText(scaleLabel(scale));
    if (scaleIdx >= 0) {
        scaleCombo->setCurrentIndex(scaleIdx);
    } else if (value.has_value()) {
        // A scale a spectrum cannot be stored in (HJD, say). Say so instead of
        // quietly relabelling the number as an MJD.
        source += QString(" — %1 cannot be stored, pick a scale")
                      .arg(Time::scaleToString(scale));
    }
    if (value.has_value())
        timeEdit->setText(QString::number(*value, 'f', 6));

    auto* sourceItem = new QTableWidgetItem(source);
    sourceItem->setToolTip(source);
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
