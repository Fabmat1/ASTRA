#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QHash>
#include "models/Photometry.h"

/**
 * Drives the bundled lightcurvequery Python CLI as a child process,
 * streams its log lines, and parses the resulting *_lc.txt files
 * into LightcurvePoint vectors.
 *
 * Output layout (working directory):
 *   <workdir>/lightcurves/<gaia_id>/tess_lc.txt
 *                                  /ztf_lc.txt
 *                                  /atlas_lc.txt
 *                                  /gaia_lc.txt
 *                                  /bg_lc.txt
 */
class LightcurveFetcher : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QStringList sources;        // subset of {"TESS","ZTF","ATLAS","Gaia","BlackGEM"}
        double  trimTess    = 0.0;  // [0, 0.5]
        double  ztfInnerArc = 5.0;
        double  ztfOuterArc = 20.0;
        bool    noPlot      = true; // we never want the matplotlib output
        bool    noBinning   = true;
        bool    noWhitening = true;
        bool       crowding = true;
    };

    explicit LightcurveFetcher(QObject* parent = nullptr);
    ~LightcurveFetcher() override;

    // Configuration (all have sensible defaults from the build).
    static QString defaultPython();
    static QString defaultScript();          // baked-in path from CMake
    static QString defaultRequirements();    // baked-in path from CMake

    void setPython(const QString& py)     { _python = py; }
    void setScript(const QString& s)      { _script = s; }
    void setWorkingDir(const QString& d)  { _workdir = d; }
    void setAtlasToken    (const QString& t) { _atlasToken     = t; }
    void setBlackgemScript(const QString& p) { _blackgemScript = p; }

    QString python()     const { return _python; }
    QString script()     const { return _script; }
    QString workingDir() const { return _workdir; }

    /// Empty if everything looks runnable, otherwise a user-readable message.
    QString checkAvailable() const;
    void checkAvailableAsync();

    bool isRunning() const;

    void start(const QString& gaiaId, const Options& opt);
    void cancel();

    /// Where each source's output file *should* land for this gaia id.
    QHash<QString, QString> expectedOutputFiles(const QString& gaiaId) const;

    /// Parse one of lightcurvequery's *_lc.txt files into a vector of
    /// LightcurvePoint suitable for Photometry::mergeLightcurve(). Returns
    /// empty vector if the file is missing, empty, or marked "NaN, NaN, ...".
    static std::vector<LightcurvePoint>
    parseOutputFile(const QString& path,
                    const QString& source,         // "TESS" / "ZTF" / ...
                    TimeScale*     outScale = nullptr);

signals:
    void started();
    void logLine(const QString& line);   // any stdout/stderr line from the script
    void finished(int exitCode, bool ok);
    void failed(const QString& reason);
    void availabilityChecked(bool ok, const QString& message);
    void rawOutput(const QByteArray& bytes);

private slots:
    void onStdout();
    void onStderr();
    void onFinished(int code, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError err);
    void onProbeFinished(int code, QProcess::ExitStatus status);
    void onProbeErrorOccurred(QProcess::ProcessError err);

private:
    QProcess*  _proc = nullptr;
    QProcess* _probeProc  = nullptr;
    int       _probeStage = 0;

    QString    _python;
    QString    _script;
    QString    _workdir;
    QByteArray _outBuf;
    QByteArray _errBuf;
    QString _atlasToken;
    QString _blackgemScript;
};