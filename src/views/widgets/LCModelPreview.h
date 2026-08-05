#pragma once

#include <QJsonObject>
#include <QProcess>
#include <QWidget>

#include <vector>

#include "models/Photometry.h"

class QCustomPlot;
class QCPGraph;
class QCPErrorBars;
class QLabel;
class QTimer;

// Live forward-model preview for the light-curve fit dialog.
//
// The widget owns a QCustomPlot showing the binned photometry together with
// the LCURVE model produced by the *current* dialog inputs. Model evaluation
// is delegated to `lcurve_re`, the forward-model binary of the same lcurve
// source tree the solvers come from: it consumes the very config the fit would
// run with and writes the model flux at the data phases. Requests are
// debounced, and while an evaluation is in flight the plot is veiled and a
// spinner is shown so a stale curve is never mistaken for the current one.
class LCModelPreview : public QWidget {
    Q_OBJECT
  public:
    explicit LCModelPreview(QWidget *parent = nullptr);
    ~LCModelPreview() override;

    // `binaryPath` is the absolute path to lcurve_re ("" ⇒ preview disabled),
    // `workDir` a scratch directory the preview may write into.
    void setEngine(const QString &binaryPath, const QString &workDir);
    void setObservedData(const std::vector<LCFitDataPoint> &points);

    // Schedule a model evaluation for `config`; supersedes any pending one.
    void requestModel(const QJsonObject &config);
    // Drop any pending request and explain why no model is shown.
    void showNotice(const QString &text);

  private slots:
    void launch();
    void onFinished(int code, QProcess::ExitStatus status);

  private:
    void applyTheme();
    void pushObservedToPlot();
    void setBusy(bool busy);
    bool writeDataFile();
    void setStatus(const QString &text, const QString &colour);

    QString _binary;
    QString _workDir;
    QString _dataPath, _configPath, _outputPath;
    bool    _dataWritten = false;

    std::vector<LCFitDataPoint> _observed;
    QJsonObject                 _pending;
    bool                        _havePending = false;

    QProcess *_proc = nullptr;
    QTimer   *_debounce = nullptr;
    QString   _stdout;
    bool      _killing = false;
    bool      _autoScaled = false;

    QCustomPlot  *_plot = nullptr;
    QCPGraph     *_dataGraph = nullptr;
    QCPGraph     *_modelGraph = nullptr;
    QCPErrorBars *_dataErrors = nullptr;
    QLabel       *_status = nullptr;
    QWidget      *_overlay = nullptr;

  protected:
    void resizeEvent(QResizeEvent *e) override;
};
