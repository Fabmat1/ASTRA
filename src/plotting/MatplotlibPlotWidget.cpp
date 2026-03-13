#include "MatplotlibPlotWidget.h"
#include "utils/Logger.h"

#include <QJsonArray>
#include <QStandardPaths>
#include <QFileInfo>
#include <QApplication>

static const char* CAT = "MatplotlibPlot";

// ============================================================================
// Construction
// ============================================================================

MatplotlibPlotWidget::MatplotlibPlotWidget(QWidget* parent)
    : QWidget(parent)
    , _process(nullptr)
    , _inputFile(nullptr)
    , _autoRerender(true)
    , _hasLastRequest(false)
{
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(0, 0, 0, 0);
    _layout->setAlignment(Qt::AlignCenter);

    // Image display
    _imageLabel = new ScalableImageLabel(this);
    _imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    _layout->addWidget(_imageLabel, 1);

    // Spinner (centered overlay)
    _spinner = new SpinnerWidget(this);
    _spinner->hide();

    // Loading text
    _loadingLabel = new QLabel("Rendering plot...", this);
    _loadingLabel->setAlignment(Qt::AlignCenter);
    _loadingLabel->setStyleSheet(
        "color: gray; font-style: italic; font-size: 13px;");
    _loadingLabel->hide();

    // Status / error / placeholder label
    _statusLabel = new QLabel(this);
    _statusLabel->setAlignment(Qt::AlignCenter);
    _statusLabel->setWordWrap(true);
    _statusLabel->setStyleSheet(
        "color: gray; font-style: italic; font-size: 14px; padding: 20px;");
    _statusLabel->hide();

    // Resize debounce timer
    _resizeDebounce = new QTimer(this);
    _resizeDebounce->setSingleShot(true);
    _resizeDebounce->setInterval(300); // ms
    connect(_resizeDebounce, &QTimer::timeout, this, [this]() {
        if (_autoRerender && _hasLastRequest && width() > 50 && height() > 50) {
            // Update size in request and re-render
            _lastRequest.widthPx  = width()  * devicePixelRatioF();
            _lastRequest.heightPx = height() * devicePixelRatioF();
            requestPlot(_lastRequest);
        }
    });

    // Default python
    _pythonExe = findPython();

    // Default script dir: <app_dir>/scripts/plotting/
    _scriptDir = QCoreApplication::applicationDirPath() + "/scripts/plotting";
}

MatplotlibPlotWidget::~MatplotlibPlotWidget()
{
    if (_process) {
        _process->kill();
        _process->waitForFinished(1000);
        delete _process;
    }
    delete _inputFile;
}

// ============================================================================
// Public API
// ============================================================================

void MatplotlibPlotWidget::requestPlot(const PlotRequest& request)
{
    // Cancel any pending render
    if (_process) {
        _process->disconnect();
        _process->kill();
        _process->waitForFinished(500);
        delete _process;
        _process = nullptr;
    }

    _lastRequest    = request;
    _hasLastRequest = true;

    showSpinner();
    launchProcess(request);
}

void MatplotlibPlotWidget::clear()
{
    _imageLabel->clear();
    _imageLabel->show();
    _statusLabel->hide();
    hideSpinner();
    _hasLastRequest = false;
}

void MatplotlibPlotWidget::showPlaceholder(const QString& text)
{
    hideSpinner();
    _imageLabel->hide();
    _statusLabel->setText(text);
    _statusLabel->show();
}

QPixmap MatplotlibPlotWidget::currentPixmap() const
{
    return _imageLabel->originalPixmap();
}

// ============================================================================
// Resize handling
// ============================================================================

void MatplotlibPlotWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Reposition spinner to center
    QPoint center(width() / 2 - _spinner->width() / 2,
                  height() / 2 - _spinner->height() / 2 - 15);
    _spinner->move(center);
    _loadingLabel->setGeometry(0, center.y() + _spinner->height() + 5,
                               width(), 25);

    // Debounced re-render
    if (_autoRerender && _hasLastRequest)
        _resizeDebounce->start();
}

// ============================================================================
// Spinner helpers
// ============================================================================

void MatplotlibPlotWidget::showSpinner()
{
    QPoint center(width() / 2 - _spinner->width() / 2,
                  height() / 2 - _spinner->height() / 2 - 15);
    _spinner->move(center);
    _spinner->start();

    _loadingLabel->setGeometry(0, center.y() + _spinner->height() + 5,
                               width(), 25);
    _loadingLabel->show();

    _statusLabel->hide();
}

void MatplotlibPlotWidget::hideSpinner()
{
    _spinner->stop();
    _loadingLabel->hide();
}

// ============================================================================
// Process management
// ============================================================================

void MatplotlibPlotWidget::launchProcess(const PlotRequest& request)
{
    QString scriptPath = findScript(request.scriptName);
    if (scriptPath.isEmpty()) {
        hideSpinner();
        showPlaceholder(
            QString("Plot script not found: %1\n\nLooked in: %2")
                .arg(request.scriptName, _scriptDir));
        emit plotFailed("Script not found: " + request.scriptName);
        return;
    }

    // Build the JSON payload with rendering parameters
    QJsonObject fullPayload = request.payload;

    int w = request.widthPx  > 0 ? request.widthPx
                                  : static_cast<int>(width()  * devicePixelRatioF());
    int h = request.heightPx > 0 ? request.heightPx
                                  : static_cast<int>(height() * devicePixelRatioF());
    int dpi = request.dpi > 0 ? request.dpi : 150;

    fullPayload["_render_width"]  = w;
    fullPayload["_render_height"] = h;
    fullPayload["_render_dpi"]    = dpi;

    // Detect dark mode
    QColor bg = palette().color(QPalette::Window);
    bool dark = bg.lightnessF() < 0.5;
    fullPayload["_dark_mode"] = dark;

    // Write payload to temp file (avoids command-line length limits)
    delete _inputFile;
    _inputFile = new QTemporaryFile(QDir::tempPath() + "/mpl_plot_XXXXXX.json");
    _inputFile->setAutoRemove(true);

    if (!_inputFile->open()) {
        hideSpinner();
        showPlaceholder("Failed to create temporary file for plot data.");
        emit plotFailed("Temp file creation failed");
        return;
    }

    QJsonDocument doc(fullPayload);
    _inputFile->write(doc.toJson(QJsonDocument::Compact));
    _inputFile->flush();
    QString inputPath = _inputFile->fileName();

    LOG_DEBUG(CAT, QString("Launching: %1 %2 --input %3 (%4 bytes)")
        .arg(_pythonExe, scriptPath, inputPath)
        .arg(doc.toJson(QJsonDocument::Compact).size()));

    // Launch process
    _process = new QProcess(this);
    _stdoutBuffer.clear();
    _stderrBuffer.clear();

    connect(_process, &QProcess::readyReadStandardOutput, this, [this]() {
        _stdoutBuffer.append(_process->readAllStandardOutput());
    });
    connect(_process, &QProcess::readyReadStandardError, this, [this]() {
        _stderrBuffer.append(_process->readAllStandardError());
    });
    connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MatplotlibPlotWidget::onProcessFinished);
    connect(_process, &QProcess::errorOccurred,
            this, &MatplotlibPlotWidget::onProcessError);

    // The Python script writes PNG bytes to stdout
    _process->start(_pythonExe, {scriptPath, "--input", inputPath, "--output", "stdout"});

    // Timeout: kill after 30 seconds
    QTimer::singleShot(30000, this, [this]() {
        if (_process && _process->state() != QProcess::NotRunning) {
            LOG_WARNING(CAT, "Plot render timed out (30s), killing process");
            _process->kill();
        }
    });
}

void MatplotlibPlotWidget::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    hideSpinner();

    // Collect any remaining output
    if (_process) {
        _stdoutBuffer.append(_process->readAllStandardOutput());
        _stderrBuffer.append(_process->readAllStandardError());
    }

    if (status != QProcess::NormalExit || exitCode != 0) {
        QString errMsg = QString::fromUtf8(_stderrBuffer).trimmed();
        LOG_ERROR(CAT, QString("Plot process failed (exit=%1): %2")
            .arg(exitCode).arg(errMsg));

        // Show last few lines of stderr
        QStringList lines = errMsg.split('\n');
        if (lines.size() > 5)
            lines = lines.mid(lines.size() - 5);

        showPlaceholder(
            QString("Plot rendering failed:\n\n%1").arg(lines.join('\n')));
        emit plotFailed(errMsg);
        return;
    }

    // Parse PNG from stdout
    if (_stdoutBuffer.isEmpty()) {
        showPlaceholder("Plot script produced no output.");
        emit plotFailed("Empty output");
        return;
    }

    QPixmap pixmap;
    if (!pixmap.loadFromData(_stdoutBuffer, "PNG")) {
        LOG_ERROR(CAT, QString("Failed to parse PNG output (%1 bytes)")
            .arg(_stdoutBuffer.size()));
        showPlaceholder("Failed to parse plot image.");
        emit plotFailed("Invalid PNG data");
        return;
    }

    pixmap.setDevicePixelRatio(devicePixelRatioF());

    _statusLabel->hide();
    _imageLabel->setPixmap(pixmap);
    _imageLabel->show();

    LOG_DEBUG(CAT, QString("Plot rendered successfully: %1x%2 px")
        .arg(pixmap.width()).arg(pixmap.height()));

    emit plotReady();

    // Clean up
    if (_process) {
        _process->deleteLater();
        _process = nullptr;
    }
}

void MatplotlibPlotWidget::onProcessError(QProcess::ProcessError error)
{
    hideSpinner();

    QString msg;
    switch (error) {
    case QProcess::FailedToStart:
        msg = QString("Failed to start Python: '%1'\n\n"
                      "Ensure Python 3 with matplotlib is installed and on PATH.")
                  .arg(_pythonExe);
        break;
    case QProcess::Crashed:
        msg = "Python process crashed.";
        break;
    case QProcess::Timedout:
        msg = "Plot rendering timed out.";
        break;
    default:
        msg = "Unknown process error.";
        break;
    }

    LOG_ERROR(CAT, msg);
    showPlaceholder(msg);
    emit plotFailed(msg);
}

// ============================================================================
// Utility
// ============================================================================

QString MatplotlibPlotWidget::findPython() const
{
    // Try common names
    for (const auto& name : {"python3", "python"}) {
        QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) return path;
    }

#ifdef Q_OS_WIN
    // Windows: try py launcher
    QString py = QStandardPaths::findExecutable("py");
    if (!py.isEmpty()) return py;
#endif

    return "python3"; // hope for the best
}

QString MatplotlibPlotWidget::findScript(const QString& scriptName) const
{
    // Search order:
    // 1. _scriptDir / scriptName
    // 2. app_dir / scripts / plotting / scriptName
    // 3. app_dir / scriptName
    // 4. source tree (for development): ../scripts/plotting/scriptName

    QStringList searchPaths = {
        _scriptDir + "/" + scriptName,
        QCoreApplication::applicationDirPath() + "/scripts/plotting/" + scriptName,
        QCoreApplication::applicationDirPath() + "/" + scriptName,
        QCoreApplication::applicationDirPath() + "/../scripts/plotting/" + scriptName,
    };

    for (const auto& p : searchPaths) {
        if (QFileInfo::exists(p))
            return QFileInfo(p).absoluteFilePath();
    }

    return {};
}