#include "ReidentifyStarDialog.h"

#include "db/DatabaseManager.h" // adjust include path to your DatabaseManager
#include "models/Star.h"
#include "utils/Logger.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHttpMultiPart>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

QString digitsOnly(const QString &s) {
    QRegularExpression re("(\\d{6,})");
    auto               m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString jnameFromCoords(double raDeg, double decDeg) {
    if (std::isnan(raDeg) || std::isnan(decDeg))
        return {};
    double raH  = raDeg / 15.0;
    int    rh   = static_cast<int>(std::floor(raH));
    double remH = (raH - rh) * 60.0;
    int    rm   = static_cast<int>(std::floor(remH));
    double rs   = (remH - rm) * 60.0;

    double aDec = std::fabs(decDeg);
    int    dd   = static_cast<int>(std::floor(aDec));
    double remD = (aDec - dd) * 60.0;
    int    dm   = static_cast<int>(std::floor(remD));
    double ds   = (remD - dm) * 60.0;

    QChar sign = decDeg >= 0 ? '+' : '-';
    return QString::asprintf("J%02d%02d%05.2f%c%02d%02d%04.1f", rh, rm, rs,
                             sign.toLatin1(), dd, dm, ds);
}

// Try to parse "ra dec" (two decimal-degree numbers, space/comma separated).
bool parseCoordPair(const QString &s, double &ra, double &dec) {
    QString t = s.simplified();
    t.replace(',', ' ');
    const QStringList parts = t.split(' ', Qt::SkipEmptyParts);
    if (parts.size() != 2)
        return false;
    bool   okR = false, okD = false;
    double r = parts[0].toDouble(&okR);
    double d = parts[1].toDouble(&okD);
    if (!okR || !okD)
        return false;
    ra  = r;
    dec = d;
    return true;
}

// CSV → header index map + value rows.
struct Csv {
    QMap<QString, int> idx;
    QList<QStringList> rows;
    bool               empty() const { return rows.isEmpty(); }
};

Csv parseCsv(const QString &body) {
    Csv               out;
    const QStringList lines = body.split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 1)
        return out;
    const QStringList headers = lines[0].split(',');
    for (int i = 0; i < headers.size(); ++i)
        out.idx[headers[i].trimmed().toLower().remove('"')] = i;
    for (int i = 1; i < lines.size(); ++i)
        out.rows << lines[i].split(',');
    return out;
}

double cellD(const Csv &c, const QStringList &row, const QString &col) {
    int i = c.idx.value(col.toLower(), -1);
    if (i < 0 || i >= row.size())
        return std::numeric_limits<double>::quiet_NaN();
    QString s = row[i].trimmed().remove('"');
    if (s.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    bool   ok;
    double v = s.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

QString cellS(const Csv &c, const QStringList &row, const QString &col) {
    int i = c.idx.value(col.toLower(), -1);
    if (i < 0 || i >= row.size())
        return {};
    return row[i].trimmed().remove('"');
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / UI
// ─────────────────────────────────────────────────────────────────────────────
ReidentifyStarDialog::ReidentifyStarDialog(std::shared_ptr<Star> star,
                                           DatabaseManager      *dbm,
                                           QString projectId, QWidget *parent)
    : QDialog(parent), _star(std::move(star)), _dbm(dbm),
      _projectId(std::move(projectId)),
      _network(new QNetworkAccessManager(this)) {
    setWindowTitle("Re-identify Star");
    setMinimumWidth(560);

    if (_star) {
        _centerRa  = _star->getRa();
        _centerDec = _star->getDec();
    }

    setupUi();

    // Auto-run the first 5″ search if we have a position.
    if (!std::isnan(_centerRa) && !std::isnan(_centerDec))
        QTimer::singleShot(0, this, &ReidentifyStarDialog::onSearch);
    else
        setStatus(
            "This star has no coordinates - enter a name or RA/Dec below.",
            true);
}

ReidentifyStarDialog::~ReidentifyStarDialog() = default;

void ReidentifyStarDialog::setupUi() {
    auto *root = new QVBoxLayout(this);

    QString cur  = _star
                       ? (_star->getAlias().isEmpty()
                              ? QString("Gaia DR3 %1").arg(_star->getSourceId())
                              : _star->getAlias())
                       : QString();
    _headerLabel = new QLabel(
        QString("Current: <b>%1</b><br>"
                "Pick the source this star should actually point to.")
            .arg(cur.toHtmlEscaped()),
        this);
    _headerLabel->setTextFormat(Qt::RichText);
    root->addWidget(_headerLabel);

    // ── Cone-search controls ──────────────────────────────────────────────
    auto *searchRow = new QHBoxLayout();
    searchRow->addWidget(new QLabel("Radius:", this));
    _radiusSpin = new QDoubleSpinBox(this);
    _radiusSpin->setRange(0.5, 300.0);
    _radiusSpin->setDecimals(1);
    _radiusSpin->setSingleStep(1.0);
    _radiusSpin->setValue(5.0);
    _radiusSpin->setSuffix(" \u2033"); // arcsec
    searchRow->addWidget(_radiusSpin);

    _searchBtn = new QPushButton("Search Gaia", this);
    connect(_searchBtn, &QPushButton::clicked, this,
            &ReidentifyStarDialog::onSearch);
    searchRow->addWidget(_searchBtn);
    searchRow->addStretch();
    root->addLayout(searchRow);

    // ── Results table ─────────────────────────────────────────────────────
    _table = new QTableWidget(this);
    _table->setColumnCount(3);
    _table->setHorizontalHeaderLabels({"Source", "G [mag]", "Sep [\u2033]"});
    _table->horizontalHeader()->setStretchLastSection(false);
    _table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    _table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->verticalHeader()->setVisible(false);
    _table->setMinimumHeight(180);
    connect(_table, &QTableWidget::itemSelectionChanged, this,
            &ReidentifyStarDialog::onSelectionChanged);
    root->addWidget(_table);

    // ── Manual entry ──────────────────────────────────────────────────────
    auto *manualRow = new QHBoxLayout();
    manualRow->addWidget(new QLabel("Or enter manually:", this));
    _manualEdit = new QLineEdit(this);
    _manualEdit->setPlaceholderText(
        "Name (e.g. Gaia DR3 12345, TIC 9999, HD 1234) or \"ra dec\" in deg");
    manualRow->addWidget(_manualEdit, 1);
    _manualBtn = new QPushButton("Resolve", this);
    connect(_manualBtn, &QPushButton::clicked, this,
            &ReidentifyStarDialog::onManualResolve);
    connect(_manualEdit, &QLineEdit::returnPressed, this,
            &ReidentifyStarDialog::onManualResolve);
    manualRow->addWidget(_manualBtn);
    root->addLayout(manualRow);

    // ── Options / status ──────────────────────────────────────────────────
    _bibliographyCheck = new QCheckBox("Re-query bibliography (SIMBAD)", this);
    _bibliographyCheck->setChecked(true);
    root->addWidget(_bibliographyCheck);

    auto *statusRow = new QHBoxLayout();
    _busy           = new QProgressBar(this);
    _busy->setRange(0, 0);
    _busy->setMaximumWidth(120);
    _busy->setVisible(false);
    statusRow->addWidget(_busy);
    _status = new QLabel(this);
    _status->setWordWrap(true);
    statusRow->addWidget(_status, 1);
    root->addLayout(statusRow);

    // ── Buttons ───────────────────────────────────────────────────────────
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    _applyBtn = buttons->button(QDialogButtonBox::Ok);
    _applyBtn->setText("Apply && Save");
    _applyBtn->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this,
            &ReidentifyStarDialog::onApply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

// ─────────────────────────────────────────────────────────────────────────────
// Status helpers
// ─────────────────────────────────────────────────────────────────────────────
void ReidentifyStarDialog::setBusy(bool busy) {
    _busy->setVisible(busy);
    _searchBtn->setEnabled(!busy);
    _manualBtn->setEnabled(!busy);
    if (busy)
        QApplication::setOverrideCursor(Qt::BusyCursor);
    else
        QApplication::restoreOverrideCursor();
}

void ReidentifyStarDialog::setStatus(const QString &msg, bool isError) {
    _status->setText(msg);
    _status->setStyleSheet(isError ? "color: #c0392b;" : "");
}

// ─────────────────────────────────────────────────────────────────────────────
// Search
// ─────────────────────────────────────────────────────────────────────────────
void ReidentifyStarDialog::onSearch() {
    if (std::isnan(_centerRa) || std::isnan(_centerDec)) {
        setStatus("No search center. Resolve a name or enter RA/Dec first.",
                  true);
        return;
    }

    setBusy(true);
    setStatus("Searching Gaia DR3\u2026");
    QApplication::processEvents();

    std::vector<Candidate> found;
    QString                err;
    bool                   ok =
        coneSearchGaia(_centerRa, _centerDec, _radiusSpin->value(), found, err);
    setBusy(false);

    if (!ok) {
        setStatus(QString("Gaia cone search failed: %1").arg(err), true);
        return;
    }

    _candidates = std::move(found);
    populateTable();

    if (_candidates.empty())
        setStatus(
            QString("No Gaia sources within %1\u2033. Try a wider radius.")
                .arg(_radiusSpin->value(), 0, 'f', 1),
            true);
    else
        setStatus(QString("Found %1 source(s) within %2\u2033.")
                      .arg(_candidates.size())
                      .arg(_radiusSpin->value(), 0, 'f', 1));
}

void ReidentifyStarDialog::populateTable() {
    _table->setRowCount(static_cast<int>(_candidates.size()));
    for (int i = 0; i < static_cast<int>(_candidates.size()); ++i) {
        const Candidate &c = _candidates[i];
        QString          name =
            c.alias.isEmpty()
                ? QString("Gaia DR3 %1").arg(c.sourceId)
                : QString("%1  (Gaia DR3 %2)").arg(c.alias, c.sourceId);

        auto *nameItem = new QTableWidgetItem(name);
        auto *gItem    = new QTableWidgetItem(
            std::isnan(c.gmag) ? "\u2014" : QString::number(c.gmag, 'f', 3));
        auto *sepItem = new QTableWidgetItem(
            std::isnan(c.sepArcsec) ? "\u2014"
                                    : QString::number(c.sepArcsec, 'f', 2));
        gItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sepItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        _table->setItem(i, 0, nameItem);
        _table->setItem(i, 1, gItem);
        _table->setItem(i, 2, sepItem);
    }
    _applyBtn->setEnabled(false);
}

void ReidentifyStarDialog::onSelectionChanged() {
    _applyBtn->setEnabled(!_table->selectedItems().isEmpty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Manual resolve
// ─────────────────────────────────────────────────────────────────────────────
void ReidentifyStarDialog::onManualResolve() {
    const QString text = _manualEdit->text().trimmed();
    if (text.isEmpty())
        return;

    // Coordinate pair → recenter and cone-search.
    double ra, dec;
    if (parseCoordPair(text, ra, dec)) {
        _centerRa  = ra;
        _centerDec = dec;
        onSearch();
        return;
    }

    setBusy(true);
    setStatus("Resolving via SIMBAD\u2026");
    QApplication::processEvents();

    // Build a SIMBAD query from the free-form identifier.
    QString q;
    QString num = digitsOnly(text);
    if (text.startsWith("Gaia", Qt::CaseInsensitive) && !num.isEmpty())
        q = QString("query id Gaia DR3 %1").arg(num);
    else if (text.startsWith("TIC", Qt::CaseInsensitive) && !num.isEmpty())
        q = QString("query id TIC %1").arg(num);
    else
        q = QString("query id %1").arg(text);

    ResolvedIds ids;
    QString     err;
    bool        ok = resolveSimbad(q, ids, err);
    setBusy(false);

    if (!ok) {
        setStatus(QString("SIMBAD: %1").arg(err), true);
        return;
    }

    // Recenter on the resolved object if it has coords.
    if (!std::isnan(ids.ra) && !std::isnan(ids.dec)) {
        _centerRa  = ids.ra;
        _centerDec = ids.dec;
    }

    // Present the resolved object as a single (selected) candidate.
    Candidate c;
    c.sourceId  = ids.sourceId;
    c.alias     = ids.mainId;
    c.ra        = ids.ra;
    c.dec       = ids.dec;
    c.sepArcsec = std::numeric_limits<double>::quiet_NaN();

    _candidates.clear();
    _candidates.push_back(c);
    populateTable();
    _table->selectRow(0);

    if (c.sourceId.isEmpty())
        setStatus(
            "Resolved (no Gaia DR3 id - Gaia photometry will be skipped).",
            false);
    else
        setStatus(QString("Resolved to Gaia DR3 %1.").arg(c.sourceId));
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply
// ─────────────────────────────────────────────────────────────────────────────
void ReidentifyStarDialog::onApply() {
    const int row = _table->currentRow();
    if (row < 0 || row >= static_cast<int>(_candidates.size())) {
        setStatus("Select a source first.", true);
        return;
    }
    if (!_star) {
        reject();
        return;
    }

    Candidate c = _candidates[row];

    setBusy(true);
    setStatus("Fetching metadata\u2026");
    QApplication::processEvents();

    // 1) Refresh cross-IDs / alias / tic from SIMBAD when we have a Gaia id.
    ResolvedIds ids;
    if (!c.sourceId.isEmpty()) {
        QString serr;
        if (!resolveSimbad(QString("query id Gaia DR3 %1").arg(c.sourceId), ids,
                           serr))
            LOG_WARNING("ReidentifyStarDialog",
                        QString("SIMBAD failed: %1").arg(serr));
    } else {
        ids.mainId = c.alias;
        ids.ra     = c.ra;
        ids.dec    = c.dec;
    }

    // 2) Full Gaia DR3 astrometry/photometry.
    GaiaData g;
    if (!c.sourceId.isEmpty()) {
        QString gerr;
        if (!fetchGaiaFull(c.sourceId, g, gerr)) {
            setBusy(false);
            setStatus(QString("Gaia DR3 fetch failed: %1").arg(gerr), true);
            return;
        }
    }

    // Final coordinates: prefer Gaia, then SIMBAD, then candidate.
    double ra  = g.ok && !std::isnan(g.ra)
                     ? g.ra
                     : (!std::isnan(ids.ra) ? ids.ra : c.ra);
    double dec = g.ok && !std::isnan(g.dec)
                     ? g.dec
                     : (!std::isnan(ids.dec) ? ids.dec : c.dec);

    // ── Apply ONLY the identity + Gaia-derived fields. ────────────────────
    Star &s = *_star;

    // Identity
    if (!c.sourceId.isEmpty())
        s.setSourceId(c.sourceId);
    if (!ids.tic.isEmpty())
        s.setTic(ids.tic);
    if (!ids.mainId.isEmpty())
        s.setAlias(ids.mainId);
    else if (!c.alias.isEmpty())
        s.setAlias(c.alias);
    s.setJName(jnameFromCoords(ra, dec));

    // Astrometry
    s.setRa(ra);
    s.setDec(dec);
    s.setPmra(g.pmra);
    s.setPmdec(g.pmdec);
    s.setEPmra(g.e_pmra);
    s.setEPmdec(g.e_pmdec);
    s.setPlx(g.plx);
    s.setEPlx(g.e_plx);
    s.setPmraPmdecCorr(g.pmra_pmdec_corr);
    s.setPlxPmraCorr(g.plx_pmra_corr);
    s.setPlxPmdecCorr(g.plx_pmdec_corr);

    // Photometry
    s.setGmag(g.gmag);
    s.setEGmag(g.e_gmag);
    s.setBp(g.bp);
    s.setEBp(g.e_bp);
    s.setRp(g.rp);
    s.setERp(g.e_rp);
    s.setBpRp(g.bp_rp);

    // 3) Bibliography - replace the bibcode list.
    if (_bibliographyCheck->isChecked() && !c.sourceId.isEmpty()) {
        std::vector<QString> bibs;
        QString              berr;
        if (fetchBibcodes(c.sourceId, bibs, berr)) {
            s.setBibcodes(bibs); // intentional full replace
        } else {
            LOG_WARNING(
                "ReidentifyStarDialog",
                QString("Bibcode fetch failed (kept old list): %1").arg(berr));
        }
    }

    // 4) Persist.
    _dbm->updateStar(_projectId, _star);

    setBusy(false);
    LOG_INFO("ReidentifyStarDialog",
             QString("Re-identified star %1 -> Gaia DR3 %2")
                 .arg(s.getId(), s.getSourceId()));
    accept();
}

// ─────────────────────────────────────────────────────────────────────────────
// Gaia cone search (VizieR TAP)
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::coneSearchGaia(double ra, double dec,
                                          double                  radiusArcsec,
                                          std::vector<Candidate> &out,
                                          QString                &err) {
    const double radDeg = radiusArcsec / 3600.0;

    const QString adql =
        QString("SELECT Source, RA_ICRS, DE_ICRS, Gmag, "
                "DISTANCE(POINT('ICRS',RA_ICRS,DE_ICRS),"
                "POINT('ICRS',%1,%2)) AS dist "
                "FROM \"I/355/gaiadr3\" "
                "WHERE 1=CONTAINS(POINT('ICRS',RA_ICRS,DE_ICRS),"
                "CIRCLE('ICRS',%1,%2,%3)) "
                "ORDER BY dist ASC")
            .arg(ra, 0, 'f', 9)
            .arg(dec, 0, 'f', 9)
            .arg(radDeg, 0, 'f', 9);

    LOG_DEBUG("ReidentifyStarDialog", QString("Cone ADQL: %1").arg(adql));

    QNetworkRequest req(
        QUrl("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "ASTRA/1.0");

    QUrlQuery post;
    post.addQueryItem("REQUEST", "doQuery");
    post.addQueryItem("LANG", "ADQL");
    post.addQueryItem("FORMAT", "csv");
    post.addQueryItem("QUERY", adql);

    QNetworkReply *reply =
        _network->post(req, post.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        reply->deleteLater();
        err = "VizieR timed out";
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        err = reply->errorString();
        reply->deleteLater();
        return false;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const Csv csv = parseCsv(body);
    for (const QStringList &row : csv.rows) {
        Candidate c;
        // Source can be a big integer - keep as string.
        c.sourceId = cellS(csv, row, "source");
        if (c.sourceId.isEmpty())
            c.sourceId = cellS(csv, row, "source_id");
        c.ra           = cellD(csv, row, "ra_icrs");
        c.dec          = cellD(csv, row, "de_icrs");
        c.gmag         = cellD(csv, row, "gmag");
        double distDeg = cellD(csv, row, "dist");
        c.sepArcsec    = std::isnan(distDeg)
                             ? std::numeric_limits<double>::quiet_NaN()
                             : distDeg * 3600.0;
        if (!c.sourceId.isEmpty())
            out.push_back(c);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIMBAD resolve (cross-IDs + coords) - mirrors AddStarDialog::resolveSimbad
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::resolveSimbad(const QString &queryStr,
                                         ResolvedIds &out, QString &err) {
    static const QString kBegin = "==ASTRABEGIN==";
    static const QString kEnd   = "==ASTRAEND==";

    QString script;
    script = "format object f1 \"";
    script += kBegin + "\\n";
    script += "MAIN=%MAIN_ID\\n";
    script += "GAIA=%IDLIST(Gaia DR3)\\n";
    script += "TIC=%IDLIST(TIC)\\n";
    script += "RA=%COO(d;A;ICRS;J2000;2000)\\n";
    script += "DEC=%COO(d;D;ICRS;J2000;2000)\\n";
    script += kEnd + "\"\n";
    script += queryStr + "\n";

    QNetworkRequest req(
        QUrl("https://simbad.cds.unistra.fr/simbad/sim-script"));
    req.setRawHeader("User-Agent", "ASTRA/1.0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    auto     *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant("form-data; name=\"scriptFile\"; filename=\"q.txt\""));
    part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain"));
    part.setBody(script.toUtf8());
    mp->append(part);

    QNetworkReply *reply = _network->post(req, mp);
    mp->setParent(reply);

    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        reply->deleteLater();
        err = "timed out";
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        err = reply->errorString();
        reply->deleteLater();
        return false;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const int dataIdx   = body.indexOf("::data::");
    const int searchPos = (dataIdx >= 0) ? dataIdx : 0;
    int       b         = body.indexOf(kBegin, searchPos);
    int       e         = (b >= 0) ? body.indexOf(kEnd, b + kBegin.size()) : -1;

    if (b < 0 || e < 0) {
        int errIdx = body.indexOf("::error::");
        if (errIdx >= 0) {
            QString           reason;
            const QStringList lines = body.mid(errIdx).split('\n');
            for (int i = 1; i < lines.size(); ++i) {
                QString t = lines[i].trimmed();
                if (t.isEmpty() || t.startsWith("::") || t.startsWith('['))
                    continue;
                reason = t;
                break;
            }
            err = reason.isEmpty() ? "object not found" : reason;
        } else {
            err = "no data block in response";
        }
        return false;
    }

    const QString block = body.mid(b + kBegin.size(), e - b - kBegin.size());
    for (const QString &rawLine : block.split('\n', Qt::SkipEmptyParts)) {
        QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();
        if (val == "~" || val.isEmpty())
            continue;

        if (key == "MAIN")
            out.mainId = val;
        else if (key == "GAIA") {
            QString n = digitsOnly(val);
            if (!n.isEmpty())
                out.sourceId = n;
        } else if (key == "TIC") {
            QString n = digitsOnly(val);
            if (!n.isEmpty())
                out.tic = n;
        } else if (key == "RA") {
            bool   ok;
            double v = val.toDouble(&ok);
            if (ok)
                out.ra = v;
        } else if (key == "DEC") {
            bool   ok;
            double v = val.toDouble(&ok);
            if (ok)
                out.dec = v;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Full Gaia DR3 record - mirrors AddStarDialog::fetchGaiaDR3
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::fetchGaiaFull(const QString &sourceId, GaiaData &out,
                                         QString &err) {
    const QString adql =
        "SELECT Source, RA_ICRS, DE_ICRS, pmRA, pmDE, e_pmRA, e_pmDE, "
        "Plx, e_Plx, Gmag, BPmag, RPmag, "
        "FG, e_FG, FBP, e_FBP, FRP, e_FRP, "
        "pmRApmDEcor, PlxpmRAcor, PlxpmDEcor "
        "FROM \"I/355/gaiadr3\" WHERE Source=" +
        sourceId;

    QNetworkRequest req(
        QUrl("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "ASTRA/1.0");

    QUrlQuery post;
    post.addQueryItem("REQUEST", "doQuery");
    post.addQueryItem("LANG", "ADQL");
    post.addQueryItem("FORMAT", "csv");
    post.addQueryItem("QUERY", adql);

    QNetworkReply *reply =
        _network->post(req, post.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        reply->deleteLater();
        err = "VizieR timed out";
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        err = reply->errorString();
        reply->deleteLater();
        return false;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const Csv csv = parseCsv(body);
    if (csv.empty()) {
        err = "no Gaia DR3 record";
        return false;
    }
    const QStringList &row = csv.rows.first();

    out.ra      = cellD(csv, row, "ra_icrs");
    out.dec     = cellD(csv, row, "de_icrs");
    out.pmra    = cellD(csv, row, "pmra");
    out.pmdec   = cellD(csv, row, "pmde");
    out.e_pmra  = cellD(csv, row, "e_pmra");
    out.e_pmdec = cellD(csv, row, "e_pmde");
    out.plx     = cellD(csv, row, "plx");
    out.e_plx   = cellD(csv, row, "e_plx");
    out.gmag    = cellD(csv, row, "gmag");
    out.bp      = cellD(csv, row, "bpmag");
    out.rp      = cellD(csv, row, "rpmag");

    static constexpr double kPogson = 2.5 / 2.302585092994046;
    auto magErr = [&](const char *f, const char *eF) -> double {
        double F   = cellD(csv, row, f);
        double eFv = cellD(csv, row, eF);
        if (std::isnan(F) || std::isnan(eFv) || F <= 0.0 || eFv <= 0.0)
            return std::numeric_limits<double>::quiet_NaN();
        return kPogson * (eFv / F);
    };
    out.e_gmag = magErr("fg", "e_fg");
    out.e_bp   = magErr("fbp", "e_fbp");
    out.e_rp   = magErr("frp", "e_frp");

    out.pmra_pmdec_corr = cellD(csv, row, "pmrapmdecor");
    out.plx_pmra_corr   = cellD(csv, row, "plxpmracor");
    out.plx_pmdec_corr  = cellD(csv, row, "plxpmdecor");

    if (!std::isnan(out.bp) && !std::isnan(out.rp))
        out.bp_rp = out.bp - out.rp;

    out.ok = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Bibliography - SIMBAD TAP (ident ⋈ has_ref ⋈ ref)
// Replace this with your existing background fetcher if you prefer.
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::fetchBibcodes(const QString        &sourceId,
                                         std::vector<QString> &out,
                                         QString              &err) {
    const QString adql = QString("SELECT r.bibcode "
                                 "FROM ident AS i "
                                 "JOIN has_ref AS h ON h.oidref = i.oidref "
                                 "JOIN ref AS r ON r.oidbib = h.oidbibref "
                                 "WHERE i.id = 'Gaia DR3 %1'")
                             .arg(sourceId);

    QNetworkRequest req(
        QUrl("https://simbad.cds.unistra.fr/simbad/sim-tap/sync"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "ASTRA/1.0");

    QUrlQuery post;
    post.addQueryItem("REQUEST", "doQuery");
    post.addQueryItem("LANG", "ADQL");
    post.addQueryItem("FORMAT", "csv");
    post.addQueryItem("QUERY", adql);

    QNetworkReply *reply =
        _network->post(req, post.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        reply->deleteLater();
        err = "SIMBAD TAP timed out";
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        err = reply->errorString();
        reply->deleteLater();
        return false;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const Csv csv = parseCsv(body);
    for (const QStringList &row : csv.rows) {
        QString bib = cellS(csv, row, "bibcode");
        if (!bib.isEmpty())
            out.push_back(bib);
    }
    return true; // empty result is valid (object simply has no refs)
}