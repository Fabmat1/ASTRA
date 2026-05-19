#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <QFile>
#include <QDateTime>
#include <QTextStream>
#include <QElapsedTimer>  
#include <memory>
#include <atomic>

// Log levels
enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    Critical = 4
};

// Log entry structure
struct LogEntry {
    QDateTime timestamp;
    LogLevel level;
    QString threadName;
    Qt::HANDLE threadId;
    QString category;
    QString message;
    QString file;
    int line;
    QString function;
};

// Forward declaration
class LogWriter;

// Main Logger class - singleton
class Logger : public QObject
{
    Q_OBJECT

public:
    static Logger* instance();
    static void initialize(const QString& appName = "ASTRA");
    static void shutdown();
    
    // Configuration
    void setLogDirectory(const QString& path);
    void setMaxFileSize(qint64 bytes);
    void setMaxFileCount(int count);
    void setMinLogLevel(LogLevel level);
    void setConsoleOutput(bool enabled);
    
    // Logging methods
    void log(LogLevel level, const QString& category, const QString& message,
             const char* file = nullptr, int line = 0, const char* function = nullptr);
    
    // Convenience methods
    void debug(const QString& category, const QString& message,
               const char* file = nullptr, int line = 0, const char* function = nullptr);
    void info(const QString& category, const QString& message,
              const char* file = nullptr, int line = 0, const char* function = nullptr);
    void warning(const QString& category, const QString& message,
                 const char* file = nullptr, int line = 0, const char* function = nullptr);
    void error(const QString& category, const QString& message,
               const char* file = nullptr, int line = 0, const char* function = nullptr);
    void critical(const QString& category, const QString& message,
                  const char* file = nullptr, int line = 0, const char* function = nullptr);
    
    // Thread naming
    static void setThreadName(const QString& name);
    static QString getThreadName();
    
    // Flush pending logs (useful before shutdown)
    void flush();
    
signals:
    void logWritten(const LogEntry& entry);
    
private:
    explicit Logger(QObject* parent = nullptr);
    ~Logger();
    
    void enqueueEntry(const LogEntry& entry);
    
    static Logger* _instance;
    static QMutex _instanceMutex;
    static thread_local QString _threadName;
    
    QString _appName;
    QString _logDirectory;
    qint64 _maxFileSize;
    int _maxFileCount;
    LogLevel _minLogLevel;
    bool _consoleOutput;
    
    QThread* _writerThread;
    LogWriter* _writer;
    
    QMutex _queueMutex;
    QQueue<LogEntry> _entryQueue;
    QWaitCondition _queueCondition;
    bool _running;
    
    friend class LogWriter;
};

// Worker class that writes logs in its own thread
class LogWriter : public QObject
{
    Q_OBJECT

public:
    explicit LogWriter(Logger* logger, QObject* parent = nullptr);
    ~LogWriter();
    
public slots:
    void start();
    void stop();
    void flush();
    
private:
    void processQueue();
    void writeEntry(const LogEntry& entry);
    void rotateLogFile();
    void cleanupOldLogs();
    QString formatEntry(const LogEntry& entry) const;
    QString levelToString(LogLevel level) const;
    
    Logger* _logger;
    QFile* _currentFile;
    QTextStream* _stream;
    QString _currentFilePath;
    qint64 _currentFileSize;
    std::atomic<bool> _running { false };
};

// Scoped logger for function entry/exit tracking
class ScopedLogger
{
public:
    ScopedLogger(const QString& category, const QString& functionName,
                 const char* file, int line);
    ~ScopedLogger();
    
    void log(LogLevel level, const QString& message);
    
private:
    QString _category;
    QString _functionName;
    const char* _file;
    int _line;
    QElapsedTimer _timer;
};

// ============================================================================
// Logging Macros
// ============================================================================

// Basic logging macros
#define LOG_DEBUG(category, message) \
    Logger::instance()->debug(category, message, __FILE__, __LINE__, __FUNCTION__)

#define LOG_INFO(category, message) \
    Logger::instance()->info(category, message, __FILE__, __LINE__, __FUNCTION__)

#define LOG_WARNING(category, message) \
    Logger::instance()->warning(category, message, __FILE__, __LINE__, __FUNCTION__)

#define LOG_ERROR(category, message) \
    Logger::instance()->error(category, message, __FILE__, __LINE__, __FUNCTION__)

#define LOG_CRITICAL(category, message) \
    Logger::instance()->critical(category, message, __FILE__, __LINE__, __FUNCTION__)

// Formatted logging macros (using QString::arg)
#define LOG_DEBUG_F(category, ...) \
    Logger::instance()->debug(category, QString(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

#define LOG_INFO_F(category, ...) \
    Logger::instance()->info(category, QString(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

#define LOG_WARNING_F(category, ...) \
    Logger::instance()->warning(category, QString(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

#define LOG_ERROR_F(category, ...) \
    Logger::instance()->error(category, QString(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

#define LOG_CRITICAL_F(category, ...) \
    Logger::instance()->critical(category, QString(__VA_ARGS__), __FILE__, __LINE__, __FUNCTION__)

// Function scope tracking macro
#define LOG_FUNCTION(category) \
    ScopedLogger _scopedLogger_(category, __FUNCTION__, __FILE__, __LINE__)

// Task/operation tracking macros
#define LOG_TASK_START(category, taskName) \
    LOG_INFO(category, QString("Starting: %1").arg(taskName))

#define LOG_TASK_END(category, taskName) \
    LOG_INFO(category, QString("Completed: %1").arg(taskName))

#define LOG_TASK_FAIL(category, taskName, reason) \
    LOG_ERROR(category, QString("Failed: %1 - %2").arg(taskName, reason))

// Thread naming macro
#define LOG_SET_THREAD_NAME(name) \
    Logger::setThreadName(name)

#endif // LOGGER_H