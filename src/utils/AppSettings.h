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

    QStringList gridBasePaths() const { return _gridBasePaths; }
    void        setGridBasePaths(const QStringList& paths);    

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
    QString blackgemScript()   const { return _blackgemScript; }

    void setLcqueryPython  (const QString& p);
    void setLcqueryScript  (const QString& p);
    void setAtlasToken     (const QString& t);
    void setBlackgemScript (const QString& p);


signals:
    void isisBinaryPathChanged();
    void detailGridChanged();
    void gridBasePathsChanged();
    void lcquerySettingsChanged();

private:
    void load();
    void save() const;
    void applyDefaults();

    QString _isisBinaryPath;
    QStringList _gridBasePaths;

    int _rows = 2;
    int _cols = 2;
    QVector<QVector<DetailPanel>> _grid;

    QString _lcqueryPython;
    QString _lcqueryScript;
    QString _atlasToken;
    QString _blackgemScript;
};