#include "LCFitRunner.h"

QString LCFitRunner::methodBinaryName(Method m) {
  switch (m) {
  case Method::LevMarq:
    return "lcurve_levmarq";
  case Method::Mcmc:
    return "lcurve_mcmc";
  case Method::Simplex:
    return "lcurve_simplex";
  }
  return {};
}
QString LCFitRunner::methodLabel(Method m) {
  switch (m) {
  case Method::LevMarq:
    return "Levenberg-Marquardt (fast point fit)";
  case Method::Mcmc:
    return "MCMC (posterior sampling)";
  case Method::Simplex:
    return "Nelder-Mead simplex (robust)";
  }
  return {};
}

LCFitRunner::LCFitRunner(QObject *parent) : QObject(parent) {}
LCFitRunner::~LCFitRunner() {
  if (_proc && _proc->state() != QProcess::NotRunning)
    _proc->kill();
}

bool LCFitRunner::isRunning() const {
  return _proc && _proc->state() != QProcess::NotRunning;
}

void LCFitRunner::start(Method, const QString &configFilename) {
  if (isRunning()) {
    emit failed("Already running");
    return;
  }
  if (_binary.isEmpty()) {
    emit failed("No lcurve binary configured");
    return;
  }

  if (_proc)
    _proc->deleteLater();
  _proc = new QProcess(this);
  _proc->setProcessChannelMode(QProcess::MergedChannels);
  if (!_workDir.isEmpty())
    _proc->setWorkingDirectory(_workDir);

  connect(_proc, &QProcess::readyReadStandardOutput, this,
          &LCFitRunner::onReadyRead);
  connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, &LCFitRunner::onProcFinished);
  connect(_proc, &QProcess::errorOccurred, this, &LCFitRunner::onErrorOccurred);

  emit started();
  emit rawOutput(QString("$ %1 %2\n").arg(_binary, configFilename).toUtf8());
  _proc->start(_binary, {configFilename});
}

void LCFitRunner::cancel() {
  if (!_proc)
    return;
  _proc->terminate();
  if (!_proc->waitForFinished(2000))
    _proc->kill();
}

void LCFitRunner::onReadyRead() {
  emit rawOutput(_proc->readAllStandardOutput());
}
void LCFitRunner::onProcFinished(int code, QProcess::ExitStatus status) {
  emit finished(code, status == QProcess::NormalExit && code == 0);
}
void LCFitRunner::onErrorOccurred(QProcess::ProcessError) {
  emit failed(_proc ? _proc->errorString() : "Process error");
}