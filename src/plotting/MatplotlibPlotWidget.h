#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QProcess>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPixmap>
#include <QTimer>
#include <QMovie>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QDir>

#include <functional>
#include <memory>

// ============================================================================
// SpinnerWidget — animated loading indicator
// ============================================================================

class SpinnerWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SpinnerWidget(QWidget* parent = nullptr)
        : QWidget(parent), _angle(0), _timer(new QTimer(this))
    {
        setFixedSize(64, 64);
        connect(_timer, &QTimer::timeout, this, [this]() {
            _angle = (_angle + 30) % 360;
            update();
        });
    }

    void start() { _timer->start(80); show(); }
    void stop()  { _timer->stop(); hide(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(width() / 2.0, height() / 2.0);

        const int numDots = 12;
        const double radius = 20.0;
        const double dotRadius = 3.5;

        for (int i = 0; i < numDots; ++i) {
            double angle = (360.0 / numDots) * i;
            double rad = qDegreesToRadians(angle + _angle);
            double x = radius * std::cos(rad);
            double y = radius * std::sin(rad);

            double alpha = 0.15 + 0.85 * (static_cast<double>(i) / numDots);
            QColor c = palette().color(QPalette::Text);
            c.setAlphaF(alpha);
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(x, y), dotRadius, dotRadius);
        }
    }

private:
    int     _angle;
    QTimer* _timer;
};

// ============================================================================
// ScalableImageLabel — displays a pixmap that fills the widget, 
//                      preserving aspect ratio, with optional zoom
// ============================================================================

class ScalableImageLabel : public QWidget
{
    Q_OBJECT
public:
    explicit ScalableImageLabel(QWidget* parent = nullptr)
        : QWidget(parent) {}

    void setPixmap(const QPixmap& pm)
    {
        _original = pm;
        update();
    }

    void clear()
    {
        _original = QPixmap();
        update();
    }

    bool hasPixmap() const { return !_original.isNull(); }
    const QPixmap& originalPixmap() const { return _original; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (_original.isNull()) return;

        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        QSize scaled = _original.size().scaled(size(), Qt::KeepAspectRatio);
        int x = (width()  - scaled.width())  / 2;
        int y = (height() - scaled.height()) / 2;

        p.drawPixmap(x, y, scaled.width(), scaled.height(), _original);
    }

private:
    QPixmap _original;
};

// ============================================================================
// PlotRequest — describes what to render
// ============================================================================

struct PlotRequest
{
    QString   scriptName;     // e.g. "rv_plot.py", "lc_plot.py", "spectrum_plot.py"
    QJsonObject payload;      // all data + options as JSON

    // Optional: override DPI, size
    int widthPx  = 0;        // 0 = auto from widget size
    int heightPx = 0;
    int dpi      = 150;
};

// ============================================================================
// MatplotlibPlotWidget
// ============================================================================

class MatplotlibPlotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MatplotlibPlotWidget(QWidget* parent = nullptr);
    ~MatplotlibPlotWidget();

    // Submit a plot request. Previous pending request is cancelled.
    void requestPlot(const PlotRequest& request);

    // Clear the displayed plot
    void clear();

    // Show a text placeholder (no spinner)
    void showPlaceholder(const QString& text);

    // Access the rendered pixmap (e.g. for saving)
    QPixmap currentPixmap() const;

    // Set the Python executable path (default: "python3")
    void setPythonExecutable(const QString& path) { _pythonExe = path; }

    // Set the directory containing plot scripts
    void setScriptDirectory(const QString& dir) { _scriptDir = dir; }

    // Re-render on resize (with debounce)
    void setAutoRerender(bool enable) { _autoRerender = enable; }

signals:
    void plotReady();
    void plotFailed(const QString& errorMessage);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void showSpinner();
    void hideSpinner();
    void launchProcess(const PlotRequest& request);
    QString findPython() const;
    QString findScript(const QString& scriptName) const;

    // UI
    QVBoxLayout*       _layout;
    ScalableImageLabel* _imageLabel;
    SpinnerWidget*     _spinner;
    QLabel*            _statusLabel;   // for errors / placeholder text
    QLabel*            _loadingLabel;  // "Rendering plot..." text

    // Process
    QProcess*          _process;
    QByteArray         _stdoutBuffer;
    QByteArray         _stderrBuffer;
    QTemporaryFile*    _inputFile;     // temp JSON file for large payloads

    // Config
    QString            _pythonExe;
    QString            _scriptDir;
    bool               _autoRerender;
    PlotRequest        _lastRequest;
    bool               _hasLastRequest;
    QTimer*            _resizeDebounce;
};