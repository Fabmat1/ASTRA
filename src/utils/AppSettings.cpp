#include "AppSettings.h"

#include <QSettings>
#include <QStringList>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace {
constexpr const char* kGroup      = "AppSettings";
constexpr const char* kIsisBinary = "general/isisBinary";
constexpr const char* kRows       = "starDetail/rows";
constexpr const char* kCols       = "starDetail/cols";
constexpr const char* kGrid       = "starDetail/grid";
constexpr const char* kGridPaths  = "gridPaths/all";
constexpr const char* kLcqPython     = "lcquery/python";
constexpr const char* kLcqScript     = "lcquery/script";
constexpr const char* kAtlasToken    = "lcquery/atlasToken";
constexpr const char* kBlackgemScr   = "lcquery/blackgemScript";
constexpr const char* kLcurveDir = "lcurve/installDir";
}

QString AppSettings::panelName(DetailPanel p)
{
    switch (p) {
        case DetailPanel::None:           return "— Empty —";
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
    _gridBasePaths = { home + "/ISIS_models",
                       home + "/isis/synthetic_spectra/grids",
                       "/data/stellar/modelgrids" };
    
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

    int rows = std::clamp(s.value(kRows, _rows).toInt(), kMinGridDim, kMaxGridDim);
    int cols = std::clamp(s.value(kCols, _cols).toInt(), kMinGridDim, kMaxGridDim);

    QString flat = s.value(kGrid).toString();
    _gridBasePaths = s.value(kGridPaths, _gridBasePaths).toStringList();

    _lcqueryPython   = s.value(kLcqPython,    _lcqueryPython  ).toString();
    _lcqueryScript   = s.value(kLcqScript,    _lcqueryScript  ).toString();
    _atlasToken      = s.value(kAtlasToken,   _atlasToken     ).toString();
    _blackgemScript  = s.value(kBlackgemScr,  _blackgemScript ).toString();
    _lcurveDir = s.value(kLcurveDir, _lcurveDir).toString();
    s.endGroup();

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
    s.setValue(kRows, _rows);
    s.setValue(kCols, _cols);

    QStringList flat;
    for (int r = 0; r < _rows; ++r)
        for (int c = 0; c < _cols; ++c)
            flat << QString::number(static_cast<int>(_grid[r][c]));
    s.setValue(kGrid, flat.join(','));
    s.setValue(kGridPaths, _gridBasePaths);

    s.setValue(kLcqPython,    _lcqueryPython);
    s.setValue(kLcqScript,    _lcqueryScript);
    s.setValue(kAtlasToken,   _atlasToken);
    s.setValue(kBlackgemScr,  _blackgemScript);
    s.setValue(kLcurveDir, _lcurveDir);

    s.endGroup();
    s.sync();
}

void AppSettings::setIsisBinaryPath(const QString& path)
{
    if (_isisBinaryPath == path) return;
    _isisBinaryPath = path;
    save();
    emit isisBinaryPathChanged();
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

QString AppSettings::lcurveBinary(const QString &name) const {
  if (!_lcurveDir.isEmpty()) {
    const QString candidate = QDir(_lcurveDir).absoluteFilePath(name);
    QFileInfo fi(candidate);
    if (fi.exists() && fi.isExecutable())
      return fi.absoluteFilePath();
#ifdef Q_OS_WIN
    QFileInfo fiExe(candidate + ".exe");
    if (fiExe.exists())
      return fiExe.absoluteFilePath();
#endif
  }
  return QStandardPaths::findExecutable(name);
}