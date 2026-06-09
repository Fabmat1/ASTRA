#pragma once

#include <QDialog>
#include <QColor>
#include <QFont>
#include <QStringList>
#include <memory>
#include <vector>

class Star;
class QCustomPlot;
class QCPTextElement;
class QCPColorScale;
class QCPMarginGroup;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QFormLayout;
class QGroupBox;
class QListWidget;
class QListWidgetItem;

// Project-level plotting tool: scatter plots, histograms and all-sky maps of
// arbitrary numeric star fields, with export to PDF/SVG/PNG.
class ProjectPlotDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProjectPlotDialog(std::vector<std::shared_ptr<Star>> allStars,
                               std::vector<std::shared_ptr<Star>> filteredStars,
                               std::vector<std::shared_ptr<Star>> selectedStars,
                               QWidget* parent = nullptr);

private slots:
    void updatePlot();
    void onExport();
    void onPickColor();
    void onPickBgColor();
    void onPickFont();
    void onPlotTypeChanged(int index);
    void onSwapAxes();
    void onAddSeries();
    void onRemoveSeries();
    void onSeriesSelectionChanged(int row);
    void onSeriesRenamed(QListWidgetItem* item);
    void onEditOverlay();
    void onRemoveOverlay();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    enum PlotType { Scatter = 0, Histogram = 1, SkyMap = 2 };

    // One plotted dataset; the source/field/marker/color controls edit the
    // series currently selected in the series list.
    struct SeriesConfig {
        QString label;
        int     sourceId = 0;          // 0 all, 1 filtered, 2 selected
        QString xKey, yKey, histKey;
        int     markerIndex = 0;
        QColor  color;
    };

    // A file-based overlay: a line (track/isochrone) or a closed region
    // boundary drawn as a shaded polygon.
    struct OverlayConfig {
        enum Type { Line = 0, Region = 1 };
        int     type = Line;
        QString label;
        QColor  color;
        double  width = 1.5;
        int     penStyle = 0;          // 0 solid, 1 dashed, 2 dotted
        QString filePath;
        int     colX = 1, colY = 2;    // 1-based file columns
        QVector<double> tx, ty;        // loaded vertices
    };

    void setupUi();
    QWidget* buildControlPanel();
    void detectNumericFields();
    void populateFieldCombo(QComboBox* combo, const QString& defaultKey,
                            bool noneOption = false);
    QString fieldLabel(const QString& key) const;

    const std::vector<std::shared_ptr<Star>>& starsForSource(int sourceId) const;
    std::vector<double> extractField(const std::vector<std::shared_ptr<Star>>& stars,
                                     const QString& key) const;

    void syncUiToSeries();
    void loadSeriesIntoUi(const SeriesConfig& s);
    static void setComboKey(QComboBox* combo, const QString& key);

    void addOverlayOfType(int type);
    bool editOverlayDialog(OverlayConfig& ov);
    bool loadTrackFile(OverlayConfig& ov, QString* err);
    void drawOverlays();
    QString overlayDisplayText(const OverlayConfig& ov) const;

    void resetPlotState();
    void plotScatter();
    void plotHistogram();
    void plotSky();
    void applySkyAspect();
    void applyAxisLimits();
    void applyAxisStyling();
    void applyCustomTickers();
    QString defaultExportBaseName() const;
    void updateTitleElement();
    void updateColorSwatch();
    void updateFontButton();
    void positionResetButton();
    void setStatus(const QString& text);

    // Effective foreground/grid colours: derived from the custom background if
    // one is set, otherwise from the active theme.
    QColor effectiveFg() const;
    QColor effectiveGrid() const;
    void   applyCustomBackground();

    // Palette popup anchored below `anchor`. Returns an invalid colour when
    // cancelled. With `background` true the palette is white/black/greys plus
    // a "Transparent" entry (returned as alpha-0 colour); if themeDefault is
    // non-null a "Theme default" entry is offered and *themeDefault reports
    // whether it was chosen.
    QColor pickColor(QPushButton* anchor, const QColor& current,
                     bool background = false, bool* themeDefault = nullptr);

    static bool limitValue(const QLineEdit* edit, double& out);

    // Data
    std::vector<std::shared_ptr<Star>> _allStars;
    std::vector<std::shared_ptr<Star>> _filteredStars;
    std::vector<std::shared_ptr<Star>> _selectedStars;
    QStringList _numericFields;   // plottable field keys, in catalogue order
    std::vector<SeriesConfig> _series;
    std::vector<OverlayConfig> _overlays;

    // UI
    QCustomPlot*    _plot          = nullptr;
    QCPTextElement* _titleElement  = nullptr;
    QCPColorScale*  _colorScale    = nullptr;   // present while "Color by" is active
    QCPMarginGroup* _marginGroup   = nullptr;
    QComboBox*      _sourceCombo   = nullptr;
    QComboBox*      _typeCombo     = nullptr;
    QStackedWidget* _optionsStack  = nullptr;
    QListWidget*    _seriesList    = nullptr;
    QListWidget*    _overlayList   = nullptr;
    QWidget*        _overlaysGroup = nullptr;

    // Scatter page
    QComboBox* _xFieldCombo    = nullptr;
    QComboBox* _yFieldCombo    = nullptr;
    QCheckBox* _logXCheck      = nullptr;
    QCheckBox* _logYCheck      = nullptr;
    QCheckBox* _invXCheck      = nullptr;
    QCheckBox* _invYCheck      = nullptr;
    QComboBox* _colorByCombo   = nullptr;
    QComboBox* _sizeByCombo    = nullptr;
    QComboBox* _trendCombo     = nullptr;
    QSpinBox*  _trendWindowSpin = nullptr;

    // Histogram page
    QComboBox* _histFieldCombo  = nullptr;
    QSpinBox*  _binsSpin        = nullptr;
    QCheckBox* _histLogXCheck   = nullptr;
    QCheckBox* _histLogYCheck   = nullptr;
    QCheckBox* _histInvXCheck   = nullptr;
    QCheckBox* _histLogBinsCheck = nullptr;

    // Sky page
    QCheckBox* _skyGridCheck = nullptr;

    // Axis limits & filtering (collapsible)
    QWidget*   _limitsGroup      = nullptr;
    QLineEdit* _xMinEdit         = nullptr;
    QLineEdit* _xMaxEdit         = nullptr;
    QLineEdit* _yMinEdit         = nullptr;
    QLineEdit* _yMaxEdit         = nullptr;
    QCheckBox* _hideStackedCheck = nullptr;
    QSpinBox*  _stackedNSpin     = nullptr;

    // Style
    QFormLayout*    _styleForm      = nullptr;
    QComboBox*      _markerCombo    = nullptr;
    QSpinBox*       _markerSizeSpin = nullptr;
    QRadioButton*   _allPointsRadio = nullptr;
    QRadioButton*   _cullRadio      = nullptr;
    QPushButton*    _colorBtn       = nullptr;
    QPushButton*    _bgColorBtn     = nullptr;
    QDoubleSpinBox* _axisWidthSpin  = nullptr;
    QCheckBox*      _minorTicksCheck = nullptr;
    QCheckBox*      _gridCheck      = nullptr;
    QLineEdit*      _xTicksEdit     = nullptr;
    QLineEdit*      _yTicksEdit     = nullptr;
    QLineEdit*      _xLabelEdit     = nullptr;
    QLineEdit*      _yLabelEdit     = nullptr;
    QPushButton*    _fontBtn        = nullptr;
    QLineEdit*      _titleEdit      = nullptr;
    QLabel*         _statusLabel    = nullptr;

    // Overlaid on the plot, shown only after interactive zoom/pan
    QPushButton* _resetViewBtn = nullptr;
    bool         _updatingPlot = false;

    QColor _seriesColor;
    QColor _bgColor;   // invalid → follow app theme
    QFont  _axisFont;  // axis label + tick label font
};
