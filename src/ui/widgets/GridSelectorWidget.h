#pragma once

#include <QFutureWatcher>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QProgressBar;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QGroupBox;
class ElidedLabel;

struct GridPreset
{
    QString category;
    QString name;
    QString path;                       // relative path suffix that identifies the grid
    double teffMin = 0, teffMax = 0;
    double loggMin = 0, loggMax = 0;
    double heMin   = 0, heMax   = 0;
    double zMin    = 0, zMax    = 0;
    /// Further path suffixes naming the same grid. Installations predate (or
    /// postdate) the packaged directory names, so the same physical grid shows
    /// up under different spellings on different machines.
    QStringList aliases;
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
    void populateCategoryCombo();
    void populateGridCombo();
    void updatePathLabel();

    // localOnly drops every remote ("ssh://") base path from this scan.
    void    startScan(bool localOnly = false);
    void    onScanFinished();
    void    onRemoteScanProgress(const QString &hostName);
    void    abandonScanForLocalOnly();
    void    finishPopulate(); 
    void    setLoading(bool on);
    void    applySelection(const QString &category, const QString &rel);
    void    applyDefaultSelection();   // prefer sdB / "sdB standard" if present
    // Runs on a worker thread. `onRemoteHost` is called with the host name
    // before each remote listing and with an empty string after it; remote
    // bases not yet started are skipped once `skipRemote` is set.
    static QVector<DiscoveredGrid> performScan(
        const QStringList &basePaths, const QStringList &markers,
        const QStringList &skipTokens, const QVector<GridPreset> &presets,
        const std::function<void(const QString &)> &onRemoteHost,
        const std::atomic_bool &skipRemote);

    QStringList _skipTokens{"raw"};

    QStringList             _basePaths;
    QStringList             _markers{"grid.fits"};
    QVector<GridPreset>     _presets;
    QVector<DiscoveredGrid> _discovered;

    QGroupBox*   _groupBox     = nullptr;
    QComboBox*   _catCombo     = nullptr;
    QComboBox*   _gridCombo    = nullptr;
    QLineEdit*   _overrideEdit = nullptr;
    ElidedLabel* _pathLabel    = nullptr;
    QLabel*      _statusLabel  = nullptr;
    QPushButton* _refreshBtn   = nullptr;
    QPushButton* _configBtn    = nullptr;
    QPushButton* _localOnlyBtn = nullptr;

    QFutureWatcher<QVector<DiscoveredGrid>> *_scanWatcher = nullptr;
    QProgressBar                            *_spinner     = nullptr;
    std::shared_ptr<std::atomic_bool>        _scanSkipRemote;
    bool                                     _scanIsLocalOnly = false;

    QString _pendingCat, _pendingSel;

    // True once a selection has been requested explicitly (via setSelection)
    // or chosen by the user, so we stop forcing the sdB default.
    bool _hasExplicitSelection = false;
};