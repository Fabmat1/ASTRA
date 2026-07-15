#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class LCFitRunner : public QObject {
  Q_OBJECT
public:
  enum class Method { LevMarq, Mcmc, Simplex };
  struct CudaStatus {
    bool available = false;
    QString description;
    int deviceIndex = -1;
  };

  static QString methodBinaryName(Method m);
  static QString methodLabel(Method m);
  static CudaStatus cudaStatus();

  explicit LCFitRunner(QObject *parent = nullptr);
  ~LCFitRunner() override;

  void setBinaryPath(const QString &abs) { _binary = abs; }
  void setWorkingDir(const QString &dir) { _workDir = dir; }
  void setCudaEnabled(bool enabled) { _cudaEnabled = enabled; }
  void setCudaDevice(int index) { _cudaDevice = index; }

  bool isRunning() const;
  void start(Method m, const QString &configFilename);
  void cancel();

signals:
  void started();
  void rawOutput(const QByteArray &bytes);
  void finished(int exitCode, bool ok);
  void failed(const QString &reason);

private slots:
  void onReadyRead();
  void onProcFinished(int code, QProcess::ExitStatus status);
  void onErrorOccurred(QProcess::ProcessError err);

private:
  QProcess *_proc = nullptr;
  QString _binary;
  QString _workDir;
  bool _cudaEnabled = false;
  int _cudaDevice = -1;
};
