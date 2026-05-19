#include "Logger.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <algorithm>

#include "utils/AppPaths.h"
// ============================================================================
// Static members
// ============================================================================

Logger* Logger::_instance = nullptr;
QMutex Logger::_instanceMutex;
thread_local QString Logger::_threadName;

// ============================================================================
// Logger Implementation
// ============================================================================

Logger* Logger::instance()
{
    if (!_instance) {
        QMutexLocker locker(&_instanceMutex);
        if (!_instance) {
            _instance = new Logger();
        }
    }
    return _instance;
}


void Logger::initialize(const QString& appName)
{
    Logger* logger = instance();
    logger->_appName = appName;

    // Use centralized path instead of hardcoded applicationDirPath
    logger->setLogDirectory(AppPaths::logs());

    
    // Start the writer thread
    logger->_writerThread = new QThread();
    logger->_writer = new LogWriter(logger);
    logger->_writer->moveToThread(logger->_writerThread);
    
    QObject::connect(logger->_writerThread, &QThread::started, 
                     logger->_writer, &LogWriter::start);
    
    logger->_running = true;
    logger->_writerThread->start();
    
    // Set main thread name
    setThreadName("Main");
    
    // Log startup
    logger->info("Logger", QString("%1 Logger initialized").arg(appName));
}

void Logger::shutdown()
{
    if (!_instance) return;

    _instance->info("Logger", "Logger shutting down");

    QThread*   thread = _instance->_writerThread;
    LogWriter* writer = _instance->_writer;

    if (thread && writer) {
        // The deleteLater connection set up in initialize() won't fire,
        // because the writer thread has no event loop. Drop it and delete manually.
        QObject::disconnect(thread, &QThread::finished,
                            writer, &QObject::deleteLater);

        // Safe direct call: only touches std::atomic + QWaitCondition.
        writer->stop();

        // start() will return -> QThread::run() returns -> thread finishes.
        if (!thread->wait(5000)) {
            qWarning("Logger writer thread did not stop in time, terminating");
            thread->terminate();
            thread->wait();
        }

        delete writer;   // thread is dead, no concurrent access
        delete thread;
    }

    delete _instance;
    _instance = nullptr;
}

Logger::Logger(QObject* parent)
    : QObject(parent)
    , _appName("ASTRA")
    , _maxFileSize(50 * 1024 * 1024)  // 50 MB
    , _maxFileCount(100)
    , _minLogLevel(LogLevel::Debug)
    , _consoleOutput(true)
    , _writerThread(nullptr)
    , _writer(nullptr)
    , _running(false)
{
}

Logger::~Logger()
{
}

void Logger::setLogDirectory(const QString& path)
{
    QMutexLocker locker(&_queueMutex);
    _logDirectory = path;
    
    // Create directory if it doesn't exist
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}

void Logger::setMaxFileSize(qint64 bytes)
{
    _maxFileSize = bytes;
}

void Logger::setMaxFileCount(int count)
{
    _maxFileCount = count;
}

void Logger::setMinLogLevel(LogLevel level)
{
    _minLogLevel = level;
}

void Logger::setConsoleOutput(bool enabled)
{
    _consoleOutput = enabled;
}

void Logger::setThreadName(const QString& name)
{
    _threadName = name;
}

QString Logger::getThreadName()
{
    if (_threadName.isEmpty()) {
        return QString("Thread-%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    }
    return _threadName;
}

void Logger::log(LogLevel level, const QString& category, const QString& message,
                 const char* file, int line, const char* function)
{
    if (level < _minLogLevel) {
        return;
    }
    
    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.level = level;
    entry.threadName = getThreadName();
    entry.threadId = QThread::currentThreadId();
    entry.category = category;
    entry.message = message;
    entry.file = file ? QFileInfo(file).fileName() : QString();
    entry.line = line;
    entry.function = function ? QString(function) : QString();
    
    enqueueEntry(entry);
}

void Logger::debug(const QString& category, const QString& message,
                   const char* file, int line, const char* function)
{
    log(LogLevel::Debug, category, message, file, line, function);
}

void Logger::info(const QString& category, const QString& message,
                  const char* file, int line, const char* function)
{
    log(LogLevel::Info, category, message, file, line, function);
}

void Logger::warning(const QString& category, const QString& message,
                     const char* file, int line, const char* function)
{
    log(LogLevel::Warning, category, message, file, line, function);
}

void Logger::error(const QString& category, const QString& message,
                   const char* file, int line, const char* function)
{
    log(LogLevel::Error, category, message, file, line, function);
}

void Logger::critical(const QString& category, const QString& message,
                      const char* file, int line, const char* function)
{
    log(LogLevel::Critical, category, message, file, line, function);
}

void Logger::enqueueEntry(const LogEntry& entry)
{
    {
        QMutexLocker locker(&_queueMutex);
        _entryQueue.enqueue(entry);
    }
    _queueCondition.wakeOne();
    
    emit logWritten(entry);
}

void Logger::flush()
{
    if (!_writer || !_writerThread || !_writerThread->isRunning())
        return;

    // Wait until the queue is empty. Writer drains it as fast as it can.
    QMutexLocker locker(&_queueMutex);
    while (!_entryQueue.isEmpty()) {
        _queueCondition.wakeAll();
        _queueCondition.wait(&_queueMutex, 20);
    }
}

// ============================================================================
// LogWriter Implementation
// ============================================================================

LogWriter::LogWriter(Logger* logger, QObject* parent)
    : QObject(parent)
    , _logger(logger)
    , _currentFile(nullptr)
    , _stream(nullptr)
    , _currentFileSize(0)
{
}

LogWriter::~LogWriter()
{
    if (_stream) {
        delete _stream;
    }
    if (_currentFile) {
        _currentFile->close();
        delete _currentFile;
    }
}

void LogWriter::start()
{
    _running = true;

    rotateLogFile();
    cleanupOldLogs();

    while (_running.load(std::memory_order_acquire)) {
        processQueue();
    }

    // Final drain after stop()
    {
        QMutexLocker locker(&_logger->_queueMutex);
        while (!_logger->_entryQueue.isEmpty()) {
            LogEntry entry = _logger->_entryQueue.dequeue();
            locker.unlock();
            writeEntry(entry);
            locker.relock();
        }
    }

    if (_stream)      _stream->flush();
    if (_currentFile) _currentFile->flush();

    QThread::currentThread()->quit();
}

void LogWriter::stop()
{
    _running.store(false, std::memory_order_release);
    QMutexLocker locker(&_logger->_queueMutex);  // ensures the writer sees the flag
    _logger->_queueCondition.wakeAll();
}

void LogWriter::flush()
{
    // Process any remaining entries
    QMutexLocker locker(&_logger->_queueMutex);
    while (!_logger->_entryQueue.isEmpty()) {
        LogEntry entry = _logger->_entryQueue.dequeue();
        locker.unlock();
        writeEntry(entry);
        locker.relock();
    }
    
    if (_stream) {
        _stream->flush();
    }
    if (_currentFile) {
        _currentFile->flush();
    }
}

void LogWriter::processQueue()
{
    QMutexLocker locker(&_logger->_queueMutex);

    while (_logger->_entryQueue.isEmpty() &&
           _running.load(std::memory_order_acquire)) {
        _logger->_queueCondition.wait(&_logger->_queueMutex, 100);
    }

    while (!_logger->_entryQueue.isEmpty()) {
        LogEntry entry = _logger->_entryQueue.dequeue();
        locker.unlock();
        writeEntry(entry);
        locker.relock();
    }

    // Notify any thread waiting in Logger::flush()
    _logger->_queueCondition.wakeAll();
}

void LogWriter::writeEntry(const LogEntry& entry)
{
    QString formatted = formatEntry(entry);
    
    // Console output
    if (_logger->_consoleOutput) {
        // Use stderr for warnings and above, stdout for others
        if (entry.level >= LogLevel::Warning) {
            std::cerr << formatted.toStdString() << std::endl;
        } else {
            std::cout << formatted.toStdString() << std::endl;
        }
    }
    
    // File output
    if (_stream && _currentFile) {
        QByteArray data = (formatted + "\n").toUtf8();
        _currentFileSize += data.size();
        
        *_stream << formatted << "\n";
        _stream->flush();
        
        // Check if we need to rotate
        if (_currentFileSize >= _logger->_maxFileSize) {
            rotateLogFile();
        }
    }
}

void LogWriter::rotateLogFile()
{
    // Close current file
    if (_stream) {
        _stream->flush();
        delete _stream;
        _stream = nullptr;
    }
    if (_currentFile) {
        _currentFile->close();
        delete _currentFile;
        _currentFile = nullptr;
    }
    
    // Create new file with timestamp name
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
    QString filename = QString("%1_%2.log").arg(_logger->_appName, timestamp);
    _currentFilePath = _logger->_logDirectory + "/" + filename;
    
    _currentFile = new QFile(_currentFilePath);
    if (_currentFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        _stream = new QTextStream(_currentFile);
        _stream->setEncoding(QStringConverter::Utf8);
        _currentFileSize = _currentFile->size();
        
        // Write header
        *_stream << QString("================================================================================\n");
        *_stream << QString("ASTRA Log File: %1\n").arg(filename);
        *_stream << QString("Started: %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
        *_stream << QString("================================================================================\n\n");
        _stream->flush();
    } else {
        delete _currentFile;
        _currentFile = nullptr;
        std::cerr << "Failed to create log file: " << _currentFilePath.toStdString() << std::endl;
    }
    
    // Cleanup old logs
    cleanupOldLogs();
}

void LogWriter::cleanupOldLogs()
{
    QDir logDir(_logger->_logDirectory);
    QStringList filters;
    filters << QString("%1_*.log").arg(_logger->_appName);
    
    QFileInfoList logFiles = logDir.entryInfoList(filters, QDir::Files, QDir::Time);
    
    // Remove files exceeding the limit (keep newest)
    while (logFiles.size() > _logger->_maxFileCount) {
        QFileInfo oldest = logFiles.takeLast();
        if (oldest.absoluteFilePath() != _currentFilePath) {
            QFile::remove(oldest.absoluteFilePath());
        }
    }
}

QString LogWriter::formatEntry(const LogEntry& entry) const
{
    // Format: [TIMESTAMP] [LEVEL] [THREAD] [CATEGORY] MESSAGE (file:line in function)
    QString formatted;
    
    // Timestamp
    formatted += "[" + entry.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz") + "] ";
    
    // Level with padding
    formatted += "[" + levelToString(entry.level).leftJustified(8) + "] ";
    
    // Thread name
    formatted += "[" + entry.threadName.leftJustified(15) + "] ";
    
    // Category
    if (!entry.category.isEmpty()) {
        formatted += "[" + entry.category + "] ";
    }
    
    // Message
    formatted += entry.message;
    
    // Source location (for debug/warning/error)
    if (entry.level >= LogLevel::Debug && !entry.file.isEmpty()) {
        formatted += QString(" (%1:%2").arg(entry.file).arg(entry.line);
        if (!entry.function.isEmpty()) {
            formatted += QString(" in %1").arg(entry.function);
        }
        formatted += ")";
    }
    
    return formatted;
}

QString LogWriter::levelToString(LogLevel level) const
{
    switch (level) {
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARNING";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

// ============================================================================
// ScopedLogger Implementation
// ============================================================================

ScopedLogger::ScopedLogger(const QString& category, const QString& functionName,
                           const char* file, int line)
    : _category(category)
    , _functionName(functionName)
    , _file(file)
    , _line(line)
{
    _timer.start();
    Logger::instance()->debug(category, QString("Entering %1").arg(functionName),
                              file, line, functionName.toUtf8().constData());
}

ScopedLogger::~ScopedLogger()
{
    qint64 elapsed = _timer.elapsed();
    Logger::instance()->debug(_category, 
        QString("Leaving %1 (took %2 ms)").arg(_functionName).arg(elapsed),
        _file, _line, _functionName.toUtf8().constData());
}

void ScopedLogger::log(LogLevel level, const QString& message)
{
    Logger::instance()->log(level, _category, 
        QString("%1: %2").arg(_functionName, message),
        _file, _line, _functionName.toUtf8().constData());
}