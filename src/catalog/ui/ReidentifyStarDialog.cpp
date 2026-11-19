#include "catalog/ui/ReidentifyStarDialog.h"
#include "core/DelimitedTable.h"
#include "core/SkyGeometry.h"

#include "db/DatabaseManager.h" // adjust include path to your DatabaseManager
#include "core/Star.h"
#include "catalog/CdsTapClient.h"
#include "catalog/StarResolver.h"
#include "app/Logger.h"

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
    _bibliographyCheck->setToolTip(
        "Asks SIMBAD (CDS or its CfA mirror). Only SIMBAD has the "
        "bibliography; when it is unreachable the current list is kept.");
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
            std::isnan(c.gmag) ? "-" : QString::number(c.gmag, 'f', 3));
        auto *sepItem = new QTableWidgetItem(
            std::isnan(c.sepArcsec) ? "-"
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
    setStatus("Resolving\u2026");
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

    if (!ok) {
        // Without SIMBAD: a Gaia id is looked up directly, a TIC at MAST, a
        // J-designation is read off the name, anything else goes to the
        // MAST/NED/Sesame name resolvers; then a cone search recentres.
        LOG_WARNING("ReidentifyStarDialog",
                    QString("SIMBAD failed (%1); resolving without it").arg(err));
        setStatus("SIMBAD failed; resolving without it\u2026");
        QApplication::processEvents();

        double rRa = kNaN, rDec = kNaN, radius = _radiusSpin->value();
        QString via;
        if (text.startsWith("Gaia", Qt::CaseInsensitive) && !num.isEmpty()) {
            GaiaData g;
            QString  gerr;
            if (fetchGaiaFull(num, g, gerr) && g.ok) {
                rRa = g.ra; rDec = g.dec; via = "Gaia DR3";
            }
        } else if (text.startsWith("TIC", Qt::CaseInsensitive) && !num.isEmpty()) {
            StarResolver::TicEntry t;
            if (StarResolver::ticById(_network, num, t).ok) {
                rRa = t.ra; rDec = t.dec; via = "MAST TIC";
            }
        } else {
            double dr = 0;
            if (StarResolver::positionFromDesignation(text, rRa, rDec, dr)) {
                via    = "the designation";
                radius = std::max(radius, dr);
            } else {
                StarResolver::NamedPosition p;
                const auto o = StarResolver::resolveName(_network, text, p);
                if (o.ok) {
                    rRa = p.ra; rDec = p.dec; via = o.service;
                }
            }
        }
        setBusy(false);
        if (std::isnan(rRa) || std::isnan(rDec)) {
            setStatus(QString("SIMBAD: %1; no other resolver knows '%2' either.")
                          .arg(err, text), true);
            return;
        }
        _centerRa  = rRa;
        _centerDec = rDec;
        if (radius > _radiusSpin->value())
            _radiusSpin->setValue(radius);
        onSearch();
        if (!_candidates.empty())
            setStatus(QString("SIMBAD unavailable (%1). Position from %2; "
                              "%3 Gaia source(s) within %4\u2033, nearest first.")
                          .arg(err, via)
                          .arg(_candidates.size())
                          .arg(_radiusSpin->value(), 0, 'f', 1));
        return;
    }
    setBusy(false);

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

    // No TIC from SIMBAD (unreachable, or no cross-id): take the TIC entry at
    // the new position from MAST, so a re-pointed star does not keep the old
    // source's TIC.
    if (ids.tic.isEmpty() && !std::isnan(ra) && !std::isnan(dec)) {
        StarResolver::TicEntry t;
        const auto o = StarResolver::ticCone(_network, ra, dec,
                                             StarResolver::kDefaultMatchArcsec, t);
        if (o.ok)
            ids.tic = t.tic;
        else
            LOG_WARNING("ReidentifyStarDialog",
                        QString("TIC fallback failed: %1").arg(o.error));
    }

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
    s.setJName(SkyGeometry::jnameFromCoords(ra, dec));

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
    // VizieR TAP, else the ESA Gaia archive.
    std::vector<StarResolver::GaiaSource> found;
    const StarResolver::Outcome o =
        StarResolver::gaiaCone(_network, ra, dec, radiusArcsec, found);
    if (!o.ok) {
        err = QString("%1: %2").arg(o.service, o.error);
        return false;
    }
    for (const auto &g : found) {
        Candidate c;
        c.sourceId  = g.sourceId;
        c.ra        = g.ra;
        c.dec       = g.dec;
        c.gmag      = g.gmag;
        c.sepArcsec = g.sepArcsec;
        out.push_back(c);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIMBAD resolve (cross-IDs + coords) - mirrors AddStarDialog::resolveSimbad
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::resolveSimbad(const QString &queryStr,
                                         ResolvedIds &out, QString &err) {
    StarResolver::SimbadIds ids;
    const StarResolver::Outcome o =
        StarResolver::simbadIdentify(_network, queryStr, ids);
    if (!o.ok) {
        err = o.unavailable ? QStringLiteral("unreachable (%1)").arg(o.error) : o.error;
        return false;
    }
    out.mainId   = ids.mainId;
    out.sourceId = ids.sourceId;
    out.tic      = ids.tic;
    out.ra       = ids.ra;
    out.dec      = ids.dec;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Full Gaia DR3 record - mirrors AddStarDialog::fetchGaiaDR3
// ─────────────────────────────────────────────────────────────────────────────
bool ReidentifyStarDialog::fetchGaiaFull(const QString &sourceId, GaiaData &out,
                                         QString &err) {
    CdsTap::VizierLookup lookup;
    lookup.table     = "I/355/gaiadr3";
    lookup.columns   = {"Source", "RA_ICRS", "DE_ICRS", "pmRA", "pmDE",
                        "e_pmRA", "e_pmDE", "Plx", "e_Plx",
                        "Gmag", "BPmag", "RPmag",
                        "FG", "e_FG", "FBP", "e_FBP", "FRP", "e_FRP",
                        "pmRApmDEcor", "PlxpmRAcor", "PlxpmDEcor"};
    lookup.keyColumn = "Source";
    lookup.keys      = {sourceId};

    const CdsTap::Response resp = CdsTap::lookupVizier(_network, lookup, 30000);
    if (!resp.ok()) {
        err = resp.error;
        return false;
    }

    const DelimitedTable::Csv csv = DelimitedTable::parseCsv(resp.body);
    if (csv.isEmpty()) {
        err = "no Gaia DR3 record";
        return false;
    }

    out.ra      = csv.number(0, "ra_icrs");
    out.dec     = csv.number(0, "de_icrs");
    out.pmra    = csv.number(0, "pmra");
    out.pmdec   = csv.number(0, "pmde");
    out.e_pmra  = csv.number(0, "e_pmra");
    out.e_pmdec = csv.number(0, "e_pmde");
    out.plx     = csv.number(0, "plx");
    out.e_plx   = csv.number(0, "e_plx");
    out.gmag    = csv.number(0, "gmag");
    out.bp      = csv.number(0, "bpmag");
    out.rp      = csv.number(0, "rpmag");

    static constexpr double kPogson = 2.5 / 2.302585092994046;
    auto magErr = [&](const char *f, const char *eF) -> double {
        double F   = csv.number(0, QString::fromLatin1(f));
        double eFv = csv.number(0, QString::fromLatin1(eF));
        if (std::isnan(F) || std::isnan(eFv) || F <= 0.0 || eFv <= 0.0)
            return std::numeric_limits<double>::quiet_NaN();
        return kPogson * (eFv / F);
    };
    out.e_gmag = magErr("fg", "e_fg");
    out.e_bp   = magErr("fbp", "e_fbp");
    out.e_rp   = magErr("frp", "e_frp");

    out.pmra_pmdec_corr = csv.number(0, "pmrapmdecor");
    out.plx_pmra_corr   = csv.number(0, "plxpmracor");
    out.plx_pmdec_corr  = csv.number(0, "plxpmdecor");

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
    // SIMBAD TAP on CDS or the CfA mirror. Nothing else holds the
    // bibliography, so without SIMBAD the old list is kept.
    const StarResolver::Outcome o = StarResolver::simbadBibcodes(
        _network, QStringLiteral("Gaia DR3 %1").arg(sourceId), out);
    if (!o.ok)
        err = o.error;
    return o.ok; // an empty result is valid (object simply has no refs)
}