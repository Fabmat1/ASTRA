#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class AppSettings : public QObject
{
    Q_OBJECT
public:
    enum class DetailPanel {
        None = 0,
        Summary,
        RadialVelocity,
        LightCurve,
        Spectra,
    };
    Q_ENUM(DetailPanel)

    static QString panelName(DetailPanel p);
    static QList<DetailPanel> allPanels();

    static constexpr int kMinGridDim = 1;
    static constexpr int kMaxGridDim = 4;

    explicit AppSettings(QObject* parent = nullptr);

    // ── General ──────────────────────────────────────────────────────────
    QString isisBinaryPath() const { return _isisBinaryPath; }
    void    setIsisBinaryPath(const QString& path);

    // ── SED fitting (SEDplusplus sedfit binary) ──────────────────────────
    /// User override for the sedfit executable ("" = use bundled/PATH).
    QString sedFitBinaryPath() const { return _sedFitBinaryPath; }
    void    setSedFitBinaryPath(const QString& path);

    /// User override for the filter reference-data directory ("" = bundled).
    QString sedFitRefdataDir() const { return _sedFitRefdataDir; }
    void    setSedFitRefdataDir(const QString& dir);

    QStringList gridBasePaths() const { return _gridBasePaths; }
    void        setGridBasePaths(const QStringList& paths);

    // ── Fitting concurrency ──────────────────────────────────────────────
    /// Worker threads a fit may use. 0 = one per logical core (the default).
    /// Drives GAEL's internal parallelism *and* how many continuum-jitter
    /// refits it runs at once, so lowering it also lowers peak memory.
    int  fitWorkerThreads() const { return _fitWorkerThreads; }
    void setFitWorkerThreads(int n);

    /// The thread count a fit will actually use, with 0 resolved to the
    /// machine's core count. Never returns less than 1.
    static int resolveWorkerThreads(int setting);

    // ── Star Detail View grid ────────────────────────────────────────────
    int rows() const { return _rows; }
    int cols() const { return _cols; }
    DetailPanel detailCell(int row, int col) const;
    QVector<QVector<DetailPanel>> detailGrid() const { return _grid; }
    void setDetailGrid(int rows, int cols,
                       const QVector<QVector<DetailPanel>>& grid);

    QString lcqueryPython()    const { return _lcqueryPython; }
    QString lcqueryScript()    const { return _lcqueryScript; }
    QString atlasToken()       const { return _atlasToken; }
    QString adsApiToken() const { return _adsApiToken; }
    void    setAdsApiToken(const QString &t);
    QString blackgemScript()   const { return _blackgemScript; }

    void setLcqueryPython  (const QString& p);
    void setLcqueryScript  (const QString& p);
    void setAtlasToken     (const QString& t);
    void setBlackgemScript (const QString& p);

    // ── Lightcurve fitting (lcurve binaries) ─────────────────────────────
    QString lcurveDir() const { return _lcurveDir; }
    void    setLcurveDir(const QString& dir);

    /// Resolve a binary name (e.g. "lcurve_levmarq") to an absolute path.
    /// Uses _lcurveDir if set, otherwise searches PATH. Returns "" if not found.
    QString lcurveBinary(const QString& name) const;

    // ── Updates ──────────────────────────────────────────────────────────
    bool checkUpdatesOnStartup() const { return _checkUpdatesOnStartup; }
    void setCheckUpdatesOnStartup(bool on);

    /// Release version the user chose to skip (empty if none).
    QString skippedUpdateVersion() const { return _skippedUpdateVersion; }
    void    setSkippedUpdateVersion(const QString& version);


signals:
    void isisBinaryPathChanged();
    void sedFitSettingsChanged();
    void detailGridChanged();
    void gridBasePathsChanged();
    void lcquerySettingsChanged();
    void lcurveSettingsChanged();
    void adsApiTokenChanged();
    void updateSettingsChanged();
    void fitWorkerThreadsChanged();

  private:
    void load();
    void save() const;
    void applyDefaults();

    QString _isisBinaryPath;
    QString _sedFitBinaryPath;
    QString _sedFitRefdataDir;
    QStringList _gridBasePaths;
    int         _fitWorkerThreads = 0;   // 0 = one per logical core

    int _rows = 2;
    int _cols = 2;
    QVector<QVector<DetailPanel>> _grid;

    QString _lcqueryPython;
    QString _lcqueryScript;
    QString _lcurveDir;
    QString _atlasToken;
    QString _blackgemScript;
    QString _adsApiToken;

    bool    _checkUpdatesOnStartup = true;
    QString _skippedUpdateVersion;
};