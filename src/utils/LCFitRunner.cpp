#include "LCFitRunner.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLibrary>
#include <QProcessEnvironment>
#include <QStringList>

LCFitRunner::CudaStatus LCFitRunner::cudaStatus() {
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
  QLibrary driver;
#ifdef Q_OS_WIN
  driver.setFileName(QStringLiteral("nvcuda"));
#else
  driver.setFileNameAndVersion(QStringLiteral("cuda"), 1);
#endif

  if (!driver.load())
    return {false, QObject::tr("NVIDIA CUDA driver not available")};

  using CuInit = int (*)(unsigned int);
  using CuDeviceGetCount = int (*)(int *);
  using CuDeviceGet = int (*)(int *, int);
  using CuDeviceComputeCapability = int (*)(int *, int *, int);
  using CuDeviceGetName = int (*)(char *, int, int);

  const auto cuInit = reinterpret_cast<CuInit>(driver.resolve("cuInit"));
  const auto cuDeviceGetCount =
      reinterpret_cast<CuDeviceGetCount>(driver.resolve("cuDeviceGetCount"));
  const auto cuDeviceGet =
      reinterpret_cast<CuDeviceGet>(driver.resolve("cuDeviceGet"));
  const auto cuDeviceComputeCapability =
      reinterpret_cast<CuDeviceComputeCapability>(
          driver.resolve("cuDeviceComputeCapability"));
  const auto cuDeviceGetName =
      reinterpret_cast<CuDeviceGetName>(driver.resolve("cuDeviceGetName"));

  if (!cuInit || !cuDeviceGetCount || !cuDeviceGet ||
      !cuDeviceComputeCapability)
    return {false, QObject::tr("CUDA driver API is incomplete")};
  if (cuInit(0) != 0)
    return {false, QObject::tr("CUDA driver could not be initialized")};

  int deviceCount = 0;
  if (cuDeviceGetCount(&deviceCount) != 0 || deviceCount <= 0)
    return {false, QObject::tr("No NVIDIA CUDA device detected")};

  QStringList detected;
  for (int ordinal = 0; ordinal < deviceCount; ++ordinal) {
    int device = 0;
    int major = 0;
    int minor = 0;
    if (cuDeviceGet(&device, ordinal) != 0 ||
        cuDeviceComputeCapability(&major, &minor, device) != 0)
      continue;

    QString name = QObject::tr("CUDA device %1").arg(ordinal);
    if (cuDeviceGetName) {
      char buffer[256]{};
      if (cuDeviceGetName(buffer, int(sizeof(buffer)), device) == 0 &&
          buffer[0] != '\0')
        name = QString::fromUtf8(buffer);
    }
    detected << QObject::tr("%1 (compute capability %2.%3)")
                    .arg(name)
                    .arg(major)
                    .arg(minor);

    // The default lcurve CUDA build contains native Turing/Ampere kernels
    // and forward-compatible PTX starting at compute capability 7.5.
    if (major > 7 || (major == 7 && minor >= 5))
      return {true,
              QObject::tr("Compatible GPU detected: %1").arg(detected.back()),
              ordinal};
  }

  return {false,
          detected.isEmpty()
              ? QObject::tr("CUDA devices could not be queried")
              : QObject::tr("LCURVE CUDA requires compute capability 7.5 or "
                            "newer; detected: %1")
                    .arg(detected.join(QStringLiteral(", ")))};
#else
  return {false,
          QObject::tr("CUDA acceleration is unavailable on this platform")};
#endif
}

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
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  // Set both states explicitly so an inherited shell setting cannot override
  // the switch in the fit dialog.
  env.insert(QStringLiteral("LCURVE_CUDA"),
             _cudaEnabled ? QStringLiteral("1") : QStringLiteral("0"));
  if (_cudaEnabled && _cudaDevice >= 0)
    env.insert(QStringLiteral("LCURVE_CUDA_DEVICE"),
               QString::number(_cudaDevice));
  _proc->setProcessEnvironment(env);
  if (!_workDir.isEmpty())
    _proc->setWorkingDirectory(_workDir);

  connect(_proc, &QProcess::readyReadStandardOutput, this,
          &LCFitRunner::onReadyRead);
  connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, &LCFitRunner::onProcFinished);
  connect(_proc, &QProcess::errorOccurred, this, &LCFitRunner::onErrorOccurred);

  emit started();
  emit rawOutput(
      QString("[info] CUDA acceleration %1\n")
          .arg(_cudaEnabled ? QStringLiteral("requested")
                            : QStringLiteral("disabled"))
          .toUtf8());
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
  if (!_proc)
    return;
  _outputBuffer += _proc->readAllStandardOutput();
  drainOutput(false);
}

void LCFitRunner::drainOutput(bool flush) {
  static const QByteArray marker("@@LCURVE_PLOT@@");

  while (!_outputBuffer.isEmpty()) {
    const qsizetype markerPos = _outputBuffer.indexOf(marker);
    if (markerPos < 0) {
      // Keep enough trailing bytes to recognize a marker split across two
      // QProcess readyRead notifications.
      const qsizetype keep = flush ? 0 : marker.size() - 1;
      const qsizetype count = _outputBuffer.size() - keep;
      if (count > 0) {
        emit rawOutput(_outputBuffer.left(count));
        _outputBuffer.remove(0, count);
      }
      return;
    }

    if (markerPos > 0) {
      emit rawOutput(_outputBuffer.left(markerPos));
      _outputBuffer.remove(0, markerPos);
    }

    const qsizetype newline = _outputBuffer.indexOf('\n', marker.size());
    if (newline < 0) {
      if (flush) {
        emit rawOutput(_outputBuffer);
        _outputBuffer.clear();
      }
      return;
    }

    const QByteArray completeLine = _outputBuffer.left(newline + 1);
    const QByteArray payload =
        _outputBuffer.mid(marker.size(), newline - marker.size()).trimmed();
    _outputBuffer.remove(0, newline + 1);

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject() &&
        doc.object().value(QStringLiteral("protocol")).toString() ==
            QStringLiteral("lcurve.plot.v1")) {
      emit plotFrame(doc.object());
    } else {
      // Keep malformed or unknown frames visible in the terminal.
      emit rawOutput(completeLine);
    }
  }
}

void LCFitRunner::onProcFinished(int code, QProcess::ExitStatus status) {
  onReadyRead();
  drainOutput(true);
  emit finished(code, status == QProcess::NormalExit && code == 0);
}
void LCFitRunner::onErrorOccurred(QProcess::ProcessError) {
  emit failed(_proc ? _proc->errorString() : "Process error");
}
