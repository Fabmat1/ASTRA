#include "LightcurveFetcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QStringTokenizer>
#include <QProcessEnvironment>
#include <cmath>

#include "Logger.h"

#ifndef ASTRA_LCQUERY_SCRIPT
#  define ASTRA_LCQUERY_SCRIPT ""
#endif
#ifndef ASTRA_LCQUERY_REQS
#  define ASTRA_LCQUERY_REQS   ""
#endif

// ─────────────────────────────────────────────────────────────────────
LightcurveFetcher::LightcurveFetcher(QObject* parent)
    : QObject(parent)
    , _python(defaultPython())
    , _script(defaultScript())
{}

LightcurveFetcher::~LightcurveFetcher()
{
    if (_proc && _proc->state() != QProcess::NotRunning) {
        _proc->kill();
        _proc->waitForFinished(500);
    }
}

QString LightcurveFetcher::defaultPython()
{
#ifdef Q_OS_WIN
    return QStringLiteral("python");
#else
    return QStringLiteral("python3");
#endif
}

QString LightcurveFetcher::defaultScript()
{
    QString baked = QString::fromUtf8(ASTRA_LCQUERY_SCRIPT);
    if (!baked.isEmpty() && QFileInfo::exists(baked)) return baked;

    // Fallback: next to the binary (useful after install)
    QString side = QCoreApplication::applicationDirPath()
                 + "/external/lightcurvequery/lightcurvequery.py";
    if (QFileInfo::exists(side)) return side;
    return baked; // return the (possibly missing) configured path for diagnostics
}

QString LightcurveFetcher::defaultRequirements()
{
    return QString::fromUtf8(ASTRA_LCQUERY_REQS);
}

// ─────────────────────────────────────────────────────────────────────
QString LightcurveFetcher::checkAvailable() const
{
    if (_script.isEmpty() || !QFileInfo::exists(_script)) {
        return tr("lightcurvequery script not found.\n"
                  "Did you run `git submodule update --init`?\n"
                  "Expected at: %1").arg(_script);
    }
    if (_python.isEmpty())
        return tr("No Python interpreter configured.");

    // Quick "python -V" probe
    {
        QProcess p;
        p.start(_python, {"-c", "import sys; print(sys.version)"});
        if (!p.waitForStarted(2000) || !p.waitForFinished(5000))
            return tr("Could not invoke Python interpreter: %1").arg(_python);
        if (p.exitCode() != 0)
            return tr("Python sanity check failed (exit %1).").arg(p.exitCode());
    }

    // Required packages probe
    {
        QProcess p;
        p.start(_python, {"-c",
            "import requests, numpy, pandas, astropy, astroquery, "
            "lightkurve, ztfquery, gatspy"});
        p.waitForFinished(20000);
        if (p.exitCode() != 0) {
            QString stderr_ = QString::fromUtf8(p.readAllStandardError()).trimmed();
            return tr("Missing Python packages needed by lightcurvequery.\n\n"
                      "Install them with:\n"
                      "    %1 -m pip install -r %2\n\n"
                      "Python reported:\n%3")
                .arg(_python, defaultRequirements(), stderr_);
        }
    }
    return {};
}

bool LightcurveFetcher::isRunning() const
{
    return _proc && _proc->state() != QProcess::NotRunning;
}

// ─────────────────────────────────────────────────────────────────────
void LightcurveFetcher::start(const QString& gaiaId, const Options& opt)
{
    if (isRunning()) {
        emit failed(tr("Fetcher is already running."));
        return;
    }
    if (gaiaId.isEmpty()) {
        emit failed(tr("No Gaia ID provided."));
        return;
    }

    if (QString err = checkAvailable(); !err.isEmpty()) {
        emit failed(err);
        return;
    }

    if (!_proc) {
        _proc = new QProcess(this);
        _proc->setProcessChannelMode(QProcess::SeparateChannels);
        connect(_proc, &QProcess::readyReadStandardOutput,
                this, &LightcurveFetcher::onStdout);
        connect(_proc, &QProcess::readyReadStandardError,
                this, &LightcurveFetcher::onStderr);
        connect(_proc, &QProcess::finished,
                this, &LightcurveFetcher::onFinished);
        connect(_proc, &QProcess::errorOccurred,
                this, &LightcurveFetcher::onErrorOccurred);
    }

    if (_workdir.isEmpty())
        _workdir = QDir::temp().absoluteFilePath("astra_lcquery");
    QDir().mkpath(_workdir);
    _proc->setWorkingDirectory(_workdir);

    // Force unbuffered child stdio so log lines arrive promptly.
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("FORCE_COLOR",     "1"); 
    env.insert("COLUMNS",         "120");
    env.insert("TERM",            "xterm-256color");

    if (!_atlasToken.isEmpty())
        env.insert("ATLASFORCED_SECRET_KEY", _atlasToken);

    if (!_blackgemScript.isEmpty())
        env.insert("BLACKGEM_QUERYSCRIPT_LOCATION", _blackgemScript);
    else
        env.insert("BLACKGEM_QUERYSCRIPT_LOCATION", "DISABLED");

    _proc->setProcessEnvironment(env);

    QStringList args;
    args << _script << gaiaId;

    const auto has = [&](const QString& s){ return opt.sources.contains(s); };
    if (!has("TESS"))     args << "--skip-tess";
    if (!has("ZTF"))      args << "--skip-ztf";
    if (!has("ATLAS"))    args << "--skip-atlas";
    if (!has("Gaia"))     args << "--skip-gaia";
    if (!has("BlackGEM")) args << "--skip-bg";

    if (opt.noPlot)      args << "--no-plot";
    if (opt.noBinning)   args << "--no-binning";
    if (opt.noWhitening) args << "--no-whitening";

    if (opt.trimTess > 0.0)
        args << "--trim-tess"        << QString::number(opt.trimTess,    'g', 4);
    if (opt.ztfInnerArc > 0)
        args << "--ztf-inner-radius" << QString::number(opt.ztfInnerArc, 'g', 4);
    if (opt.ztfOuterArc > 0)
        args << "--ztf-outer-radius" << QString::number(opt.ztfOuterArc, 'g', 4);

    _outBuf.clear();
    _errBuf.clear();

    LOG_INFO("LCQuery", QString("Launching: %1 %2").arg(_python, args.join(' ')));
    LOG_INFO("LCQuery", QString("Working dir: %1").arg(_workdir));
    emit started();
    _proc->start(_python, args);
}

void LightcurveFetcher::cancel()
{
    if (!isRunning()) return;
    _proc->terminate();
    if (!_proc->waitForFinished(1500))
        _proc->kill();
}

// ─────────────────────────────────────────────────────────────────────
static void emitLineChunks(QByteArray& buf, std::function<void(QString)> emit_)
{
    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        QString s = QString::fromUtf8(buf.left(nl)).trimmed();
        buf.remove(0, nl + 1);
        if (!s.isEmpty()) emit_(std::move(s));
    }
}

// LightcurveFetcher.cpp — replace the bodies of onStdout / onStderr
void LightcurveFetcher::onStdout()
{
    const QByteArray chunk = _proc->readAllStandardOutput();
    if (chunk.isEmpty()) return;
    emit rawOutput(chunk);                                          // for terminal

    _outBuf += chunk;
    int nl;
    while ((nl = _outBuf.indexOf('\n')) >= 0) {
        QString s = QString::fromUtf8(_outBuf.left(nl)).trimmed();
        _outBuf.remove(0, nl + 1);
        if (!s.isEmpty()) emit logLine(s);                          // for plain log subscribers
    }
}

void LightcurveFetcher::onStderr()
{
    const QByteArray chunk = _proc->readAllStandardError();
    if (chunk.isEmpty()) return;
    emit rawOutput(chunk);                                          // merge stderr into terminal

    _errBuf += chunk;
    int nl;
    while ((nl = _errBuf.indexOf('\n')) >= 0) {
        QString s = QString::fromUtf8(_errBuf.left(nl)).trimmed();
        _errBuf.remove(0, nl + 1);
        if (!s.isEmpty()) emit logLine(s);
    }
}
void LightcurveFetcher::onFinished(int code, QProcess::ExitStatus status)
{
    onStdout(); onStderr();
    bool ok = (status == QProcess::NormalExit) && code == 0;
    emit finished(code, ok);
}

void LightcurveFetcher::onErrorOccurred(QProcess::ProcessError err)
{
    emit failed(tr("QProcess error (%1).").arg(int(err)));
}

QHash<QString,QString>
LightcurveFetcher::expectedOutputFiles(const QString& gaiaId) const
{
    QString base = QDir(_workdir).filePath("lightcurves/" + gaiaId);
    return {
        {"TESS",     base + "/tess_lc.txt"},
        {"ZTF",      base + "/ztf_lc.txt"},
        {"ATLAS",    base + "/atlas_lc.txt"},
        {"Gaia",     base + "/gaia_lc.txt"},
        {"BlackGEM", base + "/bg_lc.txt"},
    };
}

// ─────────────────────────────────────────────────────────────────────
// Output file parser
// ─────────────────────────────────────────────────────────────────────
//
// The format varies a little by source — but they're all comma-separated:
//   TESS     : time(BJD-2457000), flux, flux_err                     (3 cols)
//   ZTF      : mjd, flux, flux_err, filter                           (4 cols)
//   ATLAS    : mjd, flux, flux_err, filter                           (4 cols)
//   Gaia     : MJD, flux, flux_err, filter (G/BP/RP)                 (4 cols)
//   BlackGEM : MJD_OBS, FNU_OPT, FNUERRTOT_OPT, FILTER               (4 cols)
//
// Files that the Python pipeline failed to populate contain the literal
// "NaN, NaN, NaN, NaN, NaN, NaN, NaN" — we treat those as empty.

std::vector<LightcurvePoint>
LightcurveFetcher::parseOutputFile(const QString& path,
                                   const QString& source,
                                   TimeScale*     outScale)
{
    std::vector<LightcurvePoint> out;
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    const bool      isTess = source.compare("TESS", Qt::CaseInsensitive) == 0;
    const TimeScale scale  = isTess ? TimeScale::BTJD : TimeScale::MJD;
    if (outScale) *outScale = scale;

    QTextStream in(&f);
    int lineNo = 0;

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty() || line.startsWith('#')) continue;

        const QStringList cols = line.split(',', Qt::SkipEmptyParts);
        if (cols.size() < 3) continue;

        bool   tOk = false, yOk = false, eOk = false;
        const double t = cols[0].trimmed().toDouble(&tOk);
        const double y = cols[1].trimmed().toDouble(&yOk);
        const double e = cols[2].trimmed().toDouble(&eOk);

        if (!tOk || !yOk || !eOk)           continue;
        if (!std::isfinite(t) || !std::isfinite(y)) continue;

        LightcurvePoint pt;
        pt.time      = Time(t, scale);
        pt.flux      = y;
        pt.fluxError = (eOk && std::isfinite(e)) ? e : 0.0;
        pt.filter    = (cols.size() >= 4) ? cols[3].trimmed()
                     : isTess             ? QStringLiteral("TESS")
                     :                      QString{};

        out.push_back(std::move(pt));
    }

    LOG_INFO("LCQuery",
             QString("Parsed %1: %2 points from %3").arg(source).arg(out.size()).arg(path));
    return out;
}

void LightcurveFetcher::checkAvailableAsync()
{
    if (_script.isEmpty() || !QFileInfo::exists(_script)) {
        emit availabilityChecked(false,
            tr("lightcurvequery script not found.\nExpected at: %1").arg(_script));
        return;
    }
    if (_python.isEmpty()) {
        emit availabilityChecked(false, tr("No Python interpreter configured."));
        return;
    }

    if (!_probeProc) {
        _probeProc = new QProcess(this);
        _probeProc->setProcessChannelMode(QProcess::MergedChannels);
        connect(_probeProc, &QProcess::finished,
                this, &LightcurveFetcher::onProbeFinished);
        connect(_probeProc, &QProcess::errorOccurred,
                this, &LightcurveFetcher::onProbeErrorOccurred);
    }
    if (_probeProc->state() != QProcess::NotRunning) {
        _probeProc->kill();
        _probeProc->waitForFinished(200);
    }
    _probeStage = 0;
    _probeProc->start(_python, {"-c", "import sys; sys.exit(0)"});
}

void LightcurveFetcher::onProbeFinished(int code, QProcess::ExitStatus status)
{
    const QString output = QString::fromUtf8(_probeProc->readAll()).trimmed();

    if (status != QProcess::NormalExit || code != 0) {
        QString msg = (_probeStage == 0)
            ? tr("Python sanity check failed (exit %1).\n%2").arg(code).arg(output)
            : tr("Missing Python packages required by lightcurvequery.\n"
                 "Install with:\n    %1 -m pip install -r %2\n\n%3")
                 .arg(_python, defaultRequirements(), output);
        emit availabilityChecked(false, msg);
        return;
    }
    if (_probeStage == 0) {
        _probeStage = 1;
        _probeProc->start(_python, {"-c",
            "import requests, numpy, pandas, astropy, astroquery, "
            "lightkurve, ztfquery, gatspy"});
        return;
    }
    emit availabilityChecked(true, {});
}

void LightcurveFetcher::onProbeErrorOccurred(QProcess::ProcessError)
{
    emit availabilityChecked(false,
        tr("Could not invoke Python: %1").arg(_python));
}