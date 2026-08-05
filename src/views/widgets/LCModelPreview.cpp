#include "LCModelPreview.h"

#include "plotting/qcustomplot.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Veil + spinner drawn on top of the plot while a model evaluation runs. The
// veil keeps the stale curve readable but visibly inactive.
class BusyOverlay : public QWidget {
  public:
    explicit BusyOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        _timer = new QTimer(this);
        _timer->setInterval(40);
        connect(_timer, &QTimer::timeout, this, [this] {
            _angle = (_angle + 14) % 360;
            update();
        });
        hide();
    }

    void start() {
        if (!_timer->isActive())
            _timer->start();
        show();
        raise();
    }
    void stop() {
        _timer->stop();
        hide();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QColor veil = palette().color(QPalette::Window);
        veil.setAlpha(175);
        p.fillRect(rect(), veil);

        const double  r = 16.0;
        const QPointF c = QPointF(width() / 2.0, height() / 2.0);
        const QRectF  box(c.x() - r, c.y() - r, 2 * r, 2 * r);

        QColor track = palette().color(QPalette::WindowText);
        track.setAlpha(60);
        p.setPen(QPen(track, 3.0));
        p.drawArc(box, 0, 360 * 16);

        QColor arc = palette().color(QPalette::Highlight);
        if (!arc.isValid())
            arc = QColor(90, 140, 220);
        p.setPen(QPen(arc, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(box, -_angle * 16, 100 * 16);
    }

  private:
    QTimer *_timer = nullptr;
    int     _angle = 0;
};

double firstNumber(const QString &s, bool *ok = nullptr) {
    static const QRegularExpression re(
        R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eEdD][-+]?\d+)?)");
    const auto m = re.match(s);
    if (!m.hasMatch()) {
        if (ok)
            *ok = false;
        return 0.0;
    }
    return QString(m.captured(0)).replace('d', 'e').replace('D', 'e').toDouble(ok);
}

} // namespace

LCModelPreview::LCModelPreview(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    _status = new QLabel(tr("Model preview"));
    _status->setWordWrap(true);
    _status->setTextFormat(Qt::RichText);
    _status->setStyleSheet("color: gray;");
    root->addWidget(_status);

    _plot = new QCustomPlot;
    _plot->setMinimumWidth(320);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _plot->axisRect()->setupFullAxesBox(true);
    _plot->xAxis->setLabel(tr("Phase"));
    _plot->yAxis->setLabel(tr("Flux"));
    _plot->legend->setVisible(true);
    _plot->legend->setFont(QFont(font().family(), 8));

    _dataGraph = _plot->addGraph();
    _dataGraph->setName(tr("Binned data"));
    _dataGraph->setLineStyle(QCPGraph::lsNone);
    _dataErrors = new QCPErrorBars(_plot->xAxis, _plot->yAxis);
    _dataErrors->setDataPlottable(_dataGraph);
    _dataErrors->removeFromLegend();

    _modelGraph = _plot->addGraph();
    _modelGraph->setName(tr("Model (initial guess)"));
    _modelGraph->setPen(QPen(QColor(220, 65, 65), 2));

    applyTheme();
    root->addWidget(_plot, 1);

    _overlay = new BusyOverlay(this);

    _debounce = new QTimer(this);
    _debounce->setSingleShot(true);
    _debounce->setInterval(350);
    connect(_debounce, &QTimer::timeout, this, &LCModelPreview::launch);
}

LCModelPreview::~LCModelPreview() {
    if (_proc && _proc->state() != QProcess::NotRunning) {
        _proc->kill();
        _proc->waitForFinished(500);
    }
}

void LCModelPreview::applyTheme() {
    const QVariant themeBackground = qApp->property("themeBg");
    const QVariant themeForeground = qApp->property("themeFg");
    const QColor   background      = themeBackground.isValid()
                                         ? themeBackground.value<QColor>()
                                         : palette().color(QPalette::Window);
    const QColor   text            = themeForeground.isValid()
                                         ? themeForeground.value<QColor>()
                                         : palette().color(QPalette::WindowText);
    const QColor   grid =
        text.lightness() > 128 ? QColor(80, 80, 80) : QColor(205, 205, 205);

    _dataGraph->setScatterStyle(
        QCPScatterStyle(QCPScatterStyle::ssDisc, text, text, 3));
    QColor errCol = text;
    errCol.setAlpha(90);
    _dataErrors->setPen(QPen(errCol));

    _plot->setBackground(background);
    for (QCPAxisRect *rect : _plot->axisRects()) {
        rect->setBackground(background);
        for (QCPAxis *axis : rect->axes()) {
            axis->setBasePen(QPen(text));
            axis->setTickPen(QPen(text));
            axis->setSubTickPen(QPen(text));
            axis->setLabelColor(text);
            axis->setTickLabelColor(text);
            axis->grid()->setPen(QPen(grid, 0.5, Qt::DotLine));
        }
    }
    _plot->legend->setBrush(background);
    _plot->legend->setTextColor(text);
    _plot->legend->setBorderPen(QPen(grid));
}

void LCModelPreview::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (_overlay)
        _overlay->setGeometry(rect());
}

void LCModelPreview::setEngine(const QString &binaryPath,
                               const QString &workDir) {
    _binary      = binaryPath;
    _workDir     = workDir;
    _dataWritten = false;
    _dataPath    = QDir(workDir).absoluteFilePath("preview_input.dat");
    _configPath  = QDir(workDir).absoluteFilePath("preview_config.json");
    _outputPath  = QDir(workDir).absoluteFilePath("preview_model.txt");
}

void LCModelPreview::setObservedData(const std::vector<LCFitDataPoint> &points) {
    _observed    = points;
    _dataWritten = false;
    pushObservedToPlot();
}

void LCModelPreview::pushObservedToPlot() {
    QVector<double> x, y, e;
    x.reserve(int(_observed.size()));
    y.reserve(int(_observed.size()));
    e.reserve(int(_observed.size()));
    for (const auto &p : _observed) {
        x.push_back(p.phase);
        y.push_back(p.flux);
        e.push_back(p.fluxError);
    }
    _dataGraph->setData(x, y);
    _dataErrors->setData(e);
    _plot->rescaleAxes();
    _autoScaled = false;
    _plot->replot(QCustomPlot::rpQueuedReplot);
}

void LCModelPreview::setStatus(const QString &text, const QString &colour) {
    _status->setStyleSheet(QString("color: %1;").arg(colour));
    _status->setText(text);
}

void LCModelPreview::showNotice(const QString &text) {
    _debounce->stop();
    _havePending = false;
    setBusy(false);
    setStatus(text, "gray");
}

void LCModelPreview::requestModel(const QJsonObject &config) {
    if (_binary.isEmpty()) {
        showNotice(tr("Preview unavailable: <b>lcurve_re</b> was not found. "
                      "Set the lcurve install directory in "
                      "Settings → Lightcurve Fitting."));
        return;
    }
    if (_observed.empty()) {
        showNotice(tr("Preview unavailable: no binned data points."));
        return;
    }
    _pending     = config;
    _havePending = true;
    setBusy(true);
    _debounce->start();
}

void LCModelPreview::setBusy(bool busy) {
    auto *overlay = static_cast<BusyOverlay *>(_overlay);
    if (busy) {
        overlay->setGeometry(rect());
        overlay->start();
    } else {
        overlay->stop();
    }
}

bool LCModelPreview::writeDataFile() {
    if (_dataWritten)
        return true;
    QFile f(_dataPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream s(&f);
    s.setRealNumberNotation(QTextStream::SmartNotation);
    s.setRealNumberPrecision(17);
    for (const auto &p : _observed)
        s << p.phase << ' ' << p.dPhase << ' ' << p.flux << ' ' << p.fluxError
          << ' ' << p.weight << ' ' << p.factor << '\n';
    f.close();
    _dataWritten = true;
    return true;
}

void LCModelPreview::launch() {
    if (!_havePending)
        return;

    // A superseded evaluation is worthless - drop it and start over. The kill
    // delivers finished() synchronously from waitForFinished(), so onFinished()
    // has to know to ignore it.
    if (_proc && _proc->state() != QProcess::NotRunning) {
        _killing = true;
        _proc->kill();
        _proc->waitForFinished(500);
        _killing = false;
    }

    if (!writeDataFile()) {
        setBusy(false);
        setStatus(tr("Preview failed: could not write %1").arg(_dataPath),
                  "#c46060");
        return;
    }

    QJsonObject cfg     = _pending;
    cfg["data_file_path"]   = _dataPath;
    cfg["output_file_path"] = _outputPath;
    cfg["plot_device"]      = QStringLiteral("none");
    cfg["noise"]            = 0;
    _havePending            = false;

    QFile cf(_configPath);
    if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setBusy(false);
        setStatus(tr("Preview failed: could not write %1").arg(_configPath),
                  "#c46060");
        return;
    }
    cf.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    cf.close();

    if (!_proc) {
        _proc = new QProcess(this);
        _proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(_proc, QOverload<int, QProcess::ExitStatus>::of(
                           &QProcess::finished),
                this, &LCModelPreview::onFinished);
    }
    // A single forward model does not benefit from the GPU path, and keeping
    // it off avoids fighting a running fit for the device.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LCURVE_CUDA"), QStringLiteral("0"));
    _proc->setProcessEnvironment(env);
    _proc->setWorkingDirectory(_workDir);
    _stdout.clear();
    setBusy(true);
    _proc->start(_binary, {_configPath});
}

void LCModelPreview::onFinished(int code, QProcess::ExitStatus status) {
    if (_killing)
        return;
    _stdout = QString::fromUtf8(_proc->readAll());

    // Another change landed while this run was in flight.
    if (_havePending) {
        _debounce->start(0);
        return;
    }

    setBusy(false);

    if (status != QProcess::NormalExit || code != 0) {
        const QStringList lines =
            _stdout.split('\n', Qt::SkipEmptyParts);
        setStatus(tr("Preview failed (exit %1)%2")
                      .arg(code)
                      .arg(lines.isEmpty()
                               ? QString()
                               : ": " + lines.last().toHtmlEscaped()),
                  "#c46060");
        return;
    }

    QFile f(_outputPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(tr("Preview failed: no model written."), "#c46060");
        return;
    }

    QVector<QPair<double, double>> model;
    QTextStream                    s(&f);
    static const QRegularExpression sp(R"(\s+)");
    while (!s.atEnd()) {
        const QString line = s.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
            continue;
        const auto parts = line.split(sp, Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        model.push_back({parts[0].toDouble(), parts[2].toDouble()});
    }
    if (model.isEmpty()) {
        setStatus(tr("Preview failed: model file was empty."), "#c46060");
        return;
    }

    std::sort(model.begin(), model.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QVector<double> mx, my;
    mx.reserve(model.size());
    my.reserve(model.size());
    for (const auto &p : model) {
        mx.push_back(p.first);
        my.push_back(p.second);
    }
    _modelGraph->setData(mx, my);
    // Only the first model reframes the axes; later ones must not undo a zoom
    // the user set up to inspect an eclipse.
    if (!_autoScaled) {
        _plot->rescaleAxes();
        _autoScaled = true;
    }
    _plot->replot(QCustomPlot::rpQueuedReplot);

    // lcurve_re reports the weighted χ² of this model against the data; the
    // reduced value is the number worth showing next to an initial guess.
    QString chiText;
    for (const QString &line : _stdout.split('\n')) {
        if (!line.contains(QStringLiteral("chi**2")))
            continue;
        const auto parts = line.split(',');
        bool       okChi = false, okN = false;
        const double chi = firstNumber(parts.value(0).section('=', 1), &okChi);
        const double n   = firstNumber(parts.value(1).section('=', 1), &okN);
        if (okChi && okN && n > 0)
            chiText = tr(" &nbsp; reduced χ² = %1")
                          .arg(QString::number(chi / n, 'g', 4));
        else if (okChi)
            chiText = tr(" &nbsp; χ² = %1").arg(QString::number(chi, 'g', 4));
        break;
    }
    setStatus(tr("Model for the current initial guess%1").arg(chiText), "gray");
}
