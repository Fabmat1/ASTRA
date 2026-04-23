#include "InteractiveIsisDialog.h"

#include "views/widgets/TerminalView.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"

#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QApplication>
#include <QClipboard>
#include <QSplitter>
#include <QCheckBox>

using astra::fitting::SpectralFitJob;

namespace {

struct IParamEntry { double value = 0.0; bool frozen = false; };

bool parseInteractiveParams(const QString& path,
    QHash<int, QHash<int, QHash<QString, IParamEntry>>>& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    static const QRegularExpression reStellar(
        R"(^stellar\(\d+\)\.d(\d+)_(?:c(\d+)_)?(\w+)$)");
    static const QRegularExpression reWS(R"(\s+)");
    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        const QStringList t = line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (t.size() < 7) continue;
        bool idxOk = false; t[0].toInt(&idxOk);
        if (!idxOk) continue;
        auto m = reStellar.match(t[1]);
        if (!m.hasMatch()) continue;
        const int spec = m.captured(1).toInt();
        const int comp = m.captured(2).isEmpty() ? 1 : m.captured(2).toInt();
        const QString name = m.captured(3);
        out[spec][comp][name] = IParamEntry{
            t[4].toDouble(), t[3].toInt() != 0 };
    }
    return true;
}

bool loadInteractiveData(const QString& path, astra::fitting::FittedSpectrum& fs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    static const QRegularExpression reWS(R"(\s+)");
    QTextStream ts(&f); QString line;
    while (ts.readLineInto(&line)) {
        const QStringList t = line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (t.size() < 4) continue;
        bool ok[4];
        const double wl   = t[0].toDouble(&ok[0]);
        const double flux = t[1].toDouble(&ok[1]);
        const double err  = t[2].toDouble(&ok[2]);
        const int    flag = t[3].toInt   (&ok[3]);
        if (!(ok[0]&&ok[1]&&ok[2]&&ok[3])) continue;
        fs.lambda     .append(wl);
        fs.flux       .append(flux);
        fs.sigma      .append(err);
        fs.ignoreFlag .append(static_cast<uint8_t>(flag != 0 ? 1 : 0));
    }
    return !fs.lambda.isEmpty();
}

bool loadInteractiveModel(const QString& path,
                            QVector<double>& model, QVector<double>& cont)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    static const QRegularExpression reWS(R"(\s+)");
    QTextStream ts(&f); QString line;
    while (ts.readLineInto(&line)) {
        const QStringList t = line.trimmed().split(reWS, Qt::SkipEmptyParts);
        if (t.size() < 3) continue;
        bool ok[3];
        (void)t[0].toDouble(&ok[0]);
        const double m = t[1].toDouble(&ok[1]);
        const double c = t[2].toDouble(&ok[2]);
        if (!(ok[0]&&ok[1]&&ok[2])) continue;
        model.append(m);
        cont.append(c);
    }
    return !model.isEmpty();
}

} // namespace

// ── helpers ────────────────────────────────────────────────────────────

static QString ensureAutoepsCache(const QString& workDir)
{
    const QString cacheRoot = AppPaths::root();
    if (cacheRoot.isEmpty()) return {};
    const QString cache = cacheRoot + "/cache/isis/autoeps";
    QDir().mkpath(cache);
    const QString link = workDir + "/autoeps";
    QFileInfo fi(link);
    if (fi.exists() || fi.isSymLink()) QFile::remove(link);
    QFile::link(cache, link);
    return cache;
}

static bool bpathsContainTelluricGrid(const QStringList& basePaths)
{
    const QString marker = "telluric/LBL_A10_s0_w005_R0300000_T.fits";
    for (const auto& bp : basePaths) {
        QString p = bp.trimmed();
        if (p.isEmpty()) continue;
        if (!p.endsWith('/')) p += '/';
        if (QFile::exists(p + marker)) return true;
    }
    return false;
}

QString InteractiveIsisDialog::resolveBinary()
{
    AppSettings settings;
    const QString custom = settings.isisBinaryPath().trimmed();
    if (!custom.isEmpty() && QFileInfo(custom).isExecutable())
        return custom;
    return QStandardPaths::findExecutable("isis");
}

QString InteractiveIsisDialog::generateHeader(const SpectralFitJob& job,
                                               const QString& workDir)
{
    QString s;
    QTextStream o(&s);
    o.setRealNumberPrecision(10);

    // Flatten observation groups (one entry per SpectrumFile).
    struct Flat { astra::fitting::SpectrumFile f; astra::fitting::Observation obs; };
    QVector<Flat> flat;
    for (const auto& obs : job.observations)
        for (const auto& f : obs.files)
            flat.append({ f, obs });

    o << "% Generated by ASTRA — interactive ISIS spectroscopy session\n";
    o << "require(\"stellar_isisscripts.sl\");\n\n";

    // specs[]
    QStringList specs;
    for (const auto& e : flat) specs << "\"" + e.f.filename + "\"";
    o << "variable specs = [\n  " << specs.join(",\n  ") << "\n];\n";
    o << "variable len_sets = length([specs]);\n\n";

    // resolution arrays
    QStringList ros, rss;
    for (const auto& e : flat) {
        ros << QString::number(e.f.resOffset);
        rss << QString::number(e.f.resSlope);
    }
    o << "variable res_offset = [" << ros.join(", ") << "];\n";
    o << "variable res_slope  = [" << rss.join(", ") << "];\n\n";

    // wave_trim (List_Type)
    o << "variable wave_trim = {};\n";
    for (int i = 0; i < flat.size(); ++i) {
        const auto wc = flat[i].f.waveCut.value_or(flat[i].obs.waveCut);
        o << "list_append(wave_trim, [" << wc.first << ", " << wc.second << "]);\n";
    }
    o << "\n";

    // spectype
    QStringList st;
    for (const auto& e : flat) st << "\"" + e.f.spectype + "\"";
    o << "variable spectype = [" << st.join(", ") << "];\n\n";

    // cspline anchors per spectrum
    o << "variable cspline_anchorpoints = Array_Type[len_sets];\n";
    for (int i = 0; i < flat.size(); ++i) {
        const auto anchors = flat[i].f.anchors.value_or(flat[i].obs.anchors);
        QStringList parts;
        for (const auto& a : anchors)
            parts << QString("[%1:%2:%3]").arg(a.wlLow).arg(a.wlHigh).arg(a.spacing);
        if (parts.isEmpty()) parts << "[3500:5500:100]";
        o << "cspline_anchorpoints[" << i << "] = [" << parts.join(",") << "];\n";
    }
    o << "\n";

    o << "ifnot(len_sets==length([res_offset])==length([res_slope]))\n"
      << "  throw UsageError, \"specs/res_offset/res_slope length mismatch\";\n\n";

    // grid directories
    QStringList grids;
    for (const auto& c : job.components) {
        QString g = c.gridPath.trimmed();
        if (g.isEmpty()) continue;
        if (!g.endsWith('/')) g += '/';
        grids << "\"" + g + "\"";
    }
    if (grids.isEmpty()) grids << "\"sdB/processed/\"";
    o << "variable griddirectories = [" << grids.join(", ") << "];\n\n";

    // working dir & subdirs
    QString wd = workDir;
    if (!wd.endsWith('/')) wd += '/';
    o << "variable wd = \"" << wd << "\";\n";
    o << "() = chdir(wd);\n";
    o << "() = system(sprintf(\"mkdir -p %sfitsfiles\", wd));\n";
    o << "() = system(sprintf(\"mkdir -p %slists\",      wd));\n";
    o << "() = system(sprintf(\"mkdir -p %sparams\",     wd));\n";
    o << "() = system(sprintf(\"mkdir -p %sresults\",    wd));\n\n";

    // bpaths
    o << "variable i, bpaths, len_comp = length(griddirectories);\n";
    o << "bpaths = [\"./\"";
    for (const auto& bp : job.basePaths) {
        QString p = bp.trimmed();
        if (p.isEmpty()) continue;
        if (!p.endsWith('/')) p += '/';
        o << ",\n          \"" << p << "\"";
    }
    o << "];\n";
    o << "griddirectories = search_grid_fit_photometry(bpaths, griddirectories, \"grid.fits\");\n";
    o << "variable id;\n\n";

    // ── RV-spline correction (configurable from ASTRA) ─────────
    const auto& inter = job.isisInteractive;
    QString anchors = inter.rvAnchors.trimmed();
    if (anchors.isEmpty()) anchors = "[[3000:6500:500],[6500:25500:1000]]";

    o << "% RV-spline correction for wavelength calibration\n";
    o << "variable rvspline_anchorpoints = Array_Type[len_sets];\n";
    o << "rvspline_anchorpoints[[0:len_sets-1]] = " << anchors << ";\n";
    o << "variable rvcorr = " << (inter.rvCorrection ? 1 : 0) << ";\n";
    o << "if(rvcorr)\n";
    o << "{\n";
    o << "  variable lt = Array_Type[len_sets];\n";
    o << "  variable ft = Array_Type[len_sets];\n";
    o << "  _for id(1, len_sets, 1)\n";
    o << "  {\n";
    o << "    variable t = read_spectrum(specs[id-1], spectype[id-1]);\n";
    o << "    lt[id-1] = rvspline_anchorpoints[id-1];\n";
    o << "    lt[id-1] = (lt[id-1])[where(t.l[0]-1 <= lt[id-1] <= t.l[-1]+1)];\n";
    o << "    lt[id-1] = (lt[id-1])[where(abs(t.l[0]-lt[id-1])>200.)];\n";
    o << "    lt[id-1] = (lt[id-1])[where(abs(t.l[-1]-lt[id-1])>200.)];\n";
    o << "    lt[id-1] = union(t.l[0]-1, t.l[-1]+1, lt[id-1]);\n";
    o << "    ft[id-1] = 0.*lt[id-1];\n";
    o << "  }\n";
    o << "  rvcorr = {lt, ft};\n";
    o << "}\n";
    o << "else rvcorr = NULL;\n\n";

    // ── Macrobroadening model ──────────────────────────────────
    const QString mb = inter.macrobroadening.isEmpty() ? "r" : inter.macrobroadening;
    o << "variable macrobroadening = String_Type[len_comp];\n";
    o << "macrobroadening[[0:len_comp-1]] = \"" << mb << "\";\n\n";

    // ── Initialize fit ─────────────────────────────────────────
    o << "initialize_grid_fit_spectroscopy(griddirectories;\n"
      << "                                 datasets=len_sets,\n"
      << "                                 num_slaves=1,\n"
      << "                                 macrobroadening=macrobroadening,\n"
      << "                                 rvcorr=rvcorr);\n";
    o << "fit_fun(\"stellar\");\n\n";

    // ── Telluric fitting (only if grid is available on disk) ──
    if (bpathsContainTelluricGrid(job.basePaths)) {
        o << "% telluric grid found in bpaths → including telluric model\n";
        o << "initialize_telluric(search_grid_fit_photometry(bpaths, [\"telluric/\"], "
             "\"LBL_A10_s0_w005_R0300000_T.fits\")[0];\n"
          << "                    datasets=len_sets);\n";
        o << "fit_fun(\"stellar*telluric\");\n\n";
    } else {
        o << "% telluric grid not found in bpaths — skipping telluric model\n\n";
        o << "set_par(sprintf(\"telluric(1).d%d_airmass\",id),0,1; min=0, max=3);\n";
        o << "set_par(sprintf(\"telluric(1).d%d_pwv\",id),0,1);\n";
        o << "set_par(sprintf(\"telluric(1).d%d_barycorr\",id),0,1);\n";
    }

    return s;
}

QString InteractiveIsisDialog::generateScript(const SpectralFitJob& job,
                                               const QString& workDir)
{
    QString out = generateHeader(job, workDir);
    QFile body(":/scripts/spectroscopy_interactive_body.sl");
    if (body.open(QIODevice::ReadOnly | QIODevice::Text)) {
        out += QString::fromUtf8(body.readAll());
    } else {
        out += "% ERROR: :/scripts/spectroscopy_interactive_body.sl not found.\n"
               "% Add the interactive script body to resources and rebuild.\n";
    }
    return out;
}

// ── dialog ─────────────────────────────────────────────────────────────

InteractiveIsisDialog::InteractiveIsisDialog(const SpectralFitJob& job,
                                              QWidget* parent)
    : QDialog(parent), _job(job)
{
    setWindowTitle("Interactive ISIS session");
    resize(1200, 720);
    setModal(false);

    _workDir = _job.outputPath;
    if (_workDir.isEmpty()) _workDir = QDir::tempPath() + "/ASTRA-interactive";
    QDir().mkpath(_workDir);
    _scriptPath = _workDir + "/fit.sl";

    auto* v = new QVBoxLayout(this);

    _status = new QLabel;
    v->addWidget(_status);

    _term = new TerminalView;
    connect(_term, &TerminalView::lineEntered,
            this,  &InteractiveIsisDialog::onLineEntered);

    _macros = new IsisMacroPanel;
    _macros->setMinimumWidth(260);
    connect(_macros, &IsisMacroPanel::runCommands,
            this,    &InteractiveIsisDialog::sendCommands);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(_term);
    splitter->addWidget(_macros);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    v->addWidget(splitter, 1);

    auto* row = new QHBoxLayout;
    _startBtn     = new QPushButton("Start ISIS");
    _stopBtn      = new QPushButton("Stop");
    _showBtn      = new QPushButton("Show script...");
    _extractBtn = new QPushButton("Extract fit");
    _extractBtn->setEnabled(false);
    _extractBtn->setToolTip(
        "Import current params + rebinned/model spectra into ASTRA.\n"
        "Run 'save_quick;' and 'write_spec(; ascii);' in ISIS first.");

    _hotkeyToggle = new QCheckBox("Hotkey mode");
    _closeBtn     = new QPushButton("Close");
    _stopBtn->setEnabled(false);
    _hotkeyToggle->setToolTip(
        "Send each keystroke to ISIS as its own line, for 'hotkeys;' sessions.");

    connect(_startBtn,     &QPushButton::clicked, this, &InteractiveIsisDialog::onStart);
    connect(_stopBtn,      &QPushButton::clicked, this, &InteractiveIsisDialog::onStop);
    connect(_showBtn,      &QPushButton::clicked, this, &InteractiveIsisDialog::onShowScript);
    connect(_closeBtn,     &QPushButton::clicked, this, &QDialog::accept);
    connect(_hotkeyToggle, &QCheckBox::toggled,   this, [this](bool on){
        _term->setHotkeyMode(on);
        if (on) _term->focusInput();
    });
    connect(_extractBtn, &QPushButton::clicked,
        this, &InteractiveIsisDialog::onExtractFit);

    row->addWidget(_startBtn);
    row->addWidget(_stopBtn);
    row->addWidget(_showBtn);
    row->addWidget(_extractBtn);
    row->addWidget(_hotkeyToggle);
    row->addStretch();
    row->addWidget(_closeBtn);
    v->addLayout(row);

    _extractPoll = new QTimer(this);
    _extractPoll->setInterval(2000);
    connect(_extractPoll, &QTimer::timeout,
            this, &InteractiveIsisDialog::checkExtractAvailable);
    _extractPoll->start();
    checkExtractAvailable();

    appendStatus(QString("Work dir: %1").arg(_workDir));
    appendStatus(QString("Binary  : %1").arg(
        resolveBinary().isEmpty() ? "<not found>" : resolveBinary()));
}

InteractiveIsisDialog::~InteractiveIsisDialog()
{
    if (_proc && _proc->state() != QProcess::NotRunning) {
        _proc->terminate();
        _proc->waitForFinished(1500);
        if (_proc->state() != QProcess::NotRunning) _proc->kill();
        _proc->waitForFinished(-1);
    }
}

void InteractiveIsisDialog::closeEvent(QCloseEvent* e)
{
    if (_proc && _proc->state() != QProcess::NotRunning) {
        const auto r = QMessageBox::question(this, "ISIS is still running",
            "Stop the ISIS session and close?",
            QMessageBox::Yes | QMessageBox::No);
        if (r != QMessageBox::Yes) { e->ignore(); return; }
        onStop();
    }
    QDialog::closeEvent(e);
}

void InteractiveIsisDialog::appendStatus(const QString& s)
{
    _status->setText(s);
    _term->appendStatusLine(s);
}

void InteractiveIsisDialog::onStart()
{
    const QString binary = resolveBinary();
    if (binary.isEmpty()) {
        QMessageBox::warning(this, "ISIS not found",
            "ISIS binary is not configured. Set it in App Settings.");
        return;
    }

    // Write script
    {
        QFile f(_scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Error",
                "Could not write " + _scriptPath);
            return;
        }
        QTextStream ts(&f);
        ts << generateScript(_job, _workDir);
    }
    ensureAutoepsCache(_workDir);

    if (!_proc) {
        _proc = new QProcess(this);
        _proc->setWorkingDirectory(_workDir);
        _proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(_proc, &QProcess::readyReadStandardOutput,
                this,  &InteractiveIsisDialog::onReadyRead);
        connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,  &InteractiveIsisDialog::onProcessFinished);
    }

    QString prog = binary;
    QStringList args { "fit.sl" };

    // Prepend stdbuf to force unbuffered output when available.
    // Without this, ISIS's libc stdout is block-buffered off a TTY and
    // prompts / progress lines trickle out in large chunks.
    const QString stdbuf = QStandardPaths::findExecutable("stdbuf");
    if (!stdbuf.isEmpty()) {
        prog = stdbuf;
        args = { "-o0", "-e0", binary, "fit.sl" };
    }

    appendStatus(QString("Starting: %1 %2").arg(prog, args.join(' ')));
    _startBtn->setEnabled(false);
    _stopBtn ->setEnabled(true);
    _term->setInputEnabled(true);
    _term->focusInput();

    _proc->start(prog, args);
    if (!_proc->waitForStarted(10000)) {
        appendStatus("Failed to start ISIS.");
        _startBtn->setEnabled(true);
        _stopBtn ->setEnabled(false);
        _term->setInputEnabled(false);
    }
}

void InteractiveIsisDialog::onStop()
{
    if (!_proc || _proc->state() == QProcess::NotRunning) return;
    appendStatus("Stopping ISIS…");
    _proc->terminate();
    if (!_proc->waitForFinished(3000)) _proc->kill();
    _proc->waitForFinished(-1);
}

void InteractiveIsisDialog::onReadyRead()
{
    if (!_proc) return;
    const QByteArray chunk = _proc->readAllStandardOutput();
    if (!chunk.isEmpty()) _term->appendOutput(QString::fromLocal8Bit(chunk));
}

void InteractiveIsisDialog::onProcessFinished(int exitCode,
                                               QProcess::ExitStatus status)
{
    onReadyRead();  // drain tail
    const QString msg = (status == QProcess::NormalExit)
        ? QString("ISIS exited normally (code %1).").arg(exitCode)
        : QString("ISIS crashed (code %1).").arg(exitCode);
    appendStatus(msg);
    _startBtn->setEnabled(true);
    _stopBtn ->setEnabled(false);
    _term->setInputEnabled(false);
}

void InteractiveIsisDialog::onLineEntered(const QString& line)
{
    if (!_proc || _proc->state() != QProcess::Running) return;
    // Echo locally so the user sees what they typed in context.
    _term->appendOutput(line + "\n");
    QByteArray payload = line.toUtf8();
    payload.append('\n');
    _proc->write(payload);
}

void InteractiveIsisDialog::onShowScript()
{
    const QString body = generateScript(_job, _workDir);

    QDialog dlg(this);
    dlg.setWindowTitle("Generated fit.sl");
    dlg.resize(820, 640);

    auto* v = new QVBoxLayout(&dlg);
    auto* txt = new QPlainTextEdit;
    txt->setReadOnly(true);
    txt->setPlainText(body);
    txt->setStyleSheet("font-family:monospace; font-size:11px;");
    v->addWidget(txt, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* copy = bb->addButton("Copy", QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [body]{
        QApplication::clipboard()->setText(body);
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    dlg.exec();
}

void InteractiveIsisDialog::sendCommands(const QStringList& commands)
{
    if (!_proc || _proc->state() != QProcess::Running) {
        appendStatus("ISIS is not running.");
        return;
    }
    for (const QString& c : commands) {
        if (c.isEmpty()) continue;
        _term->appendOutput("> " + c + "\n");
        QByteArray payload = c.toUtf8();
        payload.append('\n');
        _proc->write(payload);
    }
    _term->focusInput();
}

void InteractiveIsisDialog::checkExtractAvailable()
{
    const bool ok =
        QFile::exists(_workDir + "/params/params") &&
        QFile::exists(_workDir + "/results/data_d1.dat") &&
        QFile::exists(_workDir + "/results/model_d1.dat");
    if (_extractBtn && _extractBtn->isEnabled() != ok)
        _extractBtn->setEnabled(ok);
}

void InteractiveIsisDialog::onExtractFit()
{
    QHash<int, QHash<int, QHash<QString, IParamEntry>>> params;
    if (!parseInteractiveParams(_workDir + "/params/params", params)) {
        appendStatus("Could not read params/params.");
        return;
    }

    QVector<QString> submittedIds;
    for (const auto& obs : _job.observations)
        for (const auto& fi : obs.files)
            submittedIds.append(fi.spectrumId);

    const int nSpec = submittedIds.size();
    const int nComp = std::max(1, int(_job.components.size()));

    QSet<QString> untied;
    for (const auto& u : _job.untiedParams)
        untied.insert(u.trimmed().toLower());

    auto getVal = [&](int s, int c, const QString& name)
        -> std::optional<IParamEntry> {
        if (!params.contains(s))        return std::nullopt;
        if (!params[s].contains(c))     return std::nullopt;
        const auto& m = params[s][c];
        if (!m.contains(name))          return std::nullopt;
        return m.value(name);
    };

    astra::fitting::SpectralFitResult result;
    result.success   = true;
    result.converged = true;

    for (int c = 1; c <= nComp; ++c) {
        astra::fitting::FittedComponent fc;
        auto addParam = [&](const QString& isisName,
                            const QString& astraKey,
                            QVector<astra::fitting::FittedParameter>& dest)
        {
            const bool isUntied = untied.contains(astraKey);
            const int  count    = isUntied ? nSpec : 1;
            for (int s = 1; s <= count; ++s) {
                astra::fitting::FittedParameter fp;
                if (auto e = getVal(s, c, isisName)) {
                    fp.value  = e->value;
                    fp.frozen = e->frozen;
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
        result.components.append(fc);
    }

    for (int s = 0; s < nSpec; ++s) {
        astra::fitting::FittedSpectrum fs;
        fs.spectrumId = submittedIds[s];

        const QString dPath = QString("%1/results/data_d%2.dat")
                                 .arg(_workDir).arg(s + 1);
        const QString mPath = QString("%1/results/model_d%2.dat")
                                 .arg(_workDir).arg(s + 1);
        if (!QFile::exists(dPath) || !QFile::exists(mPath)
            || !loadInteractiveData(dPath, fs))
        {
            result.rejectedFiles.append(fs.spectrumId);
            continue;
        }
        QVector<double> model, cont;
        if (!loadInteractiveModel(mPath, model, cont)) {
            result.rejectedFiles.append(fs.spectrumId);
            continue;
        }
        const int n = std::min(fs.lambda.size(), model.size());
        fs.lambda    .resize(n);
        fs.flux      .resize(n);
        fs.sigma     .resize(n);
        fs.ignoreFlag.resize(n);

        // ISIS interactive writes *continuum-normalized* data and model (both ~1)
        // plus the continuum level as a separate column. ASTRA stores absolute
        // fluxes and derives the normalized view by dividing by continuum, so
        // multiply everything back up on import.
        QVector<double> absFlux(n), absModel(n), absSigma(n), contVec(n);
        for (int i = 0; i < n; ++i) {
            const double c = cont[i];
            contVec [i] = c;
            absFlux [i] = fs.flux [i] * c;
            absModel[i] = model    [i] * c;
            absSigma[i] = fs.sigma [i] * c;
        }
        fs.flux      = absFlux;
        fs.sigma     = absSigma;
        fs.model     = absModel;
        fs.continuum = contVec;

        if (s == 0) result.nDataPoints = n;
        result.spectra.append(fs);
    }

    if (result.spectra.isEmpty()) {
        appendStatus("Nothing to extract — no matching spectrum files found.");
        return;
    }

    appendStatus(QString("Extracted %1 spectrum%2 — saving to ASTRA.")
                     .arg(result.spectra.size())
                     .arg(result.spectra.size() == 1 ? "" : "s"));
    emit fitExtracted(result, _job);
}