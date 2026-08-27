#include "ArchiveFetchWidget.h"

#include "controllers/ApplicationController.h"
#include "models/Star.h"
#include "utils/AppSettings.h"
#include "utils/spectrafetch/LamostArchiveClient.h"
#include "utils/spectrafetch/SpectrumArchiveClient.h"
#include "utils/spectrafetch/SpectrumArchiveRegistry.h"
#include "views/widgets/FlowLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QTimeZone>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {
enum Column {
    ColArchive = 0,
    ColInstrument,
    ColDate,
    ColType,
    ColResolution,
    ColSnr,
    ColSize,
    ColStatus,
    ColCount,
};

QString mjdToDateString(double mjd) {
    if (std::isnan(mjd) || mjd <= 0) return QStringLiteral("-");
    const qint64 msecs = qint64((mjd - 40587.0) * 86400000.0);
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(msecs, QTimeZone::UTC);
    // Survey cone searches only report whole MJDs; a fabricated "00:00"
    // would suggest precision that is not there. (Imported spectra get their
    // exact epochs from the product files.)
    if (mjd == std::floor(mjd))
        return dt.toString(QStringLiteral("yyyy-MM-dd"));
    return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

QString sizeString(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("-");
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 kB").arg(bytes / 1024);
    return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0,
                                       'f', 1);
}
}   // namespace

ArchiveFetchWidget::ArchiveFetchWidget(const Context& ctx, QWidget* parent)
    : QWidget(parent), _ctx(ctx) {
    setupUi();

    if (SpectrumFetchService* svc = service()) {
        connect(svc, &SpectrumFetchService::discoveryProgress, this,
                &ArchiveFetchWidget::onDiscoveryProgress);
        connect(svc, &SpectrumFetchService::discoveryFinished, this,
                &ArchiveFetchWidget::onDiscoveryFinished);
        connect(svc, &SpectrumFetchService::itemFinished, this,
                &ArchiveFetchWidget::onItemFinished);
        connect(svc, &SpectrumFetchService::starSpectraUpdated, this,
                &ArchiveFetchWidget::onStarSpectraUpdated);
        if (_ctx.controller)
            _importedOriginIds = svc->knownOriginIds(_ctx.projectId);
    }
}

SpectrumFetchService* ArchiveFetchWidget::service() const {
    return _ctx.controller ? _ctx.controller->spectrumFetchService() : nullptr;
}

void ArchiveFetchWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // The option rows wrap instead of stretching, so this tab never dictates
    // the minimum width of the dialog's right pane.
    auto* archHost = new QWidget;
    auto* archRow  = new FlowLayout(archHost, 0, 10, 4);

    // Archive picks; every registered client shows up automatically.
    for (const auto& client : SpectrumArchiveRegistry::instance().allClients()) {
        auto* cb = new QCheckBox(client->displayName());
        cb->setChecked(true);
        _archiveChecks.insert(client->archive(), cb);
        archRow->addWidget(cb);
    }
    layout->addWidget(archHost);

    auto* optHost = new QWidget;
    auto* optRow  = new FlowLayout(optHost, 0, 10, 4);

    auto* radiusPair = new QWidget;
    {
        auto* lay = new QHBoxLayout(radiusPair);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        lay->addWidget(new QLabel(QStringLiteral("Search radius:")));
        _radiusSpin = new QDoubleSpinBox;
        _radiusSpin->setRange(0.5, 120.0);
        _radiusSpin->setDecimals(1);
        _radiusSpin->setSuffix(QStringLiteral(" arcsec"));
        _radiusSpin->setValue(
            _ctx.controller && _ctx.controller->settings()
                ? _ctx.controller->settings()->specFetchRadiusArcsec()
                : 3.0);
        lay->addWidget(_radiusSpin);
    }
    optRow->addWidget(radiusPair);

    auto* drPair = new QWidget;
    {
        auto* lay = new QHBoxLayout(drPair);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        lay->addWidget(new QLabel(QStringLiteral("LAMOST DR:")));
        _lamostDr = new QComboBox;
        _lamostDr->addItems(LamostArchiveClient::knownDataReleases(false));
        _lamostDr->setToolTip(
            QStringLiteral("Data release searched by the LAMOST archives "
                           "(MRS is available from DR7 on)."));
        // Follow the release last picked in the batch fetch dialog.
        if (_ctx.controller && _ctx.controller->settings()) {
            const QJsonDocument doc = QJsonDocument::fromJson(
                _ctx.controller->settings()->specFetchLastOptions().toUtf8());
            const QString last = doc.object().value("lamostDr").toString();
            if (int i = _lamostDr->findText(last); i >= 0)
                _lamostDr->setCurrentIndex(i);
        }
        lay->addWidget(_lamostDr);
    }
    optRow->addWidget(drPair);

    _exposuresCb = new QCheckBox(QStringLiteral("Individual exposures"));
    _exposuresCb->setToolTip(
        QStringLiteral("Fetch the single exposures instead of the coadded "
                       "product where the archive provides them (LAMOST MRS, "
                       "SDSS, APOGEE; LAMOST LRS for Oct 2011 - Jun 2017). "
                       "Products without exposures fall back to the coadd."));
    optRow->addWidget(_exposuresCb);

    _redownloadCb = new QCheckBox(QStringLiteral("Re-download existing"));
    _redownloadCb->setToolTip(
        QStringLiteral("Replace spectra that were already fetched from an "
                       "archive instead of skipping them."));
    optRow->addWidget(_redownloadCb);

    layout->addWidget(optHost);

    _table = new QTableWidget(0, ColCount);
    _table->setHorizontalHeaderLabels(
        {QStringLiteral("Archive"), QStringLiteral("Instrument"),
         QStringLiteral("Date (UTC)"), QStringLiteral("Type"),
         QStringLiteral("R"), QStringLiteral("SNR"), QStringLiteral("Size"),
         QStringLiteral("Status")});
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->verticalHeader()->setVisible(false);
    _table->horizontalHeader()->setStretchLastSection(true);
    _table->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    layout->addWidget(_table, 1);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(8);

    _statusLabel = new QLabel(
        QStringLiteral("Pick archives and press Search."));
    _statusLabel->setWordWrap(true);
    bottomRow->addWidget(_statusLabel, 1);

    _searchBtn = new QPushButton(QStringLiteral("Search"));
    connect(_searchBtn, &QPushButton::clicked, this,
            &ArchiveFetchWidget::onSearch);
    bottomRow->addWidget(_searchBtn);

    _downloadBtn = new QPushButton(QStringLiteral("Download Selected"));
    _downloadBtn->setEnabled(false);
    connect(_downloadBtn, &QPushButton::clicked, this,
            &ArchiveFetchWidget::onDownloadSelected);
    bottomRow->addWidget(_downloadBtn);

    _downloadAllBtn = new QPushButton(QStringLiteral("Download All"));
    _downloadAllBtn->setEnabled(false);
    connect(_downloadAllBtn, &QPushButton::clicked, this,
            &ArchiveFetchWidget::onDownloadAll);
    bottomRow->addWidget(_downloadAllBtn);

    layout->addLayout(bottomRow);
}

// A multi-spectrum product (LAMOST exposures, SDSS cameras) imports its
// children under "<originId>#<part>", so the product counts as imported when
// its own id or any child of it exists.
bool ArchiveFetchWidget::alreadyImported(const QString& originId) const {
    if (_importedOriginIds.contains(originId)) return true;
    const QString prefix = originId + QLatin1Char('#');
    for (const QString& id : _importedOriginIds)
        if (id.startsWith(prefix)) return true;
    return false;
}

QList<SpecFetch::Archive> ArchiveFetchWidget::checkedArchives() const {
    QList<SpecFetch::Archive> out;
    for (auto it = _archiveChecks.constBegin(); it != _archiveChecks.constEnd();
         ++it)
        if (it.value()->isChecked()) out.append(it.key());
    return out;
}

void ArchiveFetchWidget::setBusy(bool busy) {
    // The search button doubles as the stop button: an archive can take
    // minutes, and stopping keeps whatever the others already found.
    _searching = busy;
    _searchBtn->setText(busy ? QStringLiteral("Stop") : QStringLiteral("Search"));
    _searchBtn->setEnabled(true);
    _downloadBtn->setEnabled(!busy && !_results.isEmpty());
    _downloadAllBtn->setEnabled(!busy && !_results.isEmpty());
}

void ArchiveFetchWidget::onSearch() {
    if (!_ctx.star || !service()) return;

    if (_searching) {
        if (!_sessionId.isEmpty()) {
            service()->cancelSession(_sessionId);
            _statusLabel->setText(
                QStringLiteral("Stopping search; keeping what was found..."));
        }
        return;
    }

    const QList<SpecFetch::Archive> archives = checkedArchives();
    if (archives.isEmpty()) {
        _statusLabel->setText(QStringLiteral("No archives selected."));
        return;
    }

    AppSettings* settings =
        _ctx.controller ? _ctx.controller->settings() : nullptr;

    SpectrumFetchService::Options opt;
    opt.archives            = archives;
    opt.radiusArcsec        = _radiusSpin->value();
    opt.maxParallelDownloads =
        settings ? settings->specFetchMaxParallel() : 2;
    opt.autoQueueAll       = false;
    opt.redownloadExisting = _redownloadCb->isChecked();
    for (const SpecFetch::Archive a : archives) {
        SpecFetch::ArchiveOptions aopt;
        aopt.fetchExposures = _exposuresCb->isChecked();
        if (settings) aopt.token = settings->specFetchLamostToken();
        if (a == SpecFetch::Archive::LamostLRS ||
            a == SpecFetch::Archive::LamostMRS)
            aopt.dataRelease = _lamostDr->currentText();
        opt.perArchive.insert(a, aopt);
    }

    _results.clear();
    _table->setRowCount(0);

    _sessionId = service()->startSession({_ctx.star}, _ctx.projectId, opt);
    if (_sessionId.isEmpty()) {
        _statusLabel->setText(
            QStringLiteral("Cannot search: the star has no coordinates."));
        return;
    }

    _importedOriginIds = service()->knownOriginIds(_ctx.projectId);
    setBusy(true);
    _statusLabel->setText(QStringLiteral("Searching archives..."));
}

void ArchiveFetchWidget::onDiscoveryProgress(const QString& id,
                                             const QString& archiveLabel,
                                             int done, int total) {
    Q_UNUSED(done);
    Q_UNUSED(total);
    if (id != _sessionId) return;
    _statusLabel->setText(QStringLiteral("Searching %1...").arg(archiveLabel));
}

void ArchiveFetchWidget::onDiscoveryFinished(
    const QString& id, const QList<SpecFetch::RemoteSpectrum>& results) {
    if (id != _sessionId) return;

    populateTable(results);
    setBusy(false);

    if (results.isEmpty())
        _statusLabel->setText(QStringLiteral("No spectra found."));
    else
        _statusLabel->setText(
            QStringLiteral("%1 products available (an Exposures product "
                           "imports one spectrum per exposure). Select rows "
                           "and press Download Selected.")
                .arg(results.size()));
}

void ArchiveFetchWidget::populateTable(
    const QList<SpecFetch::RemoteSpectrum>& results) {
    _results = results;
    _table->setRowCount(results.size());

    for (int i = 0; i < results.size(); ++i) {
        const SpecFetch::RemoteSpectrum& r = results.at(i);

        auto setItem = [this, i](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            _table->setItem(i, col, item);
        };

        setItem(ColArchive, r.archiveLabel);
        setItem(ColInstrument, r.instrumentHint.isEmpty() ? r.collection
                                                          : r.instrumentHint);
        setItem(ColDate, mjdToDateString(r.mjd));
        setItem(ColType, r.isCoadd ? QStringLiteral("Coadd")
                                   : QStringLiteral("Exposures"));
        setItem(ColResolution,
                std::isnan(r.resolution)
                    ? QStringLiteral("-")
                    : QStringLiteral("%1").arg(qRound(r.resolution)));
        setItem(ColSnr, std::isnan(r.snr)
                            ? QStringLiteral("-")
                            : QStringLiteral("%1").arg(r.snr, 0, 'f', 1));
        setItem(ColSize, sizeString(r.sizeBytes));
        setItem(ColStatus, alreadyImported(r.originId)
                               ? QStringLiteral("imported")
                               : QString());
    }
}

void ArchiveFetchWidget::onDownloadSelected() {
    QList<SpecFetch::RemoteSpectrum> picks;
    QSet<int> rows;
    const auto selected = _table->selectionModel()
                              ? _table->selectionModel()->selectedRows()
                              : QModelIndexList();
    for (const QModelIndex& idx : selected) rows.insert(idx.row());
    for (int row : rows)
        if (row >= 0 && row < _results.size()) picks.append(_results.at(row));

    if (picks.isEmpty()) {
        _statusLabel->setText(QStringLiteral("No rows selected."));
        return;
    }
    queuePicks(picks);
}

void ArchiveFetchWidget::onDownloadAll() {
    if (_results.isEmpty()) return;
    queuePicks(_results);
}

void ArchiveFetchWidget::queuePicks(
    const QList<SpecFetch::RemoteSpectrum>& picks) {
    if (_sessionId.isEmpty() || !service()) return;
    service()->queueDownloads(_sessionId, picks);
    _statusLabel->setText(
        QStringLiteral("Downloading %1 file(s)...").arg(picks.size()));
}

void ArchiveFetchWidget::setRowStatus(int row, const QString& text) {
    if (row < 0 || row >= _table->rowCount()) return;
    if (QTableWidgetItem* item = _table->item(row, ColStatus))
        item->setText(text);
}

void ArchiveFetchWidget::onItemFinished(const QString& id,
                                        const SpecFetch::RemoteSpectrum& item,
                                        bool ok, bool skippedDuplicate,
                                        const QString& message) {
    if (id != _sessionId) return;

    for (int i = 0; i < _results.size(); ++i) {
        if (_results.at(i).originId != item.originId) continue;
        if (skippedDuplicate)
            setRowStatus(i, QStringLiteral("already imported"));
        else if (ok)
            // The service reports how many spectra the product yielded
            // ("imported 3 spectrum(a)") - a row can import several.
            setRowStatus(i, message.isEmpty() ? QStringLiteral("imported")
                                              : message);
        else
            setRowStatus(i, QStringLiteral("failed: %1").arg(message));
        break;
    }

    bool found = false;
    const auto info = service()->sessionInfo(_sessionId, &found);
    if (found && info.downloadsTotal > 0) {
        if (info.downloadsDone >= info.downloadsTotal)
            _statusLabel->setText(info.summary.isEmpty()
                                      ? QStringLiteral("Done.")
                                      : info.summary);
        else
            _statusLabel->setText(QStringLiteral("Downloading... %1/%2")
                                      .arg(info.downloadsDone)
                                      .arg(info.downloadsTotal));
    }
}

void ArchiveFetchWidget::onStarSpectraUpdated(const QString& starId) {
    if (!_ctx.star || starId != _ctx.star->getId()) return;
    if (service())
        _importedOriginIds = service()->knownOriginIds(_ctx.projectId);
    emit spectraImported();
}
