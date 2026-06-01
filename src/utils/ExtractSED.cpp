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

// Whitespace tokenizer over a byte range — produces views, no allocations.
inline int splitWhitespace(const char *b, const char *e, std::string_view *out,
                           int maxTok) {
    int n = 0;
    while (b < e && n < maxTok) {
        while (b < e && (*b == ' ' || *b == '\t' || *b == '\r'))
            ++b;
        if (b >= e)
            break;
        const char *s = b;
        while (b < e && *b != ' ' && *b != '\t' && *b != '\r')
            ++b;
        out[n++] = std::string_view(s, static_cast<size_t>(b - s));
    }
    return n;
}

inline bool toDouble(std::string_view sv, double &out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc();
}

inline bool toInt(std::string_view sv, int &out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc();
}

inline QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
}

// ── Whitespace-delimited tokenizer that respects quoted strings ─
QStringList tokenizeLine(const QString& line)
{
    return line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}

// ── Parse photometry_fit_mag.txt ─────────────────────────────
bool parseObservedPhotometry(const QString                   &filepath,
                             std::vector<SEDPhotometryPoint> &points) {
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();

    const char *p   = data.constData();
    const char *end = p + data.size();
    points.reserve(static_cast<size_t>(std::count(p, end, '\n')));

    bool             headerChecked = false;
    std::string_view tok[16];

    while (p < end) {
        const char *nl   = static_cast<const char *>(memchr(p, '\n', end - p));
        const char *beg  = p;
        const char *lend = nl ? nl : end;
        p                = nl ? nl + 1 : end;

        const int nt = splitWhitespace(beg, lend, tok, 16);
        if (nt == 0)
            continue;

        if (!headerChecked) { // detect header by non-numeric col 0
            headerChecked = true;
            double probe;
            if (!toDouble(tok[0], probe))
                continue; // header row → skip
            // otherwise it's data; fall through
        }

        if (nt < 12)
            continue;

        SEDPhotometryPoint pt;
        toDouble(tok[0], pt.lambdaMin);
        toDouble(tok[1], pt.lambda);
        toDouble(tok[2], pt.lambdaMax);
        toDouble(tok[3], pt.fluxMin);
        toDouble(tok[4], pt.flux);
        toDouble(tok[5], pt.fluxMax);
        toDouble(tok[6], pt.diff);
        toDouble(tok[7], pt.diffErr);
        pt.passband = toQString(tok[8]);
        pt.system   = toQString(tok[9]);
        toInt(tok[10], pt.flag);
        pt.vizierCatalog = toQString(tok[11]);

        points.push_back(std::move(pt));
    }
    return !points.empty();
}

// ── Parse photometry_fit.txt ─────────────────────────────────
bool parseModelCurve(const QString &filepath, std::vector<double> &wavelengths,
                     std::vector<double>              &totalFlux,
                     std::vector<std::vector<double>> &compFluxes,
                     int                              &numComponents) {
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) // no Text flag: we handle \r ourselves
        return false;
    const QByteArray data = file.readAll();
    file.close();

    const char *p   = data.constData();
    const char *end = p + data.size();

    const int approxLines = static_cast<int>(std::count(p, end, '\n'));
    wavelengths.reserve(approxLines);
    totalFlux.reserve(approxLines);

    numComponents               = 0;
    bool             haveHeader = false;
    std::string_view tok[32];

    while (p < end) {
        const char *nl   = static_cast<const char *>(memchr(p, '\n', end - p));
        const char *beg  = p;
        const char *lend = nl ? nl : end;
        p                = nl ? nl + 1 : end;

        const int nt = splitWhitespace(beg, lend, tok, 32);
        if (nt == 0)
            continue;

        if (!haveHeader) { // first non-empty line = header
            numComponents = (nt >= 4) ? (nt - 2) : 1;
            compFluxes.assign(numComponents, {});
            for (auto &v : compFluxes)
                v.reserve(approxLines);
            haveHeader = true;
            continue;
        }

        if (nt < 2)
            continue;
        double wl, fl;
        if (!toDouble(tok[0], wl) || !toDouble(tok[1], fl))
            continue;

        wavelengths.push_back(wl);
        totalFlux.push_back(fl);

        for (int c = 0; c < numComponents; ++c) {
            const int col = 2 + c;
            double    v;
            if (col < nt && toDouble(tok[col], v))
                compFluxes[c].push_back(v);
            else
                compFluxes[c].push_back(fl); // mirror total (original behavior)
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

    // Extract content of the first $...$ math block only,
    // which separates the numeric value from trailing unit text
    // like $0.0523 \pm 0.0022$\,mag  →  0.0523 \pm 0.0022
    if (s.startsWith('$')) {
        int closingDollar = s.indexOf('$', 1);
        if (closingDollar > 0)
            s = s.mid(1, closingDollar - 1);
        else
            s = s.mid(1);
    }

    // Strip LaTeX commands that wrap or hide numbers
    static QRegularExpression colorRe(R"(\\color\{[^}]*\})");
    s.remove(colorRe);

    static QRegularExpression phantomRe(R"(\\phantom\{[^}]*\})");
    s.remove(phantomRe);

    s.remove("\\left(");
    s.remove("\\right)");
    s.remove("\\,");

    // Remove all remaining braces so  value^{+err}_{-err}
    // becomes  value^+err_-err
    s.remove('{');
    s.remove('}');

    // Handle \times10^N scientific notation (braces already gone)
    double multiplier = 1.0;
    static QRegularExpression sciRe(R"(\\times\s*10\^\s*([+-]?\d+))");
    QRegularExpressionMatch sciMatch = sciRe.match(s);
    if (sciMatch.hasMatch()) {
        multiplier = std::pow(10.0, sciMatch.captured(1).toDouble());
        s = s.left(sciMatch.capturedStart()).trimmed();
    }

    // Asymmetric: value^+upper_-lower
    // Upper/lower may be negative when the fit hits a boundary (e.g. ^+-0)
    static QRegularExpression asymRe(
        R"(([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*\^\s*\+\s*(-?[\d.]+(?:[eE][+-]?\d+)?)\s*_\s*-\s*(-?[\d.]+(?:[eE][+-]?\d+)?))");
    QRegularExpressionMatch asymMatch = asymRe.match(s);
    if (asymMatch.hasMatch()) {
        r.value    = asymMatch.captured(1).toDouble() * multiplier;
        r.errUp    = std::abs(asymMatch.captured(2).toDouble()) * multiplier;
        r.errDown  = std::abs(asymMatch.captured(3).toDouble()) * multiplier;
        r.hasError = true;
        r.valid    = true;
        return r;
    }

    // Symmetric: value \pm error
    static QRegularExpression symRe(
        R"(([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*\\pm\s*([\d.]+(?:[eE][+-]?\d+)?))");
    QRegularExpressionMatch symMatch = symRe.match(s);
    if (symMatch.hasMatch()) {
        r.value    = symMatch.captured(1).toDouble() * multiplier;
        r.errUp    = symMatch.captured(2).toDouble() * multiplier;
        r.errDown  = r.errUp;
        r.hasError = true;
        r.valid    = true;
        return r;
    }

    // Plain number
    bool ok;
    double v = s.trimmed().toDouble(&ok);
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

    if (model->components.empty()) {
        SEDComponentParams c1;
        c1.componentIndex = 1;
        model->components.push_back(c1);
    }

    // Single-component fits have no explicit "Component 1:" header,
    // so start with component 1 active
    int currentComponent = 1;

    auto ensureComponent = [&](int idx) {
        while (static_cast<int>(model->components.size()) < idx) {
            SEDComponentParams c;
            c.componentIndex = static_cast<int>(model->components.size()) + 1;
            model->components.push_back(c);
        }
    };

    // Find the last unescaped '&' in a line — this is the real table
    // column separator.  Escaped \& (e.g. S\&F) is skipped.
    auto findColumnSeparator = [](const QString& line) -> int {
        for (int i = line.length() - 1; i >= 0; --i) {
            if (line[i] == '&' && (i == 0 || line[i - 1] != '\\'))
                return i;
        }
        return -1;
    };

    const QStringList lines = content.split('\n');

    for (const QString& rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        if (line.startsWith("\\documentclass") ||
            line.startsWith("\\usepackage") ||
            line.startsWith("\\begin{") ||
            line.startsWith("\\end{") ||
            line.startsWith("\\renewcommand") ||
            line.startsWith("\\hline"))
            continue;

        // \multicolumn lines have no real column separator —
        // check them for component headers and move on
        if (line.contains("\\multicolumn")) {
            if (line.contains("Component 1")) {
                currentComponent = 1;
                ensureComponent(1);
            } else if (line.contains("Component 2")) {
                currentComponent = 2;
                model->numComponents = 2;
                ensureComponent(2);
            }
            continue;
        }

        int sepPos = findColumnSeparator(line);
        if (sepPos < 0) continue;

        QString label = line.left(sepPos).trimmed();
        QString value = line.mid(sepPos + 1).trimmed();

        if (value.endsWith("\\\\")) {
            value.chop(2);
            value = value.trimmed();
        }

        // ── Object name ──────────────────────────────────
        if (label.contains("Object:")) {
            static QRegularExpression objRe(R"(Object:\s*(.+))");
            QRegularExpressionMatch m = objRe.match(label);
            if (m.hasMatch())
                model->objectName = m.captured(1).trimmed();
            continue;
        }

        // Fallback component detection for non-\multicolumn formats
        if (label.contains("Component 1") && !label.contains("\\phantom")) {
            currentComponent = 1;
            ensureComponent(1);
            continue;
        }
        if (label.contains("Component 2") && !label.contains("\\phantom")) {
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

        if (label.contains("\\varpi") && label.contains("Gaia")) {
            if (pv.valid) {
                model->parallax = pv.value;
                model->parallaxError = pv.errUp;
            }
            static QRegularExpression ruweRe(R"(RUWE[^=]*=\s*([\d.]+))");
            QRegularExpressionMatch rm = ruweRe.match(label);
            if (rm.hasMatch()) model->parallaxRuwe = rm.captured(1).toDouble();
            static QRegularExpression zpoRe(R"(ZPO[^=]*=\s*([+-]?[\d.]+))");
            QRegularExpressionMatch zm = zpoRe.match(label);
            if (zm.hasMatch()) model->parallaxZpo = zm.captured(1).toDouble();
            continue;
        }

        if (label.contains("Distance") && label.contains("mode") && !label.contains("\\phantom")) {
            if (pv.valid) { model->distanceMode = pv.value; model->distanceModeError = pv.errUp; }
            continue;
        }
        if (label.contains("Distance") && label.contains("median") && !label.contains("\\phantom")) {
            if (pv.valid) { model->distanceMedian = pv.value; model->distanceMedianError = pv.errUp; }
            continue;
        }

        if (label.contains("\\chi^2") && label.contains("best fit")) {
            if (pv.valid) model->chi2Reduced = pv.value;
            continue;
        }

        if (label.contains("\\delta_\\textnormal{excess}") ||
            label.contains("excess noise")) {
            if (pv.valid) model->excessNoise = pv.value;
            continue;
        }

        // ── Per-component parameters ─────────────────────
        if (currentComponent < 1) continue;
        ensureComponent(currentComponent);
        auto& comp = model->components[currentComponent - 1];

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
        if (label.contains("\\log (g") && !isPhantom &&
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
        if (label.contains("Surface ratio")) {
            if (pv.valid) {
                comp.surfaceRatio = pv.value;
                comp.surfaceRatioErrUp = pv.errUp;
                comp.surfaceRatioErrDown = pv.errDown;
            }
            continue;
        }

        if (label.contains("Radius") && !isPhantom) {
            if (pv.valid)
                comp.radius = { pv.value, pv.errUp, pv.errDown };
            continue;
        }
        if (label.contains("Radius") && isPhantom) {
            if (pv.valid)
                comp.radiusMedian = { pv.value, pv.errUp, pv.errDown };
            continue;
        }

        if (label.contains("Mass") && !isPhantom) {
            if (pv.valid)
                comp.mass = { pv.value, pv.errUp, pv.errDown };
            continue;
        }
        if (label.contains("Mass") && isPhantom) {
            if (pv.valid)
                comp.massMedian = { pv.value, pv.errUp, pv.errDown };
            continue;
        }

        if (label.contains("Luminosity") && !isPhantom) {
            if (pv.valid)
                comp.luminosity = { pv.value, pv.errUp, pv.errDown };
            continue;
        }
        if (label.contains("Luminosity") && isPhantom) {
            if (pv.valid)
                comp.luminosityMedian = { pv.value, pv.errUp, pv.errDown };
            continue;
        }

        if (label.contains("\\varv_\\mathrm{grav}") ||
            label.contains("varv_\\mathrm{grav}")) {
            if (pv.valid)
                comp.vGrav = { pv.value, pv.errUp, pv.errDown };
            continue;
        }

        if (label.contains("\\varv_\\mathrm{esc}") ||
            label.contains("varv_\\mathrm{esc}")) {
            if (pv.valid)
                comp.vEsc = { pv.value, pv.errUp, pv.errDown };
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

SEDExtractResult ExtractSED::extractFromDirectory(const QString &dirPath) {
    SEDExtractResult result;
    result.folderName = QFileInfo(dirPath).fileName();

    QDir dir(dirPath);

    // Single readdir() instead of a dozen exists()/stat() calls.
    const QStringList listing =
        dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    const QSet<QString> files(listing.cbegin(), listing.cend());

    const auto has = [&files](const char *name) {
        return files.contains(QLatin1String(name));
    };

    // We already validated this in findSEDDirectories, but cheap to confirm
    // now that it's just an in-memory lookup:
    if (!has("photometry_fit.txt") && !has("photometry_fit_mag.txt")) {
        result.errorMessage =
            "Not an ISIS SED fit directory (missing photometry_fit*.txt)";
        return result;
    }

    auto model          = std::make_shared<SEDModel>();
    model->creationDate = QDateTime::currentDateTime();

    // ── 1. Model SED curve ───────────────────────────────────
    if (has("photometry_fit.txt")) {
        int nc = 1;
        if (!parseModelCurve(dir.filePath("photometry_fit.txt"),
                             model->modelWavelengths, model->modelFluxes,
                             model->componentFluxes, nc)) {
            result.errorMessage = "Failed to parse photometry_fit.txt";
            return result;
        }
        model->numComponents = nc;
    }

    // ── 2. Observed photometry ───────────────────────────────
    if (has("photometry_fit_mag.txt"))
        parseObservedPhotometry(dir.filePath("photometry_fit_mag.txt"),
                                model->observedPoints);

    // ── 3. Stellar component files ───────────────────────────
    for (int c = 1; c <= 2; ++c) {
        const QString name =
            QStringLiteral("photometry_results_stellar_c%1.txt").arg(c);
        if (!files.contains(name))
            continue;

        while (static_cast<int>(model->components.size()) < c) {
            SEDComponentParams cp;
            cp.componentIndex = static_cast<int>(model->components.size()) + 1;
            model->components.push_back(cp);
        }
        parseStellarComponent(dir.filePath(name), model->components[c - 1]);
        if (c > model->numComponents)
            model->numComponents = c;
    }

    // ── 4. TeX results ───────────────────────────────────────
    if (has("photometry_results.tex"))
        parseTexResults(dir.filePath("photometry_results.tex"), model);

    // ── 5. Component indices ─────────────────────────────────
    for (int i = 0; i < static_cast<int>(model->components.size()); ++i)
        model->components[i].componentIndex = i + 1;

    // ── 6. Photometric points ────────────────────────────────
    buildPhotometricPoints(model->observedPoints, result.photometricPoints);

    // ── 7. Result + photometry.dat merge ─────────────────────
    result.model      = model;
    result.objectName = model->objectName;
    result.success    = true;

    if (has("photometry.dat"))
        mergePhotometryDat(dir.filePath("photometry.dat"),
                           model->observedPoints);

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