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
        : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void setPixmap(const QPixmap& pm)
    {
        _original = pm;
        resetView();
        update();
    }

    void clear()
    {
        _original = QPixmap();
        _zoom = 1.0;
        _pan = QPointF(0, 0);
        update();
    }

    bool hasPixmap() const { return !_original.isNull(); }
    const QPixmap& originalPixmap() const { return _original; }

    void resetView()
    {
        _zoom = 1.0;
        _pan = QPointF(0, 0);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (_original.isNull()) return;

        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        // Fit-to-widget base size
        QSizeF base = _original.size().scaled(size(), Qt::KeepAspectRatio);
        double w = base.width()  * _zoom;
        double h = base.height() * _zoom;

        // Center + pan offset
        double x = (width()  - w) / 2.0 + _pan.x();
        double y = (height() - h) / 2.0 + _pan.y();

        p.drawPixmap(QRectF(x, y, w, h), _original, QRectF(QPointF(0,0), _original.size()));

        // Zoom indicator when zoomed
        if (std::abs(_zoom - 1.0) > 0.01) {
            QString label = QString("%1%").arg(static_cast<int>(_zoom * 100));
            QFont f = font();
            f.setPointSize(9);
            p.setFont(f);

            QFontMetrics fm(f);
            QRect textRect = fm.boundingRect(label);
            textRect.adjust(-4, -2, 4, 2);
            textRect.moveBottomRight(QPoint(width() - 6, height() - 6));

            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 120));
            p.drawRoundedRect(textRect, 3, 3);

            p.setPen(QColor(220, 220, 220));
            p.drawText(textRect, Qt::AlignCenter, label);
        }
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (_original.isNull()) return;

        double oldZoom = _zoom;
        double delta = event->angleDelta().y() / 120.0;
        double factor = std::pow(1.15, delta);
        _zoom = std::clamp(_zoom * factor, 0.25, 20.0);

        // Zoom toward cursor position
        QPointF cursorPos = event->position();
        QPointF center(width() / 2.0 + _pan.x(), height() / 2.0 + _pan.y());
        QPointF diff = cursorPos - center;
        _pan += diff * (1.0 - _zoom / oldZoom);

        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (_original.isNull()) return;

        if (event->button() == Qt::MiddleButton ||
            event->button() == Qt::LeftButton) {
            _dragging = true;
            _dragStart = event->pos();
            _panStart = _pan;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (_dragging) {
            _pan = _panStart + (event->pos() - _dragStart);
            update();
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (_dragging) {
            _dragging = false;
            setCursor(Qt::CrossCursor);
            event->accept();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (_original.isNull()) return;
        resetView();
        event->accept();
    }

private:
    QPixmap _original;
    double  _zoom = 1.0;
    QPointF _pan{0, 0};

    bool    _dragging = false;
    QPoint  _dragStart;
    QPointF _panStart;
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