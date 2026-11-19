#include "catalog/ui/AddStarDialog.h"
#include "core/SkyGeometry.h"
#include "core/Star.h"
#include "catalog/CdsTapClient.h"
#include "catalog/StarResolver.h"
#include "app/Logger.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QProgressBar>
#include <QDoubleValidator>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QHttpMultiPart>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <QLocale>

#include <cmath>

// ------------------------------------------------------------------
// Construction & UI
// ------------------------------------------------------------------

AddStarDialog::AddStarDialog(QWidget* parent)
    : QDialog(parent)
    , _network(new QNetworkAccessManager(this))
{
    setWindowTitle("Add Star");
    setMinimumWidth(720);
    setupUi();
}

AddStarDialog::~AddStarDialog() = default;

static QLineEdit* makeNumberEdit(QWidget* parent)
{
    auto* le = new QLineEdit(parent);
    auto* v = new QDoubleValidator(le);
    v->setNotation(QDoubleValidator::ScientificNotation);
    v->setLocale(QLocale::c());
    le->setValidator(v);
    le->setPlaceholderText("-");
    return le;
}

void AddStarDialog::setupUi()
{
    auto* root = new QVBoxLayout(this);

    // --------- Identifiers + Resolve bar ---------
    auto* idBox  = new QGroupBox("Identifiers", this);
    auto* idGrid = new QGridLayout(idBox);

    _aliasEdit    = new QLineEdit(idBox);
    _sourceIdEdit = new QLineEdit(idBox);
    _ticEdit      = new QLineEdit(idBox);
    _jnameEdit    = new QLineEdit(idBox);
    _raEdit       = makeNumberEdit(idBox);
    _decEdit      = makeNumberEdit(idBox);

    _aliasEdit->setPlaceholderText("Free-form alias (e.g. PG 1232-136)");
    _sourceIdEdit->setPlaceholderText("Gaia DR3 source_id (numeric)");
    _ticEdit->setPlaceholderText("TIC ID (numeric)");
    _jnameEdit->setPlaceholderText("Jhhmmss.ss±ddmmss.s");
    _raEdit->setPlaceholderText("RA [deg]");
    _decEdit->setPlaceholderText("Dec [deg]");

    idGrid->addWidget(new QLabel("Alias:"),         0, 0);
    idGrid->addWidget(_aliasEdit,                   0, 1, 1, 3);
    idGrid->addWidget(new QLabel("Gaia DR3 ID:"),   1, 0);
    idGrid->addWidget(_sourceIdEdit,                1, 1);
    idGrid->addWidget(new QLabel("TIC:"),           1, 2);
    idGrid->addWidget(_ticEdit,                     1, 3);
    idGrid->addWidget(new QLabel("JName:"),         2, 0);
    idGrid->addWidget(_jnameEdit,                   2, 1, 1, 3);
    idGrid->addWidget(new QLabel("RA [deg]:"),      3, 0);
    idGrid->addWidget(_raEdit,                      3, 1);
    idGrid->addWidget(new QLabel("Dec [deg]:"),     3, 2);
    idGrid->addWidget(_decEdit,                     3, 3);

    // Resolve row
    auto* resolveRow = new QHBoxLayout();
    _resolveBtn = new QPushButton("Resolve", this);
    _resolveBtn->setToolTip(
        "Use whichever identifiers/coords are filled in to find the star: "
        "SIMBAD (CDS or its CfA mirror) for cross-IDs and coordinates, and "
        "when SIMBAD is unreachable the MAST/NED/Sesame name resolvers, a "
        "Gaia DR3 cone search and the TIC at MAST. Gaia DR3 astrometry and "
        "photometry come from VizieR or the ESA Gaia archive. Only empty "
        "fields are filled in.");

    _bibliographyCheck = new QCheckBox("Query bibliography (SIMBAD)", this);
    _bibliographyCheck->setChecked(true);
    _bibliographyCheck->setToolTip(
        "After adding the star, queue a SIMBAD bibliography query in the "
        "background to populate the star's bibcode list.");

    _busyIndicator = new QProgressBar(this);
    _busyIndicator->setRange(0, 0);     // indeterminate
    _busyIndicator->setMaximumWidth(120);
    _busyIndicator->setVisible(false);

    _statusLabel = new QLabel(this);
    _statusLabel->setWordWrap(true);

    resolveRow->addWidget(_resolveBtn);
    resolveRow->addWidget(_bibliographyCheck);
    resolveRow->addWidget(_busyIndicator);
    resolveRow->addWidget(_statusLabel, 1);

    connect(_resolveBtn, &QPushButton::clicked, this, &AddStarDialog::onResolve);

    // --------- Astrometry ---------
    auto* astroBox = new QGroupBox("Astrometry (Gaia DR3)", this);
    auto* astroGrid = new QGridLayout(astroBox);
    _pmraEdit            = makeNumberEdit(astroBox);
    _ePmraEdit           = makeNumberEdit(astroBox);
    _pmdecEdit           = makeNumberEdit(astroBox);
    _ePmdecEdit          = makeNumberEdit(astroBox);
    _plxEdit             = makeNumberEdit(astroBox);
    _ePlxEdit            = makeNumberEdit(astroBox);
    _pmraPmdecCorrEdit   = makeNumberEdit(astroBox);
    _plxPmraCorrEdit     = makeNumberEdit(astroBox);
    _plxPmdecCorrEdit    = makeNumberEdit(astroBox);

    int r = 0;
    astroGrid->addWidget(new QLabel("pmRA [mas/yr]:"),   r, 0); astroGrid->addWidget(_pmraEdit,  r, 1);
    astroGrid->addWidget(new QLabel("e_pmRA:"),          r, 2); astroGrid->addWidget(_ePmraEdit, r, 3);
    ++r;
    astroGrid->addWidget(new QLabel("pmDec [mas/yr]:"),  r, 0); astroGrid->addWidget(_pmdecEdit, r, 1);
    astroGrid->addWidget(new QLabel("e_pmDec:"),         r, 2); astroGrid->addWidget(_ePmdecEdit,r, 3);
    ++r;
    astroGrid->addWidget(new QLabel("Parallax [mas]:"),  r, 0); astroGrid->addWidget(_plxEdit,   r, 1);
    astroGrid->addWidget(new QLabel("e_plx:"),           r, 2); astroGrid->addWidget(_ePlxEdit,  r, 3);
    ++r;
    astroGrid->addWidget(new QLabel("corr(pmRA,pmDec):"),r, 0); astroGrid->addWidget(_pmraPmdecCorrEdit, r, 1);
    astroGrid->addWidget(new QLabel("corr(plx,pmRA):"),  r, 2); astroGrid->addWidget(_plxPmraCorrEdit,   r, 3);
    ++r;
    astroGrid->addWidget(new QLabel("corr(plx,pmDec):"), r, 0); astroGrid->addWidget(_plxPmdecCorrEdit,  r, 1);

    // --------- Photometry ---------
    auto* photBox = new QGroupBox("Gaia Photometry", this);
    auto* photGrid = new QGridLayout(photBox);
    _gmagEdit  = makeNumberEdit(photBox);
    _eGmagEdit = makeNumberEdit(photBox);
    _bpEdit    = makeNumberEdit(photBox);
    _eBpEdit   = makeNumberEdit(photBox);
    _rpEdit    = makeNumberEdit(photBox);
    _eRpEdit   = makeNumberEdit(photBox);
    _bpRpEdit  = makeNumberEdit(photBox);

    r = 0;
    photGrid->addWidget(new QLabel("G [mag]:"),  r, 0); photGrid->addWidget(_gmagEdit,  r, 1);
    photGrid->addWidget(new QLabel("e_G:"),      r, 2); photGrid->addWidget(_eGmagEdit, r, 3);
    ++r;
    photGrid->addWidget(new QLabel("BP [mag]:"), r, 0); photGrid->addWidget(_bpEdit,    r, 1);
    photGrid->addWidget(new QLabel("e_BP:"),     r, 2); photGrid->addWidget(_eBpEdit,   r, 3);
    ++r;
    photGrid->addWidget(new QLabel("RP [mag]:"), r, 0); photGrid->addWidget(_rpEdit,    r, 1);
    photGrid->addWidget(new QLabel("e_RP:"),     r, 2); photGrid->addWidget(_eRpEdit,   r, 3);
    ++r;
    photGrid->addWidget(new QLabel("BP-RP:"),    r, 0); photGrid->addWidget(_bpRpEdit,  r, 1);

    // --------- Spectroscopy ---------
    auto* specBox = new QGroupBox("Spectroscopy", this);
    auto* specGrid = new QGridLayout(specBox);
    _specClassEdit = new QLineEdit(specBox);
    _specClassEdit->setPlaceholderText("e.g. sdB, He-sdO, ...");
    _teffEdit  = makeNumberEdit(specBox);
    _eTeffEdit = makeNumberEdit(specBox);
    _loggEdit  = makeNumberEdit(specBox);
    _eLoggEdit = makeNumberEdit(specBox);
    _heEdit    = makeNumberEdit(specBox);
    _eHeEdit   = makeNumberEdit(specBox);

    r = 0;
    specGrid->addWidget(new QLabel("Spec class:"), r, 0);
    specGrid->addWidget(_specClassEdit,            r, 1, 1, 3);
    ++r;
    specGrid->addWidget(new QLabel("Teff [K]:"),   r, 0); specGrid->addWidget(_teffEdit,  r, 1);
    specGrid->addWidget(new QLabel("e_Teff:"),     r, 2); specGrid->addWidget(_eTeffEdit, r, 3);
    ++r;
    specGrid->addWidget(new QLabel("log g:"),      r, 0); specGrid->addWidget(_loggEdit,  r, 1);
    specGrid->addWidget(new QLabel("e_log g:"),    r, 2); specGrid->addWidget(_eLoggEdit, r, 3);
    ++r;
    specGrid->addWidget(new QLabel("log(He/H):"),  r, 0); specGrid->addWidget(_heEdit,    r, 1);
    specGrid->addWidget(new QLabel("e_He:"),       r, 2); specGrid->addWidget(_eHeEdit,   r, 3);

    // --------- Buttons ---------
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Add Star");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // --------- Assemble ---------
    root->addWidget(idBox);
    root->addLayout(resolveRow);

    auto* twoCol = new QHBoxLayout();
    twoCol->addWidget(astroBox);
    twoCol->addWidget(photBox);
    root->addLayout(twoCol);

    root->addWidget(specBox);
    root->addWidget(buttons);
}

// ------------------------------------------------------------------
// Status helpers
// ------------------------------------------------------------------

void AddStarDialog::setStatus(const QString& msg, bool isError)
{
    _statusLabel->setText(msg);
    _statusLabel->setStyleSheet(isError ? "color: #c0392b;" : "");
}

void AddStarDialog::setBusy(bool busy)
{
    _busyIndicator->setVisible(busy);
    _resolveBtn->setEnabled(!busy);
    if (busy)
        QApplication::setOverrideCursor(Qt::BusyCursor);
    else
        QApplication::restoreOverrideCursor();
}

// ------------------------------------------------------------------
// Static parsing helpers
// ------------------------------------------------------------------

double AddStarDialog::parseDoubleOrNaN(QLineEdit* le)
{
    const QString s = le->text().trimmed();
    if (s.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    double v = QLocale::c().toDouble(s, &ok);
    if (!ok) v = s.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

void AddStarDialog::setDoubleIfEmpty(QLineEdit* le, double v)
{
    if (!le->text().trimmed().isEmpty()) return;
    if (std::isnan(v) || v == 0.0) return;
    le->setText(QString::number(v, 'g', 10));
}

void AddStarDialog::setTextIfEmpty(QLineEdit* le, const QString& v)
{
    if (!le->text().trimmed().isEmpty()) return;
    if (v.trimmed().isEmpty()) return;
    le->setText(v.trimmed());
}

QString AddStarDialog::digitsOnly(const QString& s)
{
    QRegularExpression re("(\\d{6,})");
    auto m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString AddStarDialog::jnameFromCoords(double raDeg, double decDeg)
{
    return SkyGeometry::jnameFromCoords(raDeg, decDeg);
}

bool AddStarDialog::parseJnameToCoords(const QString& jname, double& raDeg, double& decDeg)
{
    // Accepts e.g. J123456.78+123456.7 (seconds may have variable precision).
    QRegularExpression re(
        "^[Jj]?\\s*"
        "(\\d{2})(\\d{2})(\\d{2}(?:\\.\\d+)?)"
        "([+\\-])"
        "(\\d{2})(\\d{2})(\\d{2}(?:\\.\\d+)?)");
    auto m = re.match(jname.trimmed());
    if (!m.hasMatch()) return false;
    double rh = m.captured(1).toDouble();
    double rm = m.captured(2).toDouble();
    double rs = m.captured(3).toDouble();
    QChar  sg = m.captured(4)[0];
    double dd = m.captured(5).toDouble();
    double dm = m.captured(6).toDouble();
    double ds = m.captured(7).toDouble();

    raDeg  = (rh + rm/60.0 + rs/3600.0) * 15.0;
    decDeg = (dd + dm/60.0 + ds/3600.0) * (sg == '-' ? -1.0 : 1.0);
    return true;
}

// ------------------------------------------------------------------
// Resolve
// ------------------------------------------------------------------

void AddStarDialog::onResolve()
{
    setBusy(true);
    setStatus("Resolving...");
    QApplication::processEvents();          // let the busy bar paint

    LOG_INFO("AddStarDialog", "Resolve clicked");

    const QString gaiaIdField = _sourceIdEdit->text().trimmed();
    const QString ticField    = _ticEdit->text().trimmed();
    const QString jnameField  = _jnameEdit->text().trimmed();
    const QString aliasField  = _aliasEdit->text().trimmed();
    double        ra          = parseDoubleOrNaN(_raEdit);
    double        dec         = parseDoubleOrNaN(_decEdit);

    // Radius for matching the position to a Gaia source / TIC entry. A
    // J-designation only fixes a bin, so it gets the bin's size.
    double matchRadius = StarResolver::kDefaultMatchArcsec;

    // Derive coords from JName up-front (so we have a fallback path).
    if (!jnameField.isEmpty() && (std::isnan(ra) || std::isnan(dec))) {
        double jRa, jDec, jRadius;
        if (StarResolver::positionFromDesignation(jnameField, jRa, jDec, jRadius)) {
            matchRadius = jRadius;
        } else if (!parseJnameToCoords(jnameField, jRa, jDec)) {
            jRa = jDec = std::numeric_limits<double>::quiet_NaN();
        }
        if (!std::isnan(jRa) && !std::isnan(jDec)) {
            if (std::isnan(ra))  { ra  = jRa;  setDoubleIfEmpty(_raEdit,  jRa); }
            if (std::isnan(dec)) { dec = jDec; setDoubleIfEmpty(_decEdit, jDec); }
        }
    }

    QString simbadQuery;
    if (!gaiaIdField.isEmpty()) {
        QString num = digitsOnly(gaiaIdField);
        simbadQuery = QString("query id Gaia DR3 %1").arg(num.isEmpty() ? gaiaIdField : num);
    } else if (!ticField.isEmpty()) {
        QString num = digitsOnly(ticField);
        simbadQuery = QString("query id TIC %1").arg(num.isEmpty() ? ticField : num);
    } else if (!jnameField.isEmpty()) {
        simbadQuery = QString("query id %1").arg(jnameField);
    } else if (!std::isnan(ra) && !std::isnan(dec)) {
        // Use a generous radius and ICRS J2000 explicitly so SIMBAD parses
        // the values the way the user typed them.
        simbadQuery = QString("query coo %1 %2 radius=30s "
                              "frame=ICRS epoch=J2000 equinox=2000")
                          .arg(ra,  0, 'f', 8)
                          .arg(dec, 0, 'f', 8);
    } else if (!aliasField.isEmpty()) {
        simbadQuery = QString("query id %1").arg(aliasField);
    } else {
        setBusy(false);
        setStatus("Provide a Gaia ID, TIC, JName, alias, or RA/Dec first.", true);
        LOG_WARNING("AddStarDialog", "Resolve aborted: no input identifiers");
        return;
    }

    LOG_INFO("AddStarDialog", QString("SIMBAD query: %1").arg(simbadQuery));

    QStringList sources;    // services that contributed, for the status line
    QStringList problems;   // what could not be filled, and why

    StarResolver::SimbadIds ids;
    const StarResolver::Outcome simbad =
        StarResolver::simbadIdentify(_network, simbadQuery, ids);
    if (simbad.ok) {
        LOG_INFO("AddStarDialog",
            QString("SIMBAD ok: main='%1' gaia='%2' tic='%3' ra=%4 dec=%5")
                .arg(ids.mainId, ids.sourceId, ids.tic)
                .arg(ids.ra).arg(ids.dec));
        sources << simbad.service;
        setTextIfEmpty(_sourceIdEdit, ids.sourceId);
        setTextIfEmpty(_ticEdit,      ids.tic);
        setTextIfEmpty(_aliasEdit,    ids.mainId);
        setDoubleIfEmpty(_raEdit,     ids.ra);
        setDoubleIfEmpty(_decEdit,    ids.dec);
    } else {
        LOG_WARNING("AddStarDialog", QString("SIMBAD failed: %1").arg(simbad.error));
        problems << QString("SIMBAD %1").arg(simbad.unavailable ? "unreachable" : simbad.error);
    }
    setStatus("Resolving without SIMBAD...");
    QApplication::processEvents();

    // --- Position, when nothing has given one yet -------------------------
    // A Gaia id needs none: the Gaia record below brings it.
    ra  = parseDoubleOrNaN(_raEdit);
    dec = parseDoubleOrNaN(_decEdit);
    const bool haveGaiaId = !_sourceIdEdit->text().trimmed().isEmpty();
    if ((std::isnan(ra) || std::isnan(dec)) && !haveGaiaId) {
        if (!ticField.isEmpty()) {
            StarResolver::TicEntry t;
            const auto o = StarResolver::ticById(_network, ticField, t);
            if (o.ok) {
                ra = t.ra; dec = t.dec;
                sources << o.service;
            } else {
                problems << QString("TIC lookup: %1").arg(o.error);
            }
        }
        if ((std::isnan(ra) || std::isnan(dec)) && !aliasField.isEmpty()) {
            double r = 0, d = 0, rad = 0;
            if (StarResolver::positionFromDesignation(aliasField, r, d, rad)) {
                ra = r; dec = d; matchRadius = rad;
                sources << "position from the name";
            } else {
                StarResolver::NamedPosition p;
                const auto o = StarResolver::resolveName(_network, aliasField, p);
                if (o.ok) {
                    ra = p.ra; dec = p.dec;
                    sources << o.service;
                } else {
                    problems << o.error;
                }
            }
        }
        setDoubleIfEmpty(_raEdit,  ra);
        setDoubleIfEmpty(_decEdit, dec);
    }

    // --- Gaia DR3 source at that position ---------------------------------
    if (_sourceIdEdit->text().trimmed().isEmpty() && !std::isnan(ra) && !std::isnan(dec)) {
        std::vector<StarResolver::GaiaSource> found;
        const auto o = StarResolver::gaiaCone(_network, ra, dec, matchRadius, found);
        if (o.ok && !found.empty()) {
            const auto& g = found.front();
            _sourceIdEdit->setText(g.sourceId);
            sources << QString("Gaia DR3 match at %1\" (%2%3)")
                           .arg(g.sepArcsec, 0, 'f', 2)
                           .arg(o.service)
                           .arg(found.size() > 1
                                    ? QString(", %1 sources within %2\", nearest taken")
                                          .arg(found.size()).arg(matchRadius, 0, 'f', 1)
                                    : QString());
        } else if (o.ok) {
            problems << QString("no Gaia DR3 source within %1\"").arg(matchRadius, 0, 'f', 1);
        } else {
            problems << QString("Gaia cone search: %1").arg(o.error);
        }
    }

    // --- Gaia DR3 record ---------------------------------------------------
    QString sourceIdForGaia = digitsOnly(_sourceIdEdit->text().trimmed());
    if (sourceIdForGaia.isEmpty())
        sourceIdForGaia = _sourceIdEdit->text().trimmed();

    if (!sourceIdForGaia.isEmpty()) {
        setStatus("Fetching Gaia DR3...");
        QApplication::processEvents();
        QString gaiaErr, via;
        LOG_INFO("AddStarDialog",
            QString("Querying Gaia DR3 for source_id=%1").arg(sourceIdForGaia));
        if (fetchGaiaDR3(sourceIdForGaia, gaiaErr, via)) {
            sources << QString("Gaia DR3 (%1)").arg(via);
        } else {
            LOG_WARNING("AddStarDialog", QString("Gaia DR3 failed: %1").arg(gaiaErr));
            problems << QString("Gaia DR3: %1").arg(gaiaErr);
        }
    }

    // --- TIC at the (now best) position -----------------------------------
    ra  = parseDoubleOrNaN(_raEdit);
    dec = parseDoubleOrNaN(_decEdit);
    if (_ticEdit->text().trimmed().isEmpty() && !std::isnan(ra) && !std::isnan(dec)) {
        StarResolver::TicEntry t;
        const auto o = StarResolver::ticCone(_network, ra, dec,
                                             StarResolver::kDefaultMatchArcsec, t);
        if (o.ok) {
            _ticEdit->setText(t.tic);
            sources << QString("TIC match at %1\" (MAST)").arg(t.sepArcsec, 0, 'f', 2);
        } else {
            problems << QString("TIC: %1").arg(o.error);
        }
    }

    // Synthesise JName if still empty.
    if (_jnameEdit->text().trimmed().isEmpty() && !std::isnan(ra) && !std::isnan(dec))
        _jnameEdit->setText(jnameFromCoords(ra, dec));

    const bool resolvedAnything =
        !_sourceIdEdit->text().trimmed().isEmpty() || (!std::isnan(ra) && !std::isnan(dec));
    QString msg = sources.isEmpty() ? QString("Nothing resolved.")
                                    : QString("Filled from: %1.").arg(sources.join("; "));
    if (!problems.isEmpty())
        msg += QString(" Not available: %1.").arg(problems.join("; "));
    setStatus(msg, !resolvedAnything);
    setBusy(false);
}

// ------------------------------------------------------------------
// VizieR / Gaia DR3
// ------------------------------------------------------------------

bool AddStarDialog::fetchGaiaDR3(const QString& sourceId, QString& err, QString& via)
{
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
    via = resp.viaGaiaArchive ? QStringLiteral("ESA Gaia archive, VizieR down")
        : resp.viaAsu         ? QStringLiteral("VizieR ASU, TAP down")
                              : QStringLiteral("VizieR");

    QString body = QString::fromUtf8(resp.body);

    LOG_DEBUG("AddStarDialog",
    QString("VizieR response (%1 bytes):\n%2")
        .arg(body.size()).arg(body.left(1500)));

    QStringList lines = body.split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 2) {
        err = "no Gaia DR3 record";
        LOG_WARNING("AddStarDialog",
            QString("VizieR returned no row for source_id=%1; body:\n%2")
                .arg(sourceId).arg(body));
        return false;
    }

    QStringList headers = lines[0].split(',');
    QMap<QString, int> idx;
    for (int i = 0; i < headers.size(); ++i)
        idx[headers[i].trimmed().toLower().remove('"')] = i;

    QStringList values = lines[1].split(',');

    auto getD = [&](const QString& col) -> double {
        int i = idx.value(col.toLower(), -1);
        if (i < 0 || i >= values.size()) return std::numeric_limits<double>::quiet_NaN();
        QString s = values[i].trimmed().remove('"');
        if (s.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
        bool ok; double v = s.toDouble(&ok);
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };

    setDoubleIfEmpty(_raEdit,             getD("ra_icrs"));
    setDoubleIfEmpty(_decEdit,            getD("de_icrs"));
    setDoubleIfEmpty(_pmraEdit,           getD("pmra"));
    setDoubleIfEmpty(_pmdecEdit,          getD("pmde"));
    setDoubleIfEmpty(_ePmraEdit,          getD("e_pmra"));
    setDoubleIfEmpty(_ePmdecEdit,         getD("e_pmde"));
    setDoubleIfEmpty(_plxEdit,            getD("plx"));
    setDoubleIfEmpty(_ePlxEdit,           getD("e_plx"));
    setDoubleIfEmpty(_gmagEdit,           getD("gmag"));
    setDoubleIfEmpty(_bpEdit,             getD("bpmag"));
    setDoubleIfEmpty(_rpEdit,             getD("rpmag"));

    // Gaia's VizieR mirror exposes only fluxes + flux errors. Derive the
    // magnitude uncertainties: sigma_mag ~ 1.0857 * (sigma_F / F).
    static constexpr double kPogson = 2.5 / 2.302585092994046; // ~1.0857362

    auto magErrFromFlux = [&](const char* fluxCol, const char* eFluxCol) -> double {
        double F   = getD(fluxCol);
        double eF  = getD(eFluxCol);
        if (std::isnan(F) || std::isnan(eF) || F <= 0.0 || eF <= 0.0)
            return std::numeric_limits<double>::quiet_NaN();
        return kPogson * (eF / F);
    };

    setDoubleIfEmpty(_eGmagEdit, magErrFromFlux("fg",  "e_fg"));
    setDoubleIfEmpty(_eBpEdit,   magErrFromFlux("fbp", "e_fbp"));
    setDoubleIfEmpty(_eRpEdit,   magErrFromFlux("frp", "e_frp"));
    
    LOG_DEBUG("AddStarDialog",
    QString("Gaia flux→mag errors: e_G=%1 e_BP=%2 e_RP=%3")
        .arg(parseDoubleOrNaN(_eGmagEdit))
        .arg(parseDoubleOrNaN(_eBpEdit))
        .arg(parseDoubleOrNaN(_eRpEdit)));

    setDoubleIfEmpty(_pmraPmdecCorrEdit,  getD("pmrapmdecor"));
    setDoubleIfEmpty(_plxPmraCorrEdit,    getD("plxpmracor"));
    setDoubleIfEmpty(_plxPmdecCorrEdit,   getD("plxpmdecor"));

    // Derived BP-RP if both ends are present and field empty.
    if (_bpRpEdit->text().trimmed().isEmpty()) {
        double bp = parseDoubleOrNaN(_bpEdit);
        double rp = parseDoubleOrNaN(_rpEdit);
        if (!std::isnan(bp) && !std::isnan(rp))
            _bpRpEdit->setText(QString::number(bp - rp, 'g', 8));
    }
    LOG_INFO("AddStarDialog",
        QString("VizieR filled fields for source_id=%1").arg(sourceId));
    return true;
}

// ------------------------------------------------------------------
// buildStar
// ------------------------------------------------------------------

std::shared_ptr<Star> AddStarDialog::buildStar() const
{
    auto star = std::make_shared<Star>();

    // Identifiers
    star->setAlias(_aliasEdit->text().trimmed());
    QString sid = _sourceIdEdit->text().trimmed();
    if (!sid.isEmpty()) {
        QString num = digitsOnly(sid);
        star->setSourceId(num.isEmpty() ? sid : num);
    }
    QString tic = _ticEdit->text().trimmed();
    if (!tic.isEmpty()) {
        QString num = digitsOnly(tic);
        star->setTic(num.isEmpty() ? tic : num);
    }
    star->setJName(_jnameEdit->text().trimmed());

    // Astrometry
    star->setRa(parseDoubleOrNaN(_raEdit));
    star->setDec(parseDoubleOrNaN(_decEdit));
    star->setPmra(parseDoubleOrNaN(_pmraEdit));
    star->setPmdec(parseDoubleOrNaN(_pmdecEdit));
    star->setEPmra(parseDoubleOrNaN(_ePmraEdit));
    star->setEPmdec(parseDoubleOrNaN(_ePmdecEdit));
    star->setPlx(parseDoubleOrNaN(_plxEdit));
    star->setEPlx(parseDoubleOrNaN(_ePlxEdit));
    star->setPmraPmdecCorr(parseDoubleOrNaN(_pmraPmdecCorrEdit));
    star->setPlxPmraCorr(parseDoubleOrNaN(_plxPmraCorrEdit));
    star->setPlxPmdecCorr(parseDoubleOrNaN(_plxPmdecCorrEdit));

    // Photometry
    star->setGmag(parseDoubleOrNaN(_gmagEdit));
    star->setEGmag(parseDoubleOrNaN(_eGmagEdit));
    star->setBp(parseDoubleOrNaN(_bpEdit));
    star->setEBp(parseDoubleOrNaN(_eBpEdit));
    star->setRp(parseDoubleOrNaN(_rpEdit));
    star->setERp(parseDoubleOrNaN(_eRpEdit));

    double bpRp = parseDoubleOrNaN(_bpRpEdit);
    if (std::isnan(bpRp)) {
        double bp = parseDoubleOrNaN(_bpEdit);
        double rp = parseDoubleOrNaN(_rpEdit);
        if (!std::isnan(bp) && !std::isnan(rp)) bpRp = bp - rp;
    }
    star->setBpRp(bpRp);

    // Spectroscopy
    star->setSpecClass(_specClassEdit->text().trimmed());
    star->setTeff(parseDoubleOrNaN(_teffEdit));
    star->setETeff(parseDoubleOrNaN(_eTeffEdit));
    star->setLogg(parseDoubleOrNaN(_loggEdit));
    star->setELogg(parseDoubleOrNaN(_eLoggEdit));
    star->setHe(parseDoubleOrNaN(_heEdit));
    star->setEHe(parseDoubleOrNaN(_eHeEdit));

    return star;
}

bool AddStarDialog::shouldQueryBibliography() const
{
    return _bibliographyCheck->isChecked();
}