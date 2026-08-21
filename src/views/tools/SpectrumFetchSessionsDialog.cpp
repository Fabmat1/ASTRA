#include "SpectrumFetchSessionsDialog.h"

#include "models/Star.h"
#include "utils/AppSettings.h"
#include "utils/spectrafetch/EsoArchiveClient.h"
#include "utils/spectrafetch/LamostArchiveClient.h"
#include "utils/spectrafetch/MastArchiveClient.h"
#include "utils/spectrafetch/SdssOpticalArchiveClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// ═════════════════════════════════════════════════════════════════════════════
//  SpectrumFetchSessionsDialog
// ═════════════════════════════════════════════════════════════════════════════

namespace {
QString stateGlyph(SpectrumFetchService::State s) {
    using State = SpectrumFetchService::State;
    switch (s) {
    case State::Discovering:       return QStringLiteral("?");
    case State::AwaitingSelection: return QStringLiteral("!");
    case State::Downloading:       return QStringLiteral(">");
    case State::Finished:          return QStringLiteral("+");
    case State::Failed:            return QStringLiteral("x");
    case State::Cancelled:         return QStringLiteral("-");
    }
    return QStringLiteral("?");
}

QString formatEta(qint64 ms) {
    if (ms < 0) return QString();
    const qint64 s = ms / 1000;
    if (s < 90) return QStringLiteral("~%1 s remaining").arg(std::max<qint64>(s, 1));
    if (s < 5400)
        return QStringLiteral("~%1 min remaining").arg((s + 30) / 60);
    return QStringLiteral("~%1 h remaining").arg((s + 1800) / 3600);
}
}   // namespace

SpectrumFetchSessionsDialog::SpectrumFetchSessionsDialog(
    SpectrumFetchService* service, QWidget* parent)
    : QDialog(parent), _service(service) {
    setWindowTitle(tr("Spectrum Fetch Sessions"));
    resize(860, 520);

    auto* root = new QVBoxLayout(this);

    auto* topRow = new QHBoxLayout;
    auto* newBtn = new QPushButton(tr("New Fetch..."));
    connect(newBtn, &QPushButton::clicked, this,
            &SpectrumFetchSessionsDialog::newFetchRequested);
    topRow->addWidget(newBtn);
    _summary = new QLabel;
    topRow->addWidget(_summary, 1);
    root->addLayout(topRow);

    auto* split = new QSplitter(Qt::Horizontal);
    _list = new QListWidget;
    _list->setMinimumWidth(260);
    split->addWidget(_list);

    _log = new QPlainTextEdit;
    _log->setReadOnly(true);
    _log->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->addWidget(_log);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    auto* btnRow = new QHBoxLayout;
    _reviewBtn = new QPushButton(tr("Review && Download..."));
    _reviewBtn->setEnabled(false);
    btnRow->addWidget(_reviewBtn);
    btnRow->addStretch(1);
    _cancelBtn    = new QPushButton(tr("Cancel Selected"));
    _cancelAllBtn = new QPushButton(tr("Cancel All"));
    auto* closeBtn = new QPushButton(tr("Close"));
    btnRow->addWidget(_cancelBtn);
    btnRow->addWidget(_cancelAllBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(_reviewBtn, &QPushButton::clicked, this,
            &SpectrumFetchSessionsDialog::onReviewSelected);
    connect(_cancelBtn, &QPushButton::clicked, this,
            &SpectrumFetchSessionsDialog::onCancelSelected);
    connect(_cancelAllBtn, &QPushButton::clicked, this,
            &SpectrumFetchSessionsDialog::onCancelAll);
    connect(_list, &QListWidget::itemSelectionChanged, this,
            &SpectrumFetchSessionsDialog::onSelectionChanged);

    connect(_service, &SpectrumFetchService::sessionsChanged, this,
            &SpectrumFetchSessionsDialog::rebuildList);
    connect(_service, &SpectrumFetchService::sessionLogUpdated, this,
            &SpectrumFetchSessionsDialog::onLogUpdated);
    connect(_service, &SpectrumFetchService::discoveryFinished, this,
            &SpectrumFetchSessionsDialog::onDiscoveryFinished);
    connect(_service, &SpectrumFetchService::progressChanged, this,
            [this](int, int, int) { refreshProgressLabel(); });

    _ticker = new QTimer(this);
    _ticker->setInterval(1000);
    connect(_ticker, &QTimer::timeout, this,
            &SpectrumFetchSessionsDialog::refreshProgressLabel);
    _ticker->start();

    rebuildList();
    refreshProgressLabel();
}

void SpectrumFetchSessionsDialog::watchSession(const QString& sessionId) {
    _watched.insert(sessionId);
}

void SpectrumFetchSessionsDialog::rebuildList() {
    const QString keep = selectedSessionId();
    _list->clear();

    const auto sessions = _service->sessions();
    for (const auto& info : sessions) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  #%2  %3 star(s)  %4")
                .arg(stateGlyph(info.state), info.id)
                .arg(info.starCount)
                .arg(SpectrumFetchService::stateLabel(info.state)));
        item->setData(Qt::UserRole, info.id);
        _list->addItem(item);
        if (info.id == keep) _list->setCurrentItem(item);
    }
    if (!_list->currentItem() && _list->count() > 0)
        _list->setCurrentRow(0);
    onSelectionChanged();
}

QString SpectrumFetchSessionsDialog::selectedSessionId() const {
    auto* item = _list ? _list->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void SpectrumFetchSessionsDialog::onSelectionChanged() {
    const QString id = selectedSessionId();
    if (id.isEmpty()) {
        _log->clear();
        _reviewBtn->setEnabled(false);
        _cancelBtn->setEnabled(false);
        return;
    }
    _log->setPlainText(QString::fromUtf8(_service->sessionBuffer(id)));
    bool found = false;
    const auto info = _service->sessionInfo(id, &found);
    _reviewBtn->setEnabled(
        found && info.state == SpectrumFetchService::State::AwaitingSelection);
    _cancelBtn->setEnabled(found && _service->isSessionActive(id));
}

void SpectrumFetchSessionsDialog::onLogUpdated(const QString& id) {
    if (id != selectedSessionId()) return;
    _log->setPlainText(QString::fromUtf8(_service->sessionBuffer(id)));
    _log->moveCursor(QTextCursor::End);
}

void SpectrumFetchSessionsDialog::onDiscoveryFinished(
    const QString& id, const QList<SpecFetch::RemoteSpectrum>& results) {
    Q_UNUSED(results);
    if (_watched.remove(id))
        reviewSession(id);
    else
        onSelectionChanged();   // enable the review button if applicable
}

void SpectrumFetchSessionsDialog::reviewSession(const QString& sessionId) {
    bool found = false;
    const auto info = _service->sessionInfo(sessionId, &found);
    if (!found ||
        info.state != SpectrumFetchService::State::AwaitingSelection)
        return;

    const auto results = _service->discoveredResults(sessionId);
    if (results.isEmpty()) return;

    SpectrumFetchReviewDialog dlg(results, this);
    if (dlg.exec() == QDialog::Accepted) {
        _service->queueDownloads(sessionId, dlg.picks());
    } else {
        _service->cancelSession(sessionId);
    }
}

void SpectrumFetchSessionsDialog::onReviewSelected() {
    reviewSession(selectedSessionId());
}

void SpectrumFetchSessionsDialog::onCancelSelected() {
    const QString id = selectedSessionId();
    if (!id.isEmpty()) _service->cancelSession(id);
}

void SpectrumFetchSessionsDialog::onCancelAll() { _service->cancelAll(); }

void SpectrumFetchSessionsDialog::refreshProgressLabel() {
    int done = 0, total = 0;
    for (const auto& info : _service->sessions()) {
        done += info.downloadsDone;
        total += info.downloadsTotal;
    }
    if (total == 0 && !_service->hasActiveSessions()) {
        _summary->setText(tr("No active downloads"));
        return;
    }
    QString text = tr("%1 of %2 file(s) done, %3 running")
                       .arg(done)
                       .arg(total)
                       .arg(_service->runningCount());
    const QString eta = formatEta(_service->etaMsRemaining());
    if (done < total && !eta.isEmpty())
        text += QStringLiteral("  ·  ") + eta;
    _summary->setText(text);
}

// ═════════════════════════════════════════════════════════════════════════════
//  BatchSpectrumFetchSetupDialog
// ═════════════════════════════════════════════════════════════════════════════

BatchSpectrumFetchSetupDialog::BatchSpectrumFetchSetupDialog(
    const std::vector<std::shared_ptr<Star>>& allStars,
    const std::vector<std::shared_ptr<Star>>& filteredStars,
    const std::vector<std::shared_ptr<Star>>& selectedStars,
    AppSettings* settings, QWidget* parent)
    : QDialog(parent)
    , _settings(settings)
    , _all(allStars)
    , _filtered(filteredStars)
    , _selected(selectedStars) {
    setWindowTitle(tr("Fetch Spectra"));

    auto* root = new QVBoxLayout(this);

    // ── Scope ────────────────────────────────────────────────────────────
    auto* scopeRow = new QHBoxLayout;
    scopeRow->addWidget(new QLabel(tr("Stars:")));
    _scopeCombo = new QComboBox;
    _scopeCombo->addItem(tr("All project stars (%1)").arg(_all.size()));
    _scopeCombo->addItem(tr("Filtered stars (%1)").arg(_filtered.size()));
    _scopeCombo->addItem(tr("Selected stars (%1)").arg(_selected.size()));
    auto disableEmpty = [this](int index, bool empty) {
        auto* model = qobject_cast<QStandardItemModel*>(_scopeCombo->model());
        if (model && empty)
            model->item(index)->setFlags(model->item(index)->flags() &
                                         ~Qt::ItemIsEnabled);
    };
    disableEmpty(0, _all.empty());
    disableEmpty(1, _filtered.empty());
    disableEmpty(2, _selected.empty());
    _scopeCombo->setCurrentIndex(!_selected.empty()   ? 2
                                 : !_filtered.empty() ? 1
                                                      : 0);
    scopeRow->addWidget(_scopeCombo, 1);
    root->addLayout(scopeRow);

    // ── Archives ─────────────────────────────────────────────────────────
    auto* esoBox    = new QGroupBox(tr("ESO Phase 3"));
    auto* esoLay    = new QVBoxLayout(esoBox);
    _esoCb          = new QCheckBox(tr("Enabled"));
    _esoCb->setChecked(true);
    esoLay->addWidget(_esoCb);
    _esoCollections = new QListWidget;
    _esoCollections->setMaximumHeight(110);
    for (const QString& c : EsoArchiveClient::knownCollections()) {
        auto* item = new QListWidgetItem(c, _esoCollections);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    esoLay->addWidget(_esoCollections);

    auto* lamostBox = new QGroupBox(tr("LAMOST"));
    auto* lamostLay = new QFormLayout(lamostBox);
    _lrsCb = new QCheckBox(tr("Low resolution (LRS)"));
    _lrsCb->setChecked(true);
    _mrsCb = new QCheckBox(tr("Medium resolution (MRS)"));
    _lamostDr = new QComboBox;
    _lamostDr->addItems(LamostArchiveClient::knownDataReleases(false));
    lamostLay->addRow(_lrsCb);
    lamostLay->addRow(_mrsCb);
    lamostLay->addRow(tr("Data release:"), _lamostDr);
    auto* lamostHint = new QLabel(
        tr("MRS is available from DR7 on; LRS individual exposures cover "
           "Oct 2011 - Jun 2017 (the single-exposure release)."));
    lamostHint->setWordWrap(true);
    lamostLay->addRow(lamostHint);

    auto* sdssBox = new QGroupBox(tr("SDSS optical"));
    auto* sdssLay = new QFormLayout(sdssBox);
    _sdssCb = new QCheckBox(tr("Enabled"));
    _sdssCb->setChecked(true);
    _sdssDr = new QComboBox;
    _sdssDr->addItems(SdssOpticalArchiveClient::knownDataReleases());
    sdssLay->addRow(_sdssCb);
    sdssLay->addRow(tr("Data release:"), _sdssDr);

    auto* mastBox = new QGroupBox(tr("MAST (UV/space)"));
    auto* mastLay = new QVBoxLayout(mastBox);
    _mastCb = new QCheckBox(tr("Enabled"));
    mastLay->addWidget(_mastCb);
    _mastMissions = new QListWidget;
    _mastMissions->setMaximumHeight(90);
    for (const QString& m : MastArchiveClient::knownMissions()) {
        auto* item = new QListWidgetItem(m, _mastMissions);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m == QLatin1String("HST") ||
                                    m == QLatin1String("IUE") ||
                                    m == QLatin1String("FUSE")
                                ? Qt::Checked
                                : Qt::Unchecked);
    }
    mastLay->addWidget(_mastMissions);

    auto* apogeeBox = new QGroupBox(tr("SDSS APOGEE (IR)"));
    auto* apogeeLay = new QVBoxLayout(apogeeBox);
    _apogeeCb = new QCheckBox(tr("Enabled"));
    apogeeLay->addWidget(_apogeeCb);
    auto* apogeeHint = new QLabel(
        tr("H-band R~22500 apStar spectra (DR17); individual visits come "
           "from the same file."));
    apogeeHint->setWordWrap(true);
    apogeeLay->addWidget(apogeeHint);
    apogeeLay->addStretch(1);

    auto* archRow = new QHBoxLayout;
    archRow->addWidget(esoBox, 1);
    archRow->addWidget(lamostBox, 1);
    archRow->addWidget(sdssBox, 1);
    root->addLayout(archRow);

    auto* archRow2 = new QHBoxLayout;
    archRow2->addWidget(mastBox, 1);
    archRow2->addWidget(apogeeBox, 1);
    root->addLayout(archRow2);

    // ── Common options ───────────────────────────────────────────────────
    auto* optBox = new QGroupBox(tr("Options"));
    auto* optLay = new QFormLayout(optBox);
    _radiusSpin = new QDoubleSpinBox;
    _radiusSpin->setRange(0.5, 120.0);
    _radiusSpin->setDecimals(1);
    _radiusSpin->setSuffix(tr(" arcsec"));
    _radiusSpin->setValue(_settings ? _settings->specFetchRadiusArcsec() : 3.0);
    optLay->addRow(tr("Search radius:"), _radiusSpin);

    _parallelSpin = new QSpinBox;
    _parallelSpin->setRange(1, 4);
    _parallelSpin->setValue(_settings ? _settings->specFetchMaxParallel() : 2);
    _parallelSpin->setToolTip(
        tr("Capped at 4 to avoid hammering the remote archives."));
    optLay->addRow(tr("Parallel downloads:"), _parallelSpin);

    _exposuresCb = new QCheckBox(
        tr("Fetch individual exposures instead of coadds where available"));
    _exposuresCb->setToolTip(
        tr("LAMOST, SDSS and APOGEE provide the single exposures behind "
           "their coadded products (LAMOST LRS for Oct 2011 - Jun 2017). "
           "Products without exposures fall back to the coadd."));
    optLay->addRow(_exposuresCb);

    _vacToAirCb = new QCheckBox(
        tr("Convert vacuum wavelengths to air (SDSS, LAMOST)"));
    _vacToAirCb->setChecked(true);
    optLay->addRow(_vacToAirCb);

    _redownloadCb = new QCheckBox(tr("Re-download already imported spectra"));
    optLay->addRow(_redownloadCb);
    root->addWidget(optBox);

    auto* note = new QLabel(
        tr("After the archive search a review step shows what was found "
           "before anything is downloaded."));
    note->setWordWrap(true);
    root->addWidget(note);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok |
                                      QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText(tr("Search Archives"));
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);

    restoreState();
}

const std::vector<std::shared_ptr<Star>>&
BatchSpectrumFetchSetupDialog::scopeStars() const {
    switch (_scopeCombo->currentIndex()) {
    case 1:  return _filtered;
    case 2:  return _selected;
    default: return _all;
    }
}

SpectrumFetchService::Options BatchSpectrumFetchSetupDialog::options() const {
    SpectrumFetchService::Options opt;
    opt.radiusArcsec         = _radiusSpin->value();
    opt.maxParallelDownloads = _parallelSpin->value();
    opt.autoQueueAll         = false;   // bulk flow always reviews
    opt.redownloadExisting   = _redownloadCb->isChecked();

    const QString token =
        _settings ? _settings->specFetchLamostToken() : QString();

    auto baseOptions = [this, &token]() {
        SpecFetch::ArchiveOptions a;
        a.fetchExposures = _exposuresCb->isChecked();
        a.vacToAir       = _vacToAirCb->isChecked();
        a.token          = token;
        return a;
    };

    if (_esoCb->isChecked()) {
        opt.archives << SpecFetch::Archive::EsoPhase3;
        SpecFetch::ArchiveOptions a = baseOptions();
        for (int i = 0; i < _esoCollections->count(); ++i) {
            const auto* item = _esoCollections->item(i);
            if (item->checkState() == Qt::Checked)
                a.collections << item->text();
        }
        opt.perArchive.insert(SpecFetch::Archive::EsoPhase3, a);
    }
    if (_lrsCb->isChecked()) {
        opt.archives << SpecFetch::Archive::LamostLRS;
        SpecFetch::ArchiveOptions a = baseOptions();
        a.dataRelease               = _lamostDr->currentText();
        opt.perArchive.insert(SpecFetch::Archive::LamostLRS, a);
    }
    if (_mrsCb->isChecked()) {
        opt.archives << SpecFetch::Archive::LamostMRS;
        SpecFetch::ArchiveOptions a = baseOptions();
        a.dataRelease               = _lamostDr->currentText();
        opt.perArchive.insert(SpecFetch::Archive::LamostMRS, a);
    }
    if (_sdssCb->isChecked()) {
        opt.archives << SpecFetch::Archive::SdssOptical;
        SpecFetch::ArchiveOptions a = baseOptions();
        a.dataRelease               = _sdssDr->currentText();
        opt.perArchive.insert(SpecFetch::Archive::SdssOptical, a);
    }
    if (_mastCb->isChecked()) {
        opt.archives << SpecFetch::Archive::MastSSAP;
        SpecFetch::ArchiveOptions a = baseOptions();
        for (int i = 0; i < _mastMissions->count(); ++i) {
            const auto* item = _mastMissions->item(i);
            if (item->checkState() == Qt::Checked)
                a.collections << item->text();
        }
        opt.perArchive.insert(SpecFetch::Archive::MastSSAP, a);
    }
    if (_apogeeCb->isChecked()) {
        opt.archives << SpecFetch::Archive::Apogee;
        opt.perArchive.insert(SpecFetch::Archive::Apogee, baseOptions());
    }
    return opt;
}

QString BatchSpectrumFetchSetupDialog::stateToJson() const {
    QJsonObject o;
    o["eso"]      = _esoCb->isChecked();
    o["lrs"]      = _lrsCb->isChecked();
    o["mrs"]      = _mrsCb->isChecked();
    o["sdss"]     = _sdssCb->isChecked();
    o["mast"]     = _mastCb->isChecked();
    o["apogee"]   = _apogeeCb->isChecked();
    QJsonArray mastOn;
    for (int i = 0; i < _mastMissions->count(); ++i)
        if (_mastMissions->item(i)->checkState() == Qt::Checked)
            mastOn.append(_mastMissions->item(i)->text());
    o["mastOn"] = mastOn;
    o["lamostDr"] = _lamostDr->currentText();
    o["sdssDr"]   = _sdssDr->currentText();
    o["radius"]   = _radiusSpin->value();
    o["parallel"] = _parallelSpin->value();
    o["exposures"] = _exposuresCb->isChecked();
    o["vacToAir"]  = _vacToAirCb->isChecked();
    QJsonArray esoOff;   // store deselected collections, default stays "all"
    for (int i = 0; i < _esoCollections->count(); ++i)
        if (_esoCollections->item(i)->checkState() != Qt::Checked)
            esoOff.append(_esoCollections->item(i)->text());
    o["esoOff"] = esoOff;
    return QString::fromUtf8(
        QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void BatchSpectrumFetchSetupDialog::restoreState() {
    if (!_settings) return;
    const QJsonDocument doc =
        QJsonDocument::fromJson(_settings->specFetchLastOptions().toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject o = doc.object();

    _esoCb->setChecked(o.value("eso").toBool(true));
    _lrsCb->setChecked(o.value("lrs").toBool(true));
    _mrsCb->setChecked(o.value("mrs").toBool(false));
    _sdssCb->setChecked(o.value("sdss").toBool(true));
    _mastCb->setChecked(o.value("mast").toBool(false));
    _apogeeCb->setChecked(o.value("apogee").toBool(false));
    if (o.contains("mastOn")) {
        QSet<QString> on;
        for (const QJsonValue& v : o.value("mastOn").toArray())
            on.insert(v.toString());
        for (int i = 0; i < _mastMissions->count(); ++i)
            _mastMissions->item(i)->setCheckState(
                on.contains(_mastMissions->item(i)->text()) ? Qt::Checked
                                                            : Qt::Unchecked);
    }
    const QString ldr = o.value("lamostDr").toString();
    if (int i = _lamostDr->findText(ldr); i >= 0) _lamostDr->setCurrentIndex(i);
    const QString sdr = o.value("sdssDr").toString();
    if (int i = _sdssDr->findText(sdr); i >= 0) _sdssDr->setCurrentIndex(i);
    _radiusSpin->setValue(o.value("radius").toDouble(_radiusSpin->value()));
    _parallelSpin->setValue(o.value("parallel").toInt(_parallelSpin->value()));
    _exposuresCb->setChecked(o.value("exposures").toBool(false));
    _vacToAirCb->setChecked(o.value("vacToAir").toBool(true));

    QSet<QString> off;
    for (const QJsonValue& v : o.value("esoOff").toArray())
        off.insert(v.toString());
    for (int i = 0; i < _esoCollections->count(); ++i)
        _esoCollections->item(i)->setCheckState(
            off.contains(_esoCollections->item(i)->text()) ? Qt::Unchecked
                                                           : Qt::Checked);
}

void BatchSpectrumFetchSetupDialog::accept() {
    if (_settings) _settings->setSpecFetchLastOptions(stateToJson());
    QDialog::accept();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpectrumFetchReviewDialog
// ═════════════════════════════════════════════════════════════════════════════

SpectrumFetchReviewDialog::SpectrumFetchReviewDialog(
    const QList<SpecFetch::RemoteSpectrum>& results, QWidget* parent)
    : QDialog(parent), _results(results) {
    setWindowTitle(tr("Review Found Spectra"));
    resize(560, 360);

    auto* root = new QVBoxLayout(this);

    // Aggregate per archive label.
    struct Agg { int count = 0; int stars = 0; qint64 bytes = 0;
                 QSet<QString> starIds; };
    QMap<QString, Agg> byArchive;
    for (const auto& r : _results) {
        Agg& a = byArchive[r.archiveLabel];
        ++a.count;
        a.starIds.insert(r.starId);
        if (r.sizeBytes > 0) a.bytes += r.sizeBytes;
    }

    _table = new QTableWidget(byArchive.size(), 4);
    _table->setHorizontalHeaderLabels({tr("Archive"), tr("Spectra"),
                                       tr("Stars"), tr("Est. size")});
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->verticalHeader()->setVisible(false);
    _table->horizontalHeader()->setStretchLastSection(true);
    _table->setSelectionMode(QAbstractItemView::NoSelection);

    int row = 0;
    for (auto it = byArchive.constBegin(); it != byArchive.constEnd();
         ++it, ++row) {
        auto* nameItem = new QTableWidgetItem(it.key());
        nameItem->setFlags(nameItem->flags() | Qt::ItemIsUserCheckable);
        nameItem->setCheckState(Qt::Checked);
        _table->setItem(row, 0, nameItem);
        _table->setItem(row, 1,
                        new QTableWidgetItem(QString::number(it->count)));
        _table->setItem(row, 2,
                        new QTableWidgetItem(
                            QString::number(it->starIds.size())));
        _table->setItem(
            row, 3,
            new QTableWidgetItem(
                it->bytes > 0
                    ? QStringLiteral("%1 MB").arg(
                          double(it->bytes) / (1024.0 * 1024.0), 0, 'f', 1)
                    : tr("unknown")));
    }
    root->addWidget(_table, 1);

    auto* capRow = new QHBoxLayout;
    capRow->addWidget(new QLabel(
        tr("Max spectra per star and archive (0 = no limit, newest first):")));
    _capSpin = new QSpinBox;
    _capSpin->setRange(0, 999);
    _capSpin->setValue(0);
    capRow->addWidget(_capSpin);
    capRow->addStretch(1);
    root->addLayout(capRow);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok |
                                      QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText(tr("Start Download"));
    btns->button(QDialogButtonBox::Cancel)->setText(tr("Cancel Fetch"));
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);
}

QList<SpecFetch::RemoteSpectrum> SpectrumFetchReviewDialog::picks() const {
    QSet<QString> enabled;
    for (int row = 0; row < _table->rowCount(); ++row)
        if (_table->item(row, 0)->checkState() == Qt::Checked)
            enabled.insert(_table->item(row, 0)->text());

    QList<SpecFetch::RemoteSpectrum> filtered;
    for (const auto& r : _results)
        if (enabled.contains(r.archiveLabel)) filtered.append(r);

    const int cap = _capSpin->value();
    if (cap <= 0) return filtered;

    // Newest first within each (star, archive) group.
    std::stable_sort(filtered.begin(), filtered.end(),
                     [](const SpecFetch::RemoteSpectrum& a,
                        const SpecFetch::RemoteSpectrum& b) {
                         const double ma = std::isnan(a.mjd) ? -1 : a.mjd;
                         const double mb = std::isnan(b.mjd) ? -1 : b.mjd;
                         return ma > mb;
                     });
    QHash<QString, int> perGroup;
    QList<SpecFetch::RemoteSpectrum> capped;
    for (const auto& r : filtered) {
        const QString key = r.starId + QLatin1Char('|') + r.archiveLabel;
        if (perGroup[key] >= cap) continue;
        ++perGroup[key];
        capped.append(r);
    }
    return capped;
}
