#pragma once

// A pannable, zoomable sky map around a target, in the spirit of Aladin.
//
// Every layer, a HiPS survey or a local north-up image, is resampled pixel by
// pixel into one gnomonic (TAN) view: north up and east left at the view
// centre, at a scale the user sets. The view (centre and scale) belongs to the
// widget, not to the layer, so switching between surveys keeps the same
// orientation, centre and zoom, and features line up from X-ray to radio.
//
// On top of the image the widget draws the target reticle, a compass, a scale
// bar, the cursor position and, optionally, Gaia DR3 sources, fetched for
// whatever part of the sky is in view.

#include "skymap/GaiaFieldQuery.h"
#include "skymap/Healpix.h"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <limits>

class QNetworkAccessManager;
class QTimer;

namespace SkyMap {

class HipsTileLoader;

class SkyMapWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SkyMapWidget(QWidget* parent = nullptr);
    ~SkyMapWidget() override;

    /// The star the map is about. `gaiaSourceId` (0 if unknown) keeps the
    /// star's own Gaia entry from being marked as a neighbour; `gmag` (NaN if
    /// unknown) is the reference for the brightness contrast of neighbours.
    void setTarget(double raDeg, double decDeg, qint64 gaiaSourceId, double gmag);

    /// Show a HiPS survey from hipsSurveys().
    void showHipsSurvey(const QString& id);
    /// Show an image that is north up and east left, centred on (ra, dec),
    /// with square pixels of `arcsecPerPixel`.
    void showLocalImage(const QImage& image, double raDeg, double decDeg,
                        double arcsecPerPixel, const QString& title);
    /// Show an image whose sky geometry is unknown, fitted into the widget as
    /// is. The sky overlays are hidden: they could not be placed on it.
    void showFlatImage(const QImage& image, const QString& title, const QString& note);
    /// Show only the overlays, with `message` in place of an image.
    void showNothing(const QString& title, const QString& message);

    /// Probe the current survey's servers again after a failure.
    void retryCurrentSurvey();
    /// True while the current HiPS survey could not be reached.
    bool currentSurveyFailed() const;

    void setGaiaMarkersVisible(bool on);
    bool gaiaMarkersVisible() const { return _gaiaVisible; }

    /// Centre on the target at the default field of view.
    void resetView();
    /// Multiply the zoom by `factor` (> 1 zooms in), about the view centre.
    void zoomBy(double factor);

    /// Width of the shorter side of the view, in arcseconds.
    double fieldOfViewArcsec() const;

    QSize sizeHint() const override { return { 640, 640 }; }
    QSize minimumSizeHint() const override { return { 240, 240 }; }

signals:
    /// The current survey resolved, failed or was retried.
    void surveyStateChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    enum class Layer { None, Hips, Local, Flat };

    // Tangent-plane frame at a sky position: centre, east and north unit vectors.
    struct Basis {
        Healpix::Vec3 c, e, n;
    };
    static Basis basisAt(double raDeg, double decDeg);

    // Sky <-> widget (logical pixel) coordinates in the current view.
    Healpix::Vec3 widgetToVec(const QPointF& p) const;
    bool          vecToWidget(const Healpix::Vec3& v, QPointF& p) const;
    QPointF       skyToWidget(double raDeg, double decDeg, bool* visible = nullptr) const;

    void setCenter(const Healpix::Vec3& v);
    void setScale(double arcsecPerPixel);
    void viewChanged();
    void scheduleRepaint();

    // Rendering
    void renderFrame();
    void renderHips(const Basis& b, double radPerDevPx, int w, int h);
    void renderLocal(const Basis& b, double radPerDevPx, int w, int h);

    // Overlays
    void drawGaiaMarkers(QPainter& p);
    void drawReticle(QPainter& p);
    void drawCompass(QPainter& p);
    void drawScaleBar(QPainter& p);
    void drawLocalFootprint(QPainter& p);
    void drawInfoPanels(QPainter& p);

    // Gaia
    void ensureGaiaCoverage();
    void onGaiaFinished(double ra, double dec, double radius,
                        const QVector<GaiaFieldSource>& sources, bool truncated,
                        const QString& error);
    const GaiaFieldSource* gaiaSourceAt(const QPointF& p) const;
    QString gaiaSourceText(const GaiaFieldSource& s, bool html) const;
    double  markerRadius(const GaiaFieldSource& s) const;
    bool    isTargetSource(const GaiaFieldSource& s) const;

    QString formatCoords(const Healpix::Vec3& v) const;

    QNetworkAccessManager* _nam    = nullptr;
    HipsTileLoader*        _loader = nullptr;

    // Target
    bool   _hasTarget   = false;
    double _targetRa    = 0.0;
    double _targetDec   = 0.0;
    qint64 _targetGaia  = 0;
    double _targetGmag  = std::numeric_limits<double>::quiet_NaN();

    // View: centre and scale. Owned by the widget so layers share it.
    double _centerRa  = 0.0;
    double _centerDec = 0.0;
    double _scale     = 0.3;    // arcsec per logical pixel
    bool   _viewInitialised = false;

    // Layer
    Layer   _layer = Layer::None;
    QString _surveyId;
    QString _layerTitle;
    QString _layerMessage;      // for Layer::None
    QImage  _localImage;
    double  _localRa = 0.0, _localDec = 0.0, _localScale = 1.0;

    // Last rendered frame (device pixels) and what it is showing.
    QImage  _frame;
    bool    _frameDirty = true;
    QString _hipsStatus;        // "loading 4 tiles", coverage notes
    QString _hipsDetail;        // mirror host, tile order
    bool    _noCoverage = false;
    QTimer* _repaintTimer = nullptr;

    // Interaction
    bool    _dragging = false;
    bool    _dragMoved = false;
    QPoint  _pressPos;
    Healpix::Vec3 _pressCenter{};
    double  _pressScale = 0.0;
    QPointF _cursor{ -1, -1 };
    bool    _cursorInside = false;

    // Gaia
    GaiaFieldQuery*                 _gaiaQuery = nullptr;
    QTimer*                         _gaiaTimer = nullptr;
    bool                            _gaiaVisible = false;
    QHash<qint64, GaiaFieldSource>  _gaiaSources;
    struct Cone { Healpix::Vec3 c; double radiusRad; };
    QVector<Cone>                   _gaiaCones;
    bool                            _gaiaTruncated = false;
    bool                            _gaiaTooWide = false;
    QString                         _gaiaError;
    qint64                          _selectedSource = 0;
};

} // namespace SkyMap
