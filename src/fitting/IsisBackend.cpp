#include "IsisBackend.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <optional>
#include <stdexcept>

namespace astra::fitting {

// ─────────────────────────────────────────────────────────────────────
//  Helpers: autoeps cache, progress staging, parsing
// ─────────────────────────────────────────────────────────────────────
namespace {

QString ensureAutoepsCache(const QString& workDir,
                            const std::function<void(const QString&)>& onLog)
{
    const QString cacheRoot = AppPaths::root();
    if (cacheRoot.isEmpty()) return {};
    const QString autoepsCache = cacheRoot + "/cache/isis/autoeps";
    QDir().mkpath(autoepsCache);

    const QString linkPath = workDir + "/autoeps";
    QFileInfo fi(linkPath);
    if (fi.exists() || fi.isSymLink()) QFile::remove(linkPath);

    if (!QFile::link(autoepsCache, linkPath)) {
        if (onLog) onLog(QStringLiteral(
            "Warning: could not symlink autoeps cache (%1 → %2); "
            "ISIS will rebuild it from scratch.")
                .arg(linkPath, autoepsCache));
        return {};
    }
    if (onLog) onLog(QStringLiteral("autoeps cache: %1").arg(autoepsCache));
    return autoepsCache;
}

struct IsisStage { const char* needle; const char* label; double fraction; };
constexpr IsisStage kIsisStages[] = {
    { "Ignoring spectra with SNR",          "Filtering by SNR...",       0.05 },
    { "All spectra have SNR",               "SNR filter done",         0.08 },
    { "First guess for the continuum.",     "Continuum first guess...",  0.15 },
    { "First guess for the continuum + vrad",
                                            "Continuum + vrad...",       0.25 },
    { "First guess for continuum + vrad + teff",
                                            "Continuum + atm params...", 0.35 },
    { "Performing a first full fit",        "First full fit...",         0.50 },
    { "Iteratively ignoring outliers",      "Outlier rejection...",      0.70 },
    { "Performing an iterative fitting",    "Noise re-estimation...",    0.85 },
    { "Creating output PDF files",          "Writing output...",         0.95 },
};

// ── Parsing structures ─────────────────────────────────────────
struct StellarEntry {
    double value = 0.0, min = 0.0, max = 0.0;
    bool   frozen = false;
    int    tieTo  = 0;
};

struct IsisParams {
    // stellar[spectrumIdx (1-based)][componentIdx (1-based)][paramName] → entry
    QHash<int, QHash<int, QHash<QString, StellarEntry>>> stellar;
    // cspline anchor pairs per 1-based spectrum index
    QHash<int, QVector<QPair<double,double>>>            cspline;
    int nFreeParams = 0;
};

struct PropertiesRow {
    QString filename;
    QString spectype;
    QHash<QString, double> values;   // e.g. "c1_vrad_min" → number
};

struct TexResults {
    QString grid;
    double  chi2RedFinal   = 0.0;
    double  chi2RedInitial = 0.0;
    double  snr            = 0.0;
    double  snrEff         = 0.0;
    QHash<QString, QPair<double,double>> tiedParams; // key: "teff","logg",...
};

// ── spectroscopy_spectrum_params.dat ───────────────────────────
bool parseSpectrumParamsFile(const QString& path, IsisParams& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    static const QRegularExpression reStellar(
        R"(^stellar\(\d+\)\.d(\d+)_(?:c(\d+)_)?(\w+)$)");
    static const QRegularExpression reCspline(
        R"(^cspline\(\d+\)\.d(\d+)_([xy])(\d+)$)");
    static const QRegularExpression reWS(R"(\s+)");

    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        const QStringList t = line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (t.size() < 7) continue;

        bool idxOk = false;
        t[0].toInt(&idxOk);
        if (!idxOk) continue;

        const QString name   = t[1];
        const int     tieTo  = t[2].toInt();
        const bool    frozen = (t[3].toInt() != 0);
        const double  value  = t[4].toDouble();
        const double  pmin   = t[5].toDouble();
        const double  pmax   = t[6].toDouble();
        if (!frozen) ++out.nFreeParams;

        if (auto m = reStellar.match(name); m.hasMatch()) {
            const int spec = m.captured(1).toInt();
            const int comp = m.captured(2).isEmpty() ? 1 : m.captured(2).toInt();
            const QString pname = m.captured(3);
            out.stellar[spec][comp][pname] =
                StellarEntry{ value, pmin, pmax, frozen, tieTo };
            continue;
        }
        if (auto m = reCspline.match(name); m.hasMatch()) {
            const int spec = m.captured(1).toInt();
            const QString axis = m.captured(2);
            const int k = m.captured(3).toInt();
            auto& vec = out.cspline[spec];
            while (vec.size() <= k) vec.append({0.0, 0.0});
            if (axis == "x") vec[k].first  = value;
            else             vec[k].second = value;
        }
    }
    return true;
}

// ── spectrum_properties.txt ────────────────────────────────────
bool parseSpectrumPropertiesFile(const QString& path,
                                  QVector<PropertiesRow>& rows)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream ts(&f);

    QString headerLine;
    if (!ts.readLineInto(&headerLine)) return false;
    static const QRegularExpression reWS(R"(\s+)");
    const QStringList headers =
        headerLine.trimmed().split(reWS, Qt::SkipEmptyParts);

    QString line;
    while (ts.readLineInto(&line)) {
        const QStringList tokens =
            line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (tokens.size() != headers.size()) continue;

        PropertiesRow r;
        for (int i = 0; i < headers.size(); ++i) {
            const QString& h = headers[i];
            const QString& v = tokens[i];
            if (h == "filename")       r.filename = v;
            else if (h == "spectype")  r.spectype = v;
            else {
                bool ok = false;
                double dv = v.toDouble(&ok);
                if (ok) r.values.insert(h, dv);
            }
        }
        rows.append(r);
    }
    return true;
}

// ── spectroscopy_results.tex ───────────────────────────────────
static QPair<double,double> parseTexValue(const QString& raw)
{
    QString s = raw;
    s.remove(QRegularExpression(R"(\\color\{[^}]*\})"));
    s.remove(QRegularExpression(R"(\\mathrm\{[^}]*\})"));
    s.remove(QRegularExpression(R"(\\,)"));
    s.remove('$');

    static const QRegularExpression rePm(
        R"(([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*\\pm\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?))");
    if (auto m = rePm.match(s); m.hasMatch())
        return { m.captured(1).toDouble(), m.captured(2).toDouble() };

    static const QRegularExpression reAsym(
        R"(([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*\^\{\+\{?([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\}?\}_\{-\{?([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\}?\})");
    if (auto m = reAsym.match(s); m.hasMatch()) {
        const double v = m.captured(1).toDouble();
        const double p = m.captured(2).toDouble();
        const double mi = m.captured(3).toDouble();
        return { v, 0.5 * (p + mi) };
    }

    static const QRegularExpression reNum(
        R"([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)");
    if (auto m = reNum.match(s); m.hasMatch())
        return { m.captured(0).toDouble(), 0.0 };
    return { 0.0, 0.0 };
}

bool parseResultsTexFile(const QString& path, TexResults& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString content = QString::fromUtf8(f.readAll());
    const QStringList lines = content.split('\n');

    auto rowExpr = [&](const QString& contains) -> std::optional<QString> {
        for (const QString& ln : lines) {
            if (!ln.contains(contains)) continue;
            const int amp = ln.indexOf('&');
            const int end = ln.lastIndexOf("\\\\");
            if (amp > 0 && end > amp)
                return ln.mid(amp + 1, end - amp - 1).trimmed();
        }
        return std::nullopt;
    };

    if (auto e = rowExpr("Grid ")) {
        QString g = *e;
        g.remove('$');
        out.grid = g.trimmed();
    }
    if (auto e = rowExpr(R"(\chi^2_\mathrm{red,final})"))
        out.chi2RedFinal = parseTexValue(*e).first;
    if (auto e = rowExpr(R"(\chi^2_\mathrm{red,initial})"))
        out.chi2RedInitial = parseTexValue(*e).first;
    if (auto e = rowExpr(R"(SNR}^\mathrm{tot}_\mathrm{eff})"))
        out.snrEff = parseTexValue(*e).first;
    else if (auto e = rowExpr(R"(SNR}^\mathrm{tot})"))
        out.snr = parseTexValue(*e).first;

    struct Row { const char* marker; const char* key; };
    const Row rows[] = {
        { "Effective temperature",         "teff"  },
        { "Surface gravity",               "logg"  },
        { "Projected rotational velocity", "vsini" },
        { "Macroturbulence",               "zeta"  },
        { "Microturbulence",               "xi"    },
        { "Metallicity",                   "z"     },
        { "He abundance",                  "he"    },
    };
    for (const auto& r : rows)
        if (auto e = rowExpr(QString::fromLatin1(r.marker)))
            out.tiedParams[QString::fromLatin1(r.key)] = parseTexValue(*e);

    return true;
}

// ── <uuid>_id_<N>.dat ──────────────────────────────────────────
bool loadSpectrumDat(const QString& path, FittedSpectrum& fs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    static const QRegularExpression reWS(R"(\s+)");

    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        const QStringList t = line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (t.size() < 6) continue;
        bool ok[6];
        const double wl    = t[0].toDouble(&ok[0]);
        const double model = t[1].toDouble(&ok[1]);
        const double cont  = t[2].toDouble(&ok[2]);
        const double flux  = t[3].toDouble(&ok[3]);
        const double sig   = t[4].toDouble(&ok[4]);
        const int    flag  = t[5].toInt   (&ok[5]);
        if (!(ok[0] && ok[1] && ok[2] && ok[3] && ok[4] && ok[5])) continue;

        fs.lambda    .append(wl);
        fs.model     .append(model);
        fs.continuum .append(cont);
        fs.flux      .append(flux);
        fs.sigma     .append(sig);
        // ISIS flag matches ASTRA's ignoreFlag convention as the array is used
        // downstream: non-zero marks masked points, zero marks fit points.
        fs.ignoreFlag.append(static_cast<uint8_t>(flag != 0 ? 1 : 0));
    }
    return !fs.lambda.isEmpty();
}

// ── Map everything onto SpectralFitResult ──────────────────────
bool parseIsisOutputs(SpectralFitResult& out,
                      const QString& workDir,
                      const SpectralFitJob& job,
                      const std::function<void(const QString&)>& onLog)
{
    IsisParams params;
    if (!parseSpectrumParamsFile(workDir + "/spectroscopy_spectrum_params.dat",
                                  params))
    {
        if (onLog) onLog("Could not read spectroscopy_spectrum_params.dat");
        return false;
    }

    QVector<PropertiesRow> props;
    parseSpectrumPropertiesFile(workDir + "/spectrum_properties.txt", props);

    TexResults tex;
    parseResultsTexFile(workDir + "/spectroscopy_results.tex", tex);

    QVector<QString> submittedIds;
    for (const auto& obs : job.observations)
        for (const auto& fi : obs.files)
            submittedIds.append(fi.spectrumId);

    const int nSpec = submittedIds.size();
    const int nComp = std::max(1, int(job.components.size()));

    QSet<QString> untied;
    for (const auto& u : job.untiedParams) untied.insert(u.trimmed().toLower());

    // Scalars
    out.finalChi2       = tex.chi2RedFinal;
    out.nFreeParameters = params.nFreeParams;
    out.iterations      = 0;       // not exposed by spectroscopy_automated
    out.converged       = true;    // clean exit implies converged

    auto getVal = [&](int spec, int comp,
                      const QString& isisName) -> std::optional<StellarEntry> {
        if (!params.stellar.contains(spec))              return std::nullopt;
        if (!params.stellar[spec].contains(comp))        return std::nullopt;
        const auto& m = params.stellar[spec][comp];
        if (!m.contains(isisName))                       return std::nullopt;
        return m.value(isisName);
    };

    // Components ----------------------------------------------------
    for (int c = 1; c <= nComp; ++c) {
        FittedComponent fc;
        auto addParam = [&](const QString& isisName,
                            const QString& astraKey,
                            QVector<FittedParameter>& dest) {
            const bool isUntied = untied.contains(astraKey);

            if (isUntied) {
                for (int s = 1; s <= nSpec; ++s) {
                    FittedParameter fp;
                    if (auto e = getVal(s, c, isisName)) {
                        fp.value  = e->value;
                        fp.frozen = e->frozen;
                    }
                    // Error & value override from spectrum_properties.txt
                    const QString col    = QString("c%1_%2").arg(c).arg(isisName);
                    const QString colMin = col + "_min";
                    const QString colMax = col + "_max";
                    if (s - 1 < props.size()) {
                        const auto& r = props[s - 1];
                        if (r.values.contains(col))    fp.value = r.values[col];
                        if (r.values.contains(colMin) &&
                            r.values.contains(colMax))
                        {
                            fp.error = 0.5 *
                                (r.values[colMax] - r.values[colMin]);
                        }
                    }
                    dest.append(fp);
                }
            } else {
                FittedParameter fp;
                if (auto e = getVal(1, c, isisName)) {
                    fp.value  = e->value;
                    fp.frozen = e->frozen;
                }
                if (tex.tiedParams.contains(astraKey)) {
                    const auto pv = tex.tiedParams.value(astraKey);
                    if (pv.first != 0.0 || pv.second != 0.0) {
                        fp.value = pv.first;
                        fp.error = pv.second;
                    }
                }
                dest.append(fp);
            }
        };

        addParam("teff",  "teff",  fc.teff);
        addParam("logg",  "logg",  fc.logg);
        addParam("vsini", "vsini", fc.vsini);
        addParam("HE",    "he",    fc.he);
        addParam("zeta",  "zeta",  fc.zeta);
        addParam("xi",    "xi",    fc.xi);
        addParam("z",     "z",     fc.z);
        addParam("vrad",  "vrad",  fc.vrad);
        out.components.append(fc);
    }

    // Per-spectrum plot data ---------------------------------------
    for (int s = 0; s < nSpec; ++s) {
        const QString id = submittedIds[s];
        const QString dat = QString("%1/%2_id_%3.dat")
                                .arg(workDir, id).arg(s + 1);

        FittedSpectrum fs;
        fs.spectrumId = id;

        if (!QFile::exists(dat) || !loadSpectrumDat(dat, fs)) {
            if (onLog) onLog(QStringLiteral(
                "No model data for spectrum %1 (expected %2)")
                    .arg(id, dat));
            out.rejectedFiles.append(id);
            continue;
        }
        if (s == 0) out.nDataPoints = fs.lambda.size();

        const int specIdx = s + 1;
        if (params.cspline.contains(specIdx)) {
            for (const auto& xy : params.cspline[specIdx]) {
                fs.contX.append(xy.first);
                fs.contY.append(xy.second);
            }
        }
        out.spectra.append(fs);
    }

    if (onLog) {
        onLog(QStringLiteral(
            "Parsed: grid=%1  χ²_red=%2  spectra=%3  free params=%4")
                .arg(tex.grid.isEmpty() ? "?" : tex.grid)
                .arg(out.finalChi2, 0, 'f', 3)
                .arg(out.spectra.size())
                .arg(out.nFreeParameters));
    }

    out.success = true;
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────
//  IsisBackend
// ─────────────────────────────────────────────────────────────────────

QString IsisBackend::resolveBinary()
{
    AppSettings settings;
    const QString custom = settings.isisBinaryPath().trimmed();
    if (!custom.isEmpty() && QFileInfo(custom).isExecutable())
        return custom;
    return QStandardPaths::findExecutable("isis");
}

QString IsisBackend::generateScript(const SpectralFitJob& job)
{
    QString s;
    QTextStream out(&s);
    out.setRealNumberPrecision(10);

    out << "require(\"stellar_isisscripts.sl\");\n";
    out << "variable tscript_start = _ftime;\n\n";

    QStringList grids;
    for (const auto& c : job.components) {
        QString g = c.gridPath.trimmed();
        if (g.isEmpty()) continue;
        if (!g.endsWith('/')) g += '/';
        grids << "\"" + g + "\"";
    }
    if (grids.isEmpty()) grids << "\"sdB/processed/\"";
    out << "variable modelgrid = [" << grids.join(", ") << "];\n\n";

    const bool multi = job.components.size() > 1;
    QStringList names, values, freeze;
    auto add = [&](const QString& prefix, const QString& n,
                    double v, bool fz) {
        names  << "\"" + prefix + n + "\"";
        values << QString::number(v);
        freeze << (fz ? "1" : "0");
    };
    for (int i = 0; i < job.components.size(); ++i) {
        const auto& c = job.components[i];
        const QString pfx = multi ? QString("c%1_").arg(i + 1) : QString();
        add(pfx, "vrad",  0.0,     false);
        add(pfx, "vsini", c.vsini, c.freezeVsini);
        add(pfx, "zeta",  c.zeta,  c.freezeZeta);
        add(pfx, "teff",  c.teff,  c.freezeTeff);
        add(pfx, "logg",  c.logg,  c.freezeLogg);
        add(pfx, "xi",    c.xi,    c.freezeXi);
        add(pfx, "z",     c.z,     c.freezeZ);
        add(pfx, "HE",    c.he,    c.freezeHe);
    }
    out << "variable initial_guess_params_values = struct{\n"
        << "    name   = [" << names.join(", ")  << "],\n"
        << "    value  = [" << values.join(", ") << "],\n"
        << "    freeze = [" << freeze.join(", ") << "] };\n\n";

    QStringList entries;
    for (const auto& obs : job.observations) {
        for (const auto& f : obs.files) {
            QString entry;
            QTextStream es(&entry);
            es.setRealNumberPrecision(10);
            es << "   struct{\n";
            es << "     filename = \"" << f.filename << "\",\n";
            es << "     spectype = \"" << f.spectype << "\",\n";

            const auto ignoreList = f.ignore.value_or(obs.ignore);
            if (!ignoreList.isEmpty()) {
                QStringList parts;
                for (const auto& ig : ignoreList)
                    parts << QString("{%1,%2}").arg(ig.wlLow).arg(ig.wlHigh);
                es << "     ignore = [" << parts.join(",") << "],\n";
            }
            const auto anchorList = f.anchors.value_or(obs.anchors);
            if (!anchorList.isEmpty()) {
                QStringList parts;
                for (const auto& a : anchorList)
                    parts << QString("[%1:%2:%3]")
                                 .arg(a.wlLow).arg(a.wlHigh).arg(a.spacing);
                es << "     cspline_anchorpoints = [" << parts.join(",") << "],\n";
            }
            es << "     res_offset = " << f.resOffset << ",\n";
            es << "     res_slope  = " << f.resSlope  << ",\n";
            const auto wc = f.waveCut.value_or(obs.waveCut);
            es << "     wave_cut = [" << wc.first << "," << wc.second << "]\n";
            es << "   }";
            entries << entry;
        }
    }
    out << "variable input =\n  [\n"
        << entries.join(",\n") << "\n  ];\n\n";

    QStringList untied;
    for (const auto& u : job.untiedParams) untied << "\"" + u.trimmed() + "\"";

    out << "variable qualies_for_fit = struct{\n";
    out << "  xrange             = " << job.isis.xrange << ",\n";
    out << "  error_estimation   = " << (job.isis.errorEstimation ? 1 : 0) << ",\n";
    out << "  auto_freeze_vsini  = " << (job.isis.autoFreezeVsini ? 1 : 0) << ",\n";
    out << "  add_telluric_model = " << (job.isis.addTelluricModel ? 1 : 0) << ",\n";
    out << "  apply_mask         = " << (job.isis.applyMask ? 1 : 0) << ",\n";
    out << "  xfig_ignore        = " << job.isis.xfigIgnore << ",\n";
    if (job.filterSnr   > 0) out << "  filter_snr         = " << job.filterSnr   << ",\n";
    if (job.requireBlue > 0) out << "  require_blue       = " << job.requireBlue << ",\n";
    out << "  save_model         = \"ascii\",\n";
    out << "  untie = {" << untied.join(",") << "}\n";
    out << "};\n\n";

    out << "variable bpaths = [\"./\"";
    for (const auto& bp : job.basePaths) {
        QString p = bp.trimmed();
        if (p.isEmpty()) continue;
        if (!p.endsWith('/')) p += '/';
        out << ",\n                   \"" << p << "\"";
    }
    out << "];\n";
    out << "qualies_for_fit = struct_combine(qualies_for_fit, struct{bpaths=bpaths});\n";
    out << "modelgrid = search_grid_fit_photometry(bpaths, modelgrid, \"grid.fits\");\n\n";

    out << "variable sout = spectroscopy_automated(input, modelgrid, initial_guess_params_values;;\n"
           "                                       qualies_for_fit);\n";
    out << "vmessage(sprintf(\"- script completed in %.1fs\", _ftime - tscript_start));\n";
    out << "exit;\n";

    return s;
}

SpectralFitResult IsisBackend::run(const SpectralFitJob& job,
                                    LogFn      onLog,
                                    ProgressFn onProgress,
                                    AbortFn    shouldAbort)
{
    SpectralFitResult out;

    try {
        const QString binary = resolveBinary();
        if (binary.isEmpty())
            throw std::runtime_error(
                "ISIS binary not found. Set it in App Settings.");

        const QString workDir = job.outputPath;
        if (workDir.isEmpty() || !QDir().mkpath(workDir))
            throw std::runtime_error("ISIS working directory is invalid.");

        const QString scriptPath = workDir + "/fit.sl";
        {
            QFile f(scriptPath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
                throw std::runtime_error("Cannot write ISIS script file.");
            QTextStream ts(&f);
            ts << generateScript(job);
        }

        ensureAutoepsCache(workDir, onLog);

        if (onLog) {
            onLog(QStringLiteral("ISIS binary : %1").arg(binary));
            onLog(QStringLiteral("Work dir    : %1").arg(workDir));
            onLog(QStringLiteral("Script      : %1").arg(scriptPath));
        }
        if (onProgress) onProgress(QStringLiteral("Starting ISIS..."), -1.0);

        QProcess proc;
        proc.setWorkingDirectory(workDir);
        proc.setProcessChannelMode(QProcess::MergedChannels);

        QByteArray pending;
        double lastFrac = -1.0;
        auto pump = [&](const QByteArray& chunk) {
            pending.append(chunk);
            int idx;
            while ((idx = pending.indexOf('\n')) >= 0) {
                QByteArray line = pending.left(idx);
                pending.remove(0, idx + 1);
                if (line.endsWith('\r')) line.chop(1);
                const QString q = QString::fromLocal8Bit(line);
                if (onLog) onLog(q);
                if (onProgress) {
                    for (const auto& st : kIsisStages)
                        if (q.contains(QLatin1String(st.needle))) {
                            if (st.fraction > lastFrac) {
                                lastFrac = st.fraction;
                                onProgress(QString::fromLatin1(st.label),
                                            st.fraction);
                            }
                            break;
                        }
                }
            }
        };

        proc.start(binary, { QStringLiteral("fit.sl") });
        if (!proc.waitForStarted(10000))
            throw std::runtime_error("Failed to start ISIS process.");
        if (onProgress) onProgress(QStringLiteral("ISIS running..."), -1.0);

        bool aborted = false;
        while (proc.state() != QProcess::NotRunning) {
            proc.waitForReadyRead(100);
            pump(proc.readAllStandardOutput());
            if (shouldAbort && shouldAbort()) {
                aborted = true;
                if (onLog) onLog("Abort requested — terminating ISIS...");
                proc.terminate();
                if (!proc.waitForFinished(3000)) proc.kill();
                proc.waitForFinished(-1);
                break;
            }
        }
        proc.waitForFinished(-1);
        pump(proc.readAllStandardOutput());
        if (!pending.isEmpty() && onLog) {
            onLog(QString::fromLocal8Bit(pending));
            pending.clear();
        }

        if (aborted) {
            out.success = false;
            out.errorMessage = "Aborted by user.";
            return out;
        }
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            out.success = false;
            out.errorMessage = QStringLiteral("ISIS exited with code %1")
                                   .arg(proc.exitCode());
            return out;
        }

        if (onLog) onLog("ISIS finished — parsing results...");
        if (!parseIsisOutputs(out, workDir, job, onLog)) {
            out.success = false;
            out.errorMessage =
                "ISIS completed but result files could not be parsed.";
        }
    } catch (const std::exception& e) {
        out.success      = false;
        out.errorMessage = QString::fromUtf8(e.what());
        if (onLog) onLog("ISIS error: " + out.errorMessage);
    }

    return out;
}

} // namespace astra::fitting