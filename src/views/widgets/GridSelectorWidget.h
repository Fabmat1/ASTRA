#pragma once

#include <QFutureWatcher>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <optional>

class QProgressBar;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QGroupBox;

struct GridPreset
{
    QString category;
    QString name;
    QString path;                       // relative path suffix that identifies the grid
    double teffMin = 0, teffMax = 0;
    double loggMin = 0, loggMax = 0;
    double heMin   = 0, heMax   = 0;
    double zMin    = 0, zMax    = 0;
};

struct DiscoveredGrid
{
    QString basePath;
    QString relativePath;
    QString fullPath;
    int     presetIndex = -1;
    QString category;
    QString displayName;
    double  teffMin = 0, teffMax = 0;
    double  loggMin = 0, loggMax = 0;
    double  heMin   = 0, heMax   = 0;
    double  zMin    = 0, zMax    = 0;
};

class GridSelectorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit GridSelectorWidget(QWidget* parent = nullptr);

    void setBasePaths(const QStringList& paths);
    QStringList basePaths() const { return _basePaths; }

    void setGridMarkers(const QStringList& markers);   // default {"grid.fits"}
    void setPresets(const QVector<GridPreset>& presets);
    void setTitle(const QString& title);
    void setShowConfigureButton(bool show);

    void refresh();

    bool    hasSelection() const;
    QString selectedRelativePath() const;
    QString selectedBasePath() const;
    QString selectedFullPath() const;
    std::optional<DiscoveredGrid> selectedGrid() const;

    void setSelection(const QString& category,
                      const QString& relativePathOrOverride);

    static const QVector<GridPreset>& defaultPresets();

signals:
    void selectionChanged();
    void configurePathsRequested();

private:

    bool    _scanPending = false;
    void buildUi();
    void scanPaths();
    void populateCategoryCombo();
    void populateGridCombo();

    void    startScan();
    void    onScanFinished();
    void    finishPopulate(); 
    void    setLoading(bool on);
    void    applySelection(const QString &category, const QString &rel);
    static QVector<DiscoveredGrid> performScan( // runs on worker thread
        const QStringList &basePaths, const QStringList &markers,
        const QStringList &skipTokens, const QVector<GridPreset> &presets);

    QStringList _skipTokens{"raw"};

    QStringList             _basePaths;
    QStringList             _markers{"grid.fits"};
    QVector<GridPreset>     _presets;
    QVector<DiscoveredGrid> _discovered;

    QGroupBox*   _groupBox     = nullptr;
    QComboBox*   _catCombo     = nullptr;
    QComboBox*   _gridCombo    = nullptr;
    QLineEdit*   _overrideEdit = nullptr;
    QLabel*      _statusLabel  = nullptr;
    QPushButton* _refreshBtn   = nullptr;
    QPushButton* _configBtn    = nullptr;

    QFutureWatcher<QVector<DiscoveredGrid>> *_scanWatcher = nullptr;
    QProgressBar                            *_spinner     = nullptr;

    QString _pendingCat, _pendingSel;
};