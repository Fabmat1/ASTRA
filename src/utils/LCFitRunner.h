#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class LCFitRunner : public QObject {
  Q_OBJECT
public:
  enum class Method { LevMarq, Mcmc, Simplex };
  static QString methodBinaryName(Method m);
  static QString methodLabel(Method m);

  explicit LCFitRunner(QObject *parent = nullptr);
  ~LCFitRunner() override;

  void setBinaryPath(const QString &abs) { _binary = abs; }
  void setWorkingDir(const QString &dir) { _workDir = dir; }

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
};