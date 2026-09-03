#include "AppSettings.h"

#include <QSettings>
#include <QStringList>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <algorithm>
#include <thread>

namespace {
constexpr const char* kGroup      = "AppSettings";
constexpr const char* kIsisBinary = "general/isisBinary";
constexpr const char* kSedFitBinary  = "sedfit/binary";
constexpr const char* kSedFitRefdata = "sedfit/refdataDir";
constexpr const char *kADStoken   = "general/adsToken";
constexpr const char* kRows       = "starDetail/rows";
constexpr const char* kCols       = "starDetail/cols";
constexpr const char* kGrid       = "starDetail/grid";
constexpr const char* kGridPaths  = "gridPaths/all";
constexpr const char* kFitWorkers = "fitting/workerThreads";
constexpr const char* kLcqPython     = "lcquery/python";
constexpr const char* kLcqScript     = "lcquery/script";
constexpr const char* kAtlasToken    = "lcquery/atlasToken";
constexpr const char* kBlackgemScr   = "lcquery/blackgemScript";
constexpr const char* kLcurveDir = "lcurve/installDir";
constexpr const char* kCopyContent   = "format/copyContent";
constexpr const char* kCopyStyle     = "format/copyStyle";
constexpr const char* kCopyWrapMath  = "format/latexWrapMath";
constexpr const char* kCopyName      = "format/latexIncludeName";
constexpr const char* kCopyRound     = "format/roundOnCopy";
constexpr const char* kUpdateCheck   = "update/checkOnStartup";
constexpr const char* kUpdateSkipped = "update/skippedVersion";
constexpr const char* kSpecFetchRadius      = "specfetch/radiusArcsec";
constexpr const char* kSpecFetchParallel    = "specfetch/maxParallel";
constexpr const char* kSpecFetchDir         = "specfetch/downloadDir";
constexpr const char* kSpecFetchLamostToken = "specfetch/lamostToken";
constexpr const char* kSpecFetchLastOptions = "specfetch/lastOptions";
constexpr const char* kRemoteHosts          = "remote/hosts";
constexpr const char* kRemoteGridCacheDir   = "remote/gridCacheDir";
constexpr const char* kRemoteGridCacheCapGb = "remote/gridCacheCapGb";
}

QString AppSettings::panelName(DetailPanel p)
{
    switch (p) {
        case DetailPanel::None:           return "- Empty -";
        case DetailPanel::Summary:        return "Summary";
        case DetailPanel::RadialVelocity: return "Radial Velocity";
        case DetailPanel::LightCurve:     return "Light Curves";
        case DetailPanel::Spectra:        return "Spectra";
    }
    return "?";
}

QList<AppSettings::DetailPanel> AppSettings::allPanels()
{
    return {
        DetailPanel::None,
        DetailPanel::Summary,
        DetailPanel::RadialVelocity,
        DetailPanel::LightCurve,
        DetailPanel::Spectra,
    };
}

AppSettings::AppSettings(QObject* parent) : QObject(parent)
{
    applyDefaults();
    load();
}

void AppSettings::applyDefaults()
{
    _isisBinaryPath = QStandardPaths::findExecutable("isis");

    _rows = 2;
    _cols = 2;
    _grid = {
        { DetailPanel::Summary, DetailPanel::RadialVelocity },
        { DetailPanel::Spectra, DetailPanel::LightCurve     },
    };
    const QString home = QDir::homePath();
    // Only locations inside the user's own home. Anything else is specific to
    // one site's machines: it does not belong in a default that ships with
    // the program, and shared paths name people and hosts. Add site paths
    // under Settings / Grid Paths, or reach them over SSH under Settings /
    // Remote Hosts. Non-existent paths are ignored at search time.
    _gridBasePaths = { home + "/ISIS_models",
                       home + "/isis/synthetic_spectra/grids",
                       home + "/grids" };

    _lcqueryPython = QStandardPaths::findExecutable("python3");
    if (_lcqueryPython.isEmpty())
        _lcqueryPython = QStringLiteral("python3");
    
    #ifdef ASTRA_LCQUERY_SCRIPT
    {
        QString baked = QStringLiteral(ASTRA_LCQUERY_SCRIPT);
        if (!baked.isEmpty() && QFileInfo::exists(baked))
            _lcqueryScript = baked;
    }
    #endif
    
    _atlasToken      = QString();   // user supplies
    _blackgemScript  = QString();   // optional

    for (const char* probe : { "lcurve_levmarq", "lcurve_mcmc", "lcurve_simplex" }) {
        QString p = QStandardPaths::findExecutable(probe);
        if (!p.isEmpty()) {
            _lcurveDir = QFileInfo(p).absolutePath();
            break;
        }
    }
}

void AppSettings::load()
{
    QSettings s;
    s.beginGroup(kGroup);

    _isisBinaryPath = s.value(kIsisBinary, _isisBinaryPath).toString();
    _sedFitBinaryPath = s.value(kSedFitBinary,  _sedFitBinaryPath).toString();
    _sedFitRefdataDir = s.value(kSedFitRefdata, _sedFitRefdataDir).toString();

    int rows = std::clamp(s.value(kRows, _rows).toInt(), kMinGridDim, kMaxGridDim);
    int cols = std::clamp(s.value(kCols, _cols).toInt(), kMinGridDim, kMaxGridDim);

    QString flat = s.value(kGrid).toString();
    _gridBasePaths = s.value(kGridPaths, _gridBasePaths).toStringList();
    _fitWorkerThreads = std::max(0, s.value(kFitWorkers, _fitWorkerThreads).toInt());

    _lcqueryPython   = s.value(kLcqPython,    _lcqueryPython  ).toString();
    _lcqueryScript   = s.value(kLcqScript,    _lcqueryScript  ).toString();
    _adsApiToken     = s.value(kADStoken,   _adsApiToken     ).toString();
    _atlasToken      = s.value(kAtlasToken,   _atlasToken     ).toString();
    _blackgemScript  = s.value(kBlackgemScr,  _blackgemScript ).toString();
    _lcurveDir = s.value(kLcurveDir, _lcurveDir).toString();
    _copy.content = static_cast<QuantityFormat::CopyContent>(std::clamp(
        s.value(kCopyContent, static_cast<int>(_copy.content)).toInt(), 0, 2));
    _copy.style = static_cast<QuantityFormat::CopyStyle>(std::clamp(
        s.value(kCopyStyle, static_cast<int>(_copy.style)).toInt(), 0, 1));
    _copy.latexWrapMath    = s.value(kCopyWrapMath, _copy.latexWrapMath).toBool();
    _copy.latexIncludeName = s.value(kCopyName,     _copy.latexIncludeName).toBool();
    _copy.roundOnCopy      = s.value(kCopyRound,    _copy.roundOnCopy).toBool();
    _checkUpdatesOnStartup = s.value(kUpdateCheck,   _checkUpdatesOnStartup).toBool();
    _skippedUpdateVersion  = s.value(kUpdateSkipped, _skippedUpdateVersion ).toString();
    _specFetchRadiusArcsec =
        s.value(kSpecFetchRadius, _specFetchRadiusArcsec).toDouble();
    _specFetchMaxParallel = std::clamp(
        s.value(kSpecFetchParallel, _specFetchMaxParallel).toInt(), 1, 4);
    _specFetchDir         = s.value(kSpecFetchDir, _specFetchDir).toString();
    _specFetchLamostToken =
        s.value(kSpecFetchLamostToken, _specFetchLamostToken).toString();
    _specFetchLastOptions =
        s.value(kSpecFetchLastOptions, _specFetchLastOptions).toString();
    _remoteHostsJson = s.value(kRemoteHosts, _remoteHostsJson).toString();
    _remoteGridCacheDir =
        s.value(kRemoteGridCacheDir, _remoteGridCacheDir).toString();
    _remoteGridCacheCapGb = std::clamp(
        s.value(kRemoteGridCacheCapGb, _remoteGridCacheCapGb).toInt(), 1, 10000);
    s.endGroup();

    publishCopyPrefs();

    if (!flat.isEmpty()) {
        QStringList parts = flat.split(',', Qt::SkipEmptyParts);
        if (parts.size() == rows * cols) {
            _rows = rows;
            _cols = cols;
            _grid.assign(rows, QVector<DetailPanel>(cols, DetailPanel::None));
            for (int i = 0; i < parts.size(); ++i) {
                int r = i / cols, c = i % cols;
                _grid[r][c] = static_cast<DetailPanel>(parts[i].toInt());
            }
        }
    }
}

void AppSettings::save() const
{
    QSettings s;
    s.beginGroup(kGroup);
    s.setValue(kIsisBinary, _isisBinaryPath);
    s.setValue(kSedFitBinary,  _sedFitBinaryPath);
    s.setValue(kSedFitRefdata, _sedFitRefdataDir);
    s.setValue(kRows, _rows);
    s.setValue(kCols, _cols);

    QStringList flat;
    for (int r = 0; r < _rows; ++r)
        for (int c = 0; c < _cols; ++c)
            flat << QString::number(static_cast<int>(_grid[r][c]));
    s.setValue(kGrid, flat.join(','));
    s.setValue(kGridPaths, _gridBasePaths);
    s.setValue(kFitWorkers, _fitWorkerThreads);

    s.setValue(kLcqPython,    _lcqueryPython);
    s.setValue(kLcqScript,    _lcqueryScript);
    s.setValue(kADStoken,   _adsApiToken);
    s.setValue(kAtlasToken,   _atlasToken);
    s.setValue(kBlackgemScr,  _blackgemScript);
    s.setValue(kLcurveDir, _lcurveDir);
    s.setValue(kCopyContent,  static_cast<int>(_copy.content));
    s.setValue(kCopyStyle,    static_cast<int>(_copy.style));
    s.setValue(kCopyWrapMath, _copy.latexWrapMath);
    s.setValue(kCopyName,     _copy.latexIncludeName);
    s.setValue(kCopyRound,    _copy.roundOnCopy);
    s.setValue(kUpdateCheck,   _checkUpdatesOnStartup);
    s.setValue(kUpdateSkipped, _skippedUpdateVersion);
    s.setValue(kSpecFetchRadius,      _specFetchRadiusArcsec);
    s.setValue(kSpecFetchParallel,    _specFetchMaxParallel);
    s.setValue(kSpecFetchDir,         _specFetchDir);
    s.setValue(kSpecFetchLamostToken, _specFetchLamostToken);
    s.setValue(kSpecFetchLastOptions, _specFetchLastOptions);
    s.setValue(kRemoteHosts,          _remoteHostsJson);
    s.setValue(kRemoteGridCacheDir,   _remoteGridCacheDir);
    s.setValue(kRemoteGridCacheCapGb, _remoteGridCacheCapGb);

    s.endGroup();
    s.sync();
}

void AppSettings::publishCopyPrefs() const
{
    QuantityFormat::setPrefs(_copy);
}

void AppSettings::setCopyContent(QuantityFormat::CopyContent c)
{
    if (_copy.content == c) return;
    _copy.content = c;
    publishCopyPrefs();
    save();
    emit numberFormatChanged();
}

void AppSettings::setCopyStyle(QuantityFormat::CopyStyle st)
{
    if (_copy.style == st) return;
    _copy.style = st;
    publishCopyPrefs();
    save();
    emit numberFormatChanged();
}

void AppSettings::setCopyLatexWrapMath(bool on)
{
    if (_copy.latexWrapMath == on) return;
    _copy.latexWrapMath = on;
    publishCopyPrefs();
    save();
    emit numberFormatChanged();
}

void AppSettings::setCopyIncludeName(bool on)
{
    if (_copy.latexIncludeName == on) return;
    _copy.latexIncludeName = on;
    publishCopyPrefs();
    save();
    emit numberFormatChanged();
}

void AppSettings::setCopyRoundErrors(bool on)
{
    if (_copy.roundOnCopy == on) return;
    _copy.roundOnCopy = on;
    publishCopyPrefs();
    save();
    emit numberFormatChanged();
}

void AppSettings::setIsisBinaryPath(const QString& path)
{
    if (_isisBinaryPath == path) return;
    _isisBinaryPath = path;
    save();
    emit isisBinaryPathChanged();
}

void AppSettings::setSedFitBinaryPath(const QString& path)
{
    if (_sedFitBinaryPath == path) return;
    _sedFitBinaryPath = path;
    save();
    emit sedFitSettingsChanged();
}

void AppSettings::setSedFitRefdataDir(const QString& dir)
{
    if (_sedFitRefdataDir == dir) return;
    _sedFitRefdataDir = dir;
    save();
    emit sedFitSettingsChanged();
}

AppSettings::DetailPanel AppSettings::detailCell(int row, int col) const
{
    if (row < 0 || row >= _rows || col < 0 || col >= _cols)
        return DetailPanel::None;
    return _grid[row][col];
}

void AppSettings::setDetailGrid(int rows, int cols,
                                const QVector<QVector<DetailPanel>>& grid)
{
    rows = std::clamp(rows, kMinGridDim, kMaxGridDim);
    cols = std::clamp(cols, kMinGridDim, kMaxGridDim);
    _rows = rows;
    _cols = cols;
    _grid = grid;
    save();
    emit detailGridChanged();
}

void AppSettings::setGridBasePaths(const QStringList& paths)
{
    if (_gridBasePaths == paths) return;
    _gridBasePaths = paths;
    save();
    emit gridBasePathsChanged();
}

void AppSettings::setFitWorkerThreads(int n)
{
    n = std::max(0, n);
    if (_fitWorkerThreads == n) return;
    _fitWorkerThreads = n;
    save();
    emit fitWorkerThreadsChanged();
}

int AppSettings::resolveWorkerThreads(int setting)
{
    if (setting > 0) return setting;
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<int>(hw ? hw : 1u);
}

void AppSettings::setLcqueryPython(const QString& p) {
    if (_lcqueryPython == p) return;
    _lcqueryPython = p; save(); emit lcquerySettingsChanged();
}
void AppSettings::setLcqueryScript(const QString& p) {
    if (_lcqueryScript == p) return;
    _lcqueryScript = p; save(); emit lcquerySettingsChanged();
}
void AppSettings::setAtlasToken(const QString& t) {
    if (_atlasToken == t) return;
    _atlasToken = t; save(); emit lcquerySettingsChanged();
}
void AppSettings::setAdsApiToken(const QString &t) {
    if (_adsApiToken == t) return;
    _adsApiToken = t; save(); emit adsApiTokenChanged();
}
void AppSettings::setBlackgemScript(const QString& p) {
    if (_blackgemScript == p) return;
    _blackgemScript = p; save(); emit lcquerySettingsChanged();
}

void AppSettings::setLcurveDir(const QString &dir) {
  if (_lcurveDir == dir)
    return;
  _lcurveDir = dir;
  save();
  emit lcurveSettingsChanged();
}

void AppSettings::setSpecFetchRadiusArcsec(double r) {
    if (qFuzzyCompare(_specFetchRadiusArcsec, r)) return;
    _specFetchRadiusArcsec = r;
    save();
    emit specFetchSettingsChanged();
}

void AppSettings::setSpecFetchMaxParallel(int n) {
    n = std::clamp(n, 1, 4);
    if (_specFetchMaxParallel == n) return;
    _specFetchMaxParallel = n;
    save();
    emit specFetchSettingsChanged();
}

void AppSettings::setSpecFetchDir(const QString& dir) {
    if (_specFetchDir == dir) return;
    _specFetchDir = dir;
    save();
    emit specFetchSettingsChanged();
}

void AppSettings::setSpecFetchLamostToken(const QString& t) {
    if (_specFetchLamostToken == t) return;
    _specFetchLamostToken = t;
    save();
    emit specFetchSettingsChanged();
}

void AppSettings::setSpecFetchLastOptions(const QString& json) {
    if (_specFetchLastOptions == json) return;
    _specFetchLastOptions = json;
    save();
}

void AppSettings::setRemoteHostsJson(const QString& json) {
    if (_remoteHostsJson == json) return;
    _remoteHostsJson = json;
    save();
    emit remoteHostsChanged();
}

void AppSettings::setRemoteGridCacheDir(const QString& dir) {
    if (_remoteGridCacheDir == dir) return;
    _remoteGridCacheDir = dir;
    save();
}

QString AppSettings::effectiveRemoteGridCacheDir() const {
    if (!_remoteGridCacheDir.isEmpty()) return _remoteGridCacheDir;
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/gridcache");
}

void AppSettings::setRemoteGridCacheCapGb(int gb) {
    gb = std::clamp(gb, 1, 10000);
    if (_remoteGridCacheCapGb == gb) return;
    _remoteGridCacheCapGb = gb;
    save();
}

void AppSettings::setCheckUpdatesOnStartup(bool on) {
  if (_checkUpdatesOnStartup == on) return;
  _checkUpdatesOnStartup = on;
  save();
  emit updateSettingsChanged();
}

void AppSettings::setSkippedUpdateVersion(const QString &version) {
  if (_skippedUpdateVersion == version) return;
  _skippedUpdateVersion = version;
  save();
  emit updateSettingsChanged();
}

QString AppSettings::lcurveBinary(const QString &name) const {
  // Look for an executable called `name` inside `dir`; "" if not found.
  auto inDir = [&name](const QString &dir) -> QString {
    if (dir.isEmpty())
      return {};
    const QString candidate = QDir(dir).absoluteFilePath(name);
    QFileInfo fi(candidate);
    if (fi.exists() && fi.isExecutable())
      return fi.absoluteFilePath();
#ifdef Q_OS_WIN
    QFileInfo fiExe(candidate + ".exe");
    if (fiExe.exists())
      return fiExe.absoluteFilePath();
#endif
    return {};
  };

  // 1. User-configured directory always wins (both source and AppImage runs).
  if (QString p = inDir(_lcurveDir); !p.isEmpty())
    return p;

  // 2. Right next to the ASTRA executable. This is where AppImage bundling
  //    (linuxdeploy) drops helper binaries - usr/bin alongside the app - and
  //    the AppImage's AppRun is a bare symlink to the binary, so PATH is not
  //    extended at runtime; resolving against applicationDirPath() is what
  //    actually finds the bundled lcurve inside the AppImage.
  if (QString p = inDir(QCoreApplication::applicationDirPath()); !p.isEmpty())
    return p;

  // 3. Bundled location for an installed tree (`make install`), where helper
  //    binaries live under <prefix>/libexec rather than next to the app. The
  //    prefix-relative path is computed from the install layout at build time.
#ifdef ASTRA_LCURVE_BUNDLE_RELDIR
  {
    const QString bundled =
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral(ASTRA_LCURVE_BUNDLE_RELDIR));
    if (QString p = inDir(bundled); !p.isEmpty())
      return p;
  }
#endif

  // 4. Configure-time source directory: convenience when a developer runs
  //    straight from the build tree (no install step) but has populated it.
#ifdef ASTRA_LCURVE_SOURCE_DIR
  if (QString p = inDir(QStringLiteral(ASTRA_LCURVE_SOURCE_DIR)); !p.isEmpty())
    return p;
#endif

  // 5. Last resort: PATH.
  return QStandardPaths::findExecutable(name);
}