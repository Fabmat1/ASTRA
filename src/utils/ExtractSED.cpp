#include "ExtractSED.h"
#include "models/Photometry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>
#include <cmath>
#include <limits>

// ══════════════════════════════════════════════════════════════
// Internal helpers
// ══════════════════════════════════════════════════════════════

namespace {

// ── Whitespace-delimited tokenizer that respects quoted strings ─
QStringList tokenizeLine(const QString& line)
{
    return line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}

// ── Parse photometry_fit_mag.txt ─────────────────────────────
bool parseObservedPhotometry(const QString& filepath,
                             std::vector<SEDPhotometryPoint>& points)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    bool headerSkipped = false;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        // Skip header line(s) — detect by first token being non-numeric
        if (!headerSkipped) {
            bool ok;
            tokenizeLine(line).first().toDouble(&ok);
            if (!ok) {
                headerSkipped = true;
                continue;
            }
            headerSkipped = true;
        }

        QStringList tok = tokenizeLine(line);
        if (tok.size() < 12) continue;

        SEDPhotometryPoint p;
        p.lambdaMin    = tok[0].toDouble();
        p.lambda       = tok[1].toDouble();
        p.lambdaMax    = tok[2].toDouble();
        p.fluxMin      = tok[3].toDouble();
        p.flux         = tok[4].toDouble();
        p.fluxMax      = tok[5].toDouble();
        p.diff         = tok[6].toDouble();
        p.diffErr      = tok[7].toDouble();
        p.passband     = tok[8];
        p.system       = tok[9];
        p.flag         = tok[10].toInt();
        p.vizierCatalog = tok[11];

        points.push_back(p);
    }
    return !points.empty();
}

// ── Parse photometry_fit.txt ─────────────────────────────────
bool parseModelCurve(const QString& filepath,
                     std::vector<double>& wavelengths,
                     std::vector<double>& totalFlux,
                     std::vector<std::vector<double>>& compFluxes,
                     int& numComponents)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);

    // Read header to determine column count
    QString headerLine;
    while (!in.atEnd()) {
        headerLine = in.readLine().trimmed();
        if (!headerLine.isEmpty()) break;
    }

    QStringList headerTok = tokenizeLine(headerLine);

    // Determine number of columns: "l f" = 1 comp, "l f f_c1 f_c2" = 2 comp
    int numCols = headerTok.size();
    if (numCols >= 4) {
        numComponents = numCols - 2;   // l, f, f_c1, f_c2, ...
    } else {
        numComponents = 1;             // l, f only
    }

    compFluxes.resize(numComponents);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList tok = tokenizeLine(line);
        if (tok.size() < 2) continue;

        bool ok1, ok2;
        double wl = tok[0].toDouble(&ok1);
        double fl = tok[1].toDouble(&ok2);
        if (!ok1 || !ok2) continue;

        wavelengths.push_back(wl);
        totalFlux.push_back(fl);

        for (int c = 0; c < numComponents; ++c) {
            int col = 2 + c;
            if (col < tok.size()) {
                compFluxes[c].push_back(tok[col].toDouble());
            } else {
                // If 1-component and we have no f_c1 column, mirror the total
                compFluxes[c].push_back(fl);
            }
        }
    }

    return !wavelengths.empty();
}

// ── Parse photometry_results_stellar_c*.txt ──────────────────
bool parseStellarComponent(const QString& filepath, SEDComponentParams& comp)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);

    // Skip header
    QString header = in.readLine();

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList tok = tokenizeLine(line);
        if (tok.size() < 7) continue;

        QString name = tok[0];
        double value     = tok[1].toDouble();
        double confMin   = tok[2].toDouble();
        double confMax   = tok[3].toDouble();
        double medValue  = tok[4].toDouble();
        double medConfMin = tok[5].toDouble();
        double medConfMax = tok[6].toDouble();

        // Build AsymmetricValue for mode and median
        auto makeAV = [](double val, double cMin, double cMax) -> AsymmetricValue {
            return { val, cMax - val, val - cMin };
        };

        if (name.endsWith("_R")) {
            comp.radius       = makeAV(value,    confMin,    confMax);
            comp.radiusMedian = makeAV(medValue, medConfMin, medConfMax);
        } else if (name.endsWith("_M")) {
            comp.mass       = makeAV(value,    confMin,    confMax);
            comp.massMedian = makeAV(medValue, medConfMin, medConfMax);
        } else if (name.endsWith("_L")) {
            comp.luminosity       = makeAV(value,    confMin,    confMax);
            comp.luminosityMedian = makeAV(medValue, medConfMin, medConfMax);
        }
    }
    return true;
}

// ── LaTeX number parser ──────────────────────────────────────
// Handles:  "30200", "0.0578", "3.02",
//           "$0.314^{+0.017}_{-0.016}$",
//           "$0.738 \\pm 0.029$",
//           "$\\left(1.35 \\pm 0.06\\right)\\times10^{3}$"
struct ParsedTexValue {
    double value      = 0.0;
    double errUp      = 0.0;
    double errDown    = 0.0;
    bool   hasError   = false;
    bool   valid      = false;
};

ParsedTexValue parseTexValue(const QString& raw)
{
    ParsedTexValue r;
    QString s = raw.trimmed();

    // Strip surrounding $...$
    if (s.startsWith('$')) s = s.mid(1);
    if (s.endsWith('$'))  s.chop(1);

    // Remove common LaTeX wrappers
    s.remove("\\left(");
    s.remove("\\right)");
    s.remove("\\,");
    s.remove("\\text{");
    s.remove('}');

    // Handle \times10^{N} scientific notation
    double multiplier = 1.0;
    static QRegularExpression sciRe(R"(\\times\s*10\^\{?\s*([+-]?\d+)\s*\}?)");
    QRegularExpressionMatch sciMatch = sciRe.match(s);
    if (sciMatch.hasMatch()) {
        multiplier = std::pow(10.0, sciMatch.captured(1).toDouble());
        s = s.left(sciMatch.capturedStart()).trimmed();
    }

    // Try asymmetric: value^{+upper}_{-lower}
    static QRegularExpression asymRe(
        R"(([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*\^\{?\s*\+\s*([\d.]+(?:[eE][+-]?\d+)?)\s*\}?\s*_\{?\s*-\s*([\d.]+(?:[eE][+-]?\d+)?)\s*\}?)");
    QRegularExpressionMatch asymMatch = asymRe.match(s);
    if (asymMatch.hasMatch()) {
        r.value   = asymMatch.captured(1).toDouble() * multiplier;
        r.errUp   = asymMatch.captured(2).toDouble() * multiplier;
        r.errDown = asymMatch.captured(3).toDouble() * multiplier;
        r.hasError = true;
        r.valid    = true;
        return r;
    }

    // Try symmetric: value \pm error
    static QRegularExpression symRe(
        R"(([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*\\pm\s*([\d.]+(?:[eE][+-]?\d+)?))");
    QRegularExpressionMatch symMatch = symRe.match(s);
    if (symMatch.hasMatch()) {
        r.value   = symMatch.captured(1).toDouble() * multiplier;
        r.errUp   = symMatch.captured(2).toDouble() * multiplier;
        r.errDown = r.errUp;
        r.hasError = true;
        r.valid    = true;
        return r;
    }

    // Plain number
    bool ok;
    double v = s.toDouble(&ok);
    if (ok) {
        r.value = v * multiplier;
        r.valid = true;
    }

    return r;
}

// ── Determine parameter status from TeX label ────────────────
SEDParamStatus parseStatus(const QString& line)
{
    if (line.contains("(fixed)"))      return SEDParamStatus::Fixed;
    if (line.contains("(prescribed)")) return SEDParamStatus::Prescribed;
    return SEDParamStatus::Fitted;
}

// ── Parse photometry_results.tex ─────────────────────────────
bool parseTexResults(const QString& filepath, std::shared_ptr<SEDModel> model)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    QString content = in.readAll();

    // Extract object name from "Object: XXX"
    {
        static QRegularExpression objRe(R"(Object:\s*(.+?)\s*&)");
        QRegularExpressionMatch m = objRe.match(content);
        if (m.hasMatch())
            model->objectName = m.captured(1).trimmed();
    }

    // Split into lines and process each row of the tabular

    static QRegularExpression rowRe(R"((.+?)\s*&\s*(.+?)\s*\\\\)");
    QRegularExpressionMatchIterator it = rowRe.globalMatch(content);

    int currentComponent = 0;   // 0 = global, 1 = c1, 2 = c2

    // Ensure at least one component exists
    if (model->components.empty()) {
        SEDComponentParams c1;
        c1.componentIndex = 1;
        model->components.push_back(c1);
    }

    auto ensureComponent = [&](int idx) {
        while (static_cast<int>(model->components.size()) < idx) {
            SEDComponentParams c;
            c.componentIndex = static_cast<int>(model->components.size()) + 1;
            model->components.push_back(c);
        }
    };

    while (it.hasNext()) {
        QRegularExpressionMatch row = it.next();
        QString label = row.captured(1).trimmed();
        QString value = row.captured(2).trimmed();

        // Remove trailing unit markers like \,mag, \,K, \,km\,s$^{-1}$
        // We parse the numeric part only.
        // Remove trailing \,unit
        QString cleanValue = value;
        // Remove trailing \,text..
        static QRegularExpression trailingUnit(R"(\\,.*$)");
        // But we need to be careful — the unit text comes after the number
        // Better approach: just parse the number from the raw value string

        // Detect component sections
        if (label.contains("Component 1")) {
            currentComponent = 1;
            ensureComponent(1);
            continue;
        }
        if (label.contains("Component 2")) {
            currentComponent = 2;
            model->numComponents = 2;
            ensureComponent(2);
            continue;
        }

        ParsedTexValue pv = parseTexValue(value);

        // ── Global parameters ────────────────────────────

        if (label.contains("E(B-V)") && label.contains("SFD")) {
            if (pv.valid) { model->ebvSFD = pv.value; model->ebvSFDError = pv.errUp; }
            continue;
        }
        if (label.contains("E(B-V)") && label.contains("S\\&F")) {
            if (pv.valid) { model->ebvSF = pv.value; model->ebvSFError = pv.errUp; }
            continue;
        }
        if (label.contains("E(44-55)")) {
            if (pv.valid) { model->e4455 = pv.value; model->e4455Error = pv.errUp; }
            continue;
        }
        if (label.contains("R(55)")) {
            if (pv.valid) model->r55 = pv.value;
            continue;
        }
        if (label.contains("\\log(\\Theta")) {
            if (pv.valid) { model->logTheta = pv.value; model->logThetaError = pv.errUp; }
            continue;
        }

        // Parallax — extract RUWE and ZPO from label
        if (label.contains("\\varpi") && label.contains("Gaia")) {
            if (pv.valid) {
                model->parallax = pv.value;
                model->parallaxError = pv.errUp;
            }
            // Extract RUWE
            static QRegularExpression ruweRe(R"(RUWE\s*[=]\s*([\d.]+))");
            QRegularExpressionMatch rm = ruweRe.match(label);
            if (rm.hasMatch()) model->parallaxRuwe = rm.captured(1).toDouble();
            // Extract ZPO
            static QRegularExpression zpoRe(R"(ZPO\s*[=]\s*([+-]?[\d.]+))");
            QRegularExpressionMatch zm = zpoRe.match(label);
            if (zm.hasMatch()) model->parallaxZpo = zm.captured(1).toDouble();
            continue;
        }

        // Distance (mode)
        if (label.contains("Distance") && label.contains("mode") && !label.contains("\\phantom")) {
            if (pv.valid) { model->distanceMode = pv.value; model->distanceModeError = pv.errUp; }
            continue;
        }
        // Distance (median)
        if (label.contains("Distance") && label.contains("median") && !label.contains("\\phantom")) {
            if (pv.valid) { model->distanceMedian = pv.value; model->distanceMedianError = pv.errUp; }
            continue;
        }

        // Reduced chi2
        if (label.contains("\\chi^2") && label.contains("best fit")) {
            if (pv.valid) model->chi2Reduced = pv.value;
            continue;
        }

        // Excess noise
        if (label.contains("\\delta_\\textnormal{excess}") ||
            label.contains("excess noise")) {
            if (pv.valid) model->excessNoise = pv.value;
            continue;
        }

        // ── Per-component parameters ─────────────────────
        if (currentComponent < 1) continue;
        ensureComponent(currentComponent);
        auto& comp = model->components[currentComponent - 1];

        // Skip phantom (duplicate median) lines for R, M, L — handled below
        bool isPhantom = label.contains("\\phantom");

        if (label.contains("T_{\\mathrm{eff}}") && !isPhantom) {
            if (pv.valid) {
                comp.teff = pv.value;
                comp.teffErrUp = pv.errUp;
                comp.teffErrDown = pv.errDown;
                comp.teffStatus = parseStatus(label);
            }
            continue;
        }
        if (label.contains("\\log (g") && !label.contains("\\phantom") &&
            !label.contains("\\varv_\\mathrm{grav}")) {
            if (pv.valid) {
                comp.logg = pv.value;
                comp.loggErrUp = pv.errUp;
                comp.loggErrDown = pv.errDown;
                comp.loggStatus = parseStatus(label);
            }
            continue;
        }
        if (label.contains("Microturbulence") && !isPhantom) {
            if (pv.valid) {
                comp.microturbulence = pv.value;
                comp.microturbulenceStatus = parseStatus(label);
            }
            continue;
        }
        if (label.contains("Metallicity") && !isPhantom) {
            if (pv.valid) {
                comp.metallicity = pv.value;
                comp.metallicityStatus = parseStatus(label);
            }
            continue;
        }
        if (label.contains("n(\\textnormal{He})") && !isPhantom) {
            if (pv.valid) {
                comp.heAbundance = pv.value;
                comp.heAbundanceErrUp = pv.errUp;
                comp.heAbundanceErrDown = pv.errDown;
                comp.heAbundanceStatus = parseStatus(label);
            }
            continue;
        }
        if (label.contains("A_\\mathrm{eff}") && label.contains("Surface ratio")) {
            if (pv.valid) {
                comp.surfaceRatio = pv.value;
                comp.surfaceRatioErrUp = pv.errUp;
                comp.surfaceRatioErrDown = pv.errDown;
            }
            continue;
        }

        // Radius (mode line — not phantom)
        if (label.contains("Radius") && !isPhantom) {
            if (pv.valid) {
                comp.radius = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }
        // Radius (median line — phantom)
        if (label.contains("Radius") && isPhantom) {
            if (pv.valid) {
                comp.radiusMedian = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }

        // Mass (mode)
        if (label.contains("Mass") && !isPhantom) {
            if (pv.valid) {
                comp.mass = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }
        // Mass (median)
        if (label.contains("Mass") && isPhantom) {
            if (pv.valid) {
                comp.massMedian = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }

        // Luminosity (mode)
        if (label.contains("Luminosity") && !isPhantom) {
            if (pv.valid) {
                comp.luminosity = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }
        // Luminosity (median)
        if (label.contains("Luminosity") && isPhantom) {
            if (pv.valid) {
                comp.luminosityMedian = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }

        // Gravitational redshift
        if (label.contains("\\varv_\\mathrm{grav}") ||
            label.contains("varv_\\mathrm{grav}")) {
            if (pv.valid) {
                comp.vGrav = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }

        // Escape velocity
        if (label.contains("\\varv_\\mathrm{esc}") ||
            label.contains("varv_\\mathrm{esc}")) {
            if (pv.valid) {
                comp.vEsc = { pv.value, pv.errUp, pv.errDown };
            }
            continue;
        }
    }

    return true;
}

// ── Build simplified PhotometricPoints for the Photometry container ──
void buildPhotometricPoints(const std::vector<SEDPhotometryPoint>& sedPoints,
                            std::vector<PhotometricPoint>& out)
{
    out.reserve(sedPoints.size());
    for (const auto& sp : sedPoints) {
        if (sp.flag < 0) continue;  // skip excluded points

        PhotometricPoint pp;
        pp.instrument     = sp.system;
        pp.filter         = sp.passband;
        pp.magnitude      = 0.0;        // not in the SED file
        pp.magnitudeError = 0.0;
        pp.flux           = sp.flux;
        pp.fluxError      = (sp.fluxMax - sp.fluxMin) * 0.5;
        pp.wavelength     = sp.lambda;
        out.push_back(pp);
    }
}

}  // anonymous namespace

// ══════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════

bool ExtractSED::isSEDFitDirectory(const QString& dirPath)
{
    QDir dir(dirPath);
    return dir.exists("photometry_fit.txt") ||
           dir.exists("photometry_fit_mag.txt");
}

SEDExtractResult ExtractSED::extractFromDirectory(const QString& dirPath)
{
    SEDExtractResult result;
    result.folderName = QFileInfo(dirPath).fileName();

    QDir dir(dirPath);
    if (!dir.exists()) {
        result.errorMessage = "Directory does not exist: " + dirPath;
        return result;
    }

    // Check minimum requirements
    if (!isSEDFitDirectory(dirPath)) {
        result.errorMessage = "Not an ISIS SED fit directory (missing photometry_fit*.txt)";
        return result;
    }

    auto model = std::make_shared<SEDModel>();
    model->creationDate = QDateTime::currentDateTime();

    // ── 1. Parse model SED curve ─────────────────────────────
    {
        QString fitPath = dir.filePath("photometry_fit.txt");
        if (QFile::exists(fitPath)) {
            int nc = 1;
            if (!parseModelCurve(fitPath,
                                 model->modelWavelengths,
                                 model->modelFluxes,
                                 model->componentFluxes,
                                 nc)) {
                result.errorMessage = "Failed to parse photometry_fit.txt";
                return result;
            }
            model->numComponents = nc;
        }
    }

    // ── 2. Parse observed photometry ─────────────────────────
    {
        QString obsPath = dir.filePath("photometry_fit_mag.txt");
        if (QFile::exists(obsPath)) {
            parseObservedPhotometry(obsPath, model->observedPoints);
        }
    }

    // ── 3. Parse stellar component files ─────────────────────
    for (int c = 1; c <= 2; ++c) {
        QString cPath = dir.filePath(
            QString("photometry_results_stellar_c%1.txt").arg(c));
        if (!QFile::exists(cPath)) continue;

        // Ensure component vector is large enough
        while (static_cast<int>(model->components.size()) < c) {
            SEDComponentParams cp;
            cp.componentIndex = static_cast<int>(model->components.size()) + 1;
            model->components.push_back(cp);
        }

        parseStellarComponent(cPath, model->components[c - 1]);

        if (c > model->numComponents)
            model->numComponents = c;
    }

    // ── 4. Parse TeX results (overrides/supplements above) ───
    {
        QString texPath = dir.filePath("photometry_results.tex");
        if (QFile::exists(texPath)) {
            parseTexResults(texPath, model);
        }
    }

    // ── 5. Set component indices ─────────────────────────────
    for (int i = 0; i < static_cast<int>(model->components.size()); ++i) {
        model->components[i].componentIndex = i + 1;
    }

    // ── 6. Build simplified photometric points ───────────────
    buildPhotometricPoints(model->observedPoints, result.photometricPoints);

    // ── 7. Populate result ───────────────────────────────────
    result.model       = model;
    result.objectName  = model->objectName;
    result.success     = true;

    // Merge magnitude data from photometry.dat if present
    QString photDat = dirPath + "/photometry.dat";
    if (QFile::exists(photDat) && result.model)
        mergePhotometryDat(photDat, result.model->observedPoints);

    return result;
}

void ExtractSED::mergePhotometryDat(const QString& filePath,
                                     std::vector<SEDPhotometryPoint>& points)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&file);

    std::map<std::pair<QString,QString>, int> lookup;
    for (int i = 0; i < static_cast<int>(points.size()); ++i)
        lookup[{points[i].system, points[i].passband}] = i;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("flag"))
            continue;

        QStringList parts = line.split(QRegularExpression("\\s+"),
                                       Qt::SkipEmptyParts);
        if (parts.size() < 6) continue;

        QString sys     = parts[1];
        QString band    = parts[2];
        double  mag     = parts[3].toDouble();
        double  magErr  = parts[4].toDouble();
        QString type    = parts[5];
        double  angDist = parts.size() > 6 ? parts[6].toDouble() : 0.0;
        QString vizCat  = parts.size() > 7 ? parts[7] : QString();

        auto it = lookup.find({sys, band});
        if (it != lookup.end()) {
            auto& p = points[it->second];
            p.magnitude    = mag;
            p.magnitudeErr = magErr;
            p.type         = type;
            p.angularDist  = angDist;
            if (p.vizierCatalog.isEmpty())
                p.vizierCatalog = vizCat;
        }
    }
}