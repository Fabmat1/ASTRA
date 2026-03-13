#ifndef BACKGROUNDTASKMANAGER_H
#define BACKGROUNDTASKMANAGER_H
#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QHash>          
#include <QTimer>
#include <QLabel>

#include <memory>
#include <vector>
#include <atomic>

// Forward declarations
class QStatusBar;
class QNetworkAccessManager;
class ApplicationController;
class Star;
class Spectrum;
class SpectralFit;
class RadialVelocityCurve;   
class RadialVelocityPoint;   
class RVFit;                 
class Project;               

// Base class for background tasks
class BackgroundTask : public QObject
{
    Q_OBJECT

public:
    explicit BackgroundTask(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~BackgroundTask() = default;
    
    virtual QString taskName() const = 0;
    
public slots:
    virtual void execute() = 0;
    
signals:
    void progress(const QString& message);
    void finished(bool success, const QString& message);
};

// Gaia query task
class GaiaQueryTask : public BackgroundTask
{
    Q_OBJECT

public:
    GaiaQueryTask(std::vector<std::shared_ptr<Star>> stars,
                  const QString& projectId,
                  ApplicationController* controller,
                  QObject* parent = nullptr);
    
    QString taskName() const override { return "Gaia DR3 Query"; }
    
public slots:
    void execute() override;
    
private:
    std::vector<std::shared_ptr<Star>> _stars;
    QString _projectId;
    ApplicationController* _controller;
    QNetworkAccessManager* _networkManager;
    
    bool starNeedsGaiaData(const std::shared_ptr<Star>& star) const;
    QString buildADQLQuery();
    void parseVizierResponse(const QString& response);
};

// SIMBAD query task
class SimbadQueryTask : public BackgroundTask
{
    Q_OBJECT

public:
    SimbadQueryTask(std::vector<std::shared_ptr<Star>> stars,
                    const QString& projectId,
                    ApplicationController* controller,
                    QObject* parent = nullptr);
    
    QString taskName() const override { return "SIMBAD Bibliography Query"; }
    
public slots:
    void execute() override;
    
private:
    std::vector<std::shared_ptr<Star>> _stars;
    QString _projectId;
    ApplicationController* _controller;
    QNetworkAccessManager* _networkManager;
    
    QString generateSimbadScript();
    QMap<QString, QStringList> parseSimbadResponse(const QString& response);
};

// Status bar widget with spinner
class TaskStatusWidget : public QObject
{
    Q_OBJECT

public:
    explicit TaskStatusWidget(QStatusBar* statusBar, QObject* parent = nullptr);
    ~TaskStatusWidget();
    
    void addTask(const QString& taskName);
    void updateTaskMessage(const QString& taskName, const QString& message);
    void removeTask(const QString& taskName);
    void showTemporaryMessage(const QString& message, int timeout);
    
private slots:
    void updateSpinner();
    void cycleMessages();
    
private:
    void updateDisplay();
    
    QStatusBar* _statusBar;
    QLabel* _spinnerLabel;
    QLabel* _messageLabel;
    QTimer* _spinnerTimer;
    QTimer* _cycleTimer;
    
    struct TaskInfo {
        QString name;
        QString message;
    };
    
    QList<TaskInfo> _activeTasks;
    int _currentTaskIndex;
    int _spinnerFrame;
    QString _temporaryMessage;
    QTimer* _temporaryTimer;
    
    static const QStringList _spinnerFrames;
};

// Manager for background tasks
class BackgroundTaskManager : public QObject
{
    Q_OBJECT

public:
    explicit BackgroundTaskManager(QObject* parent = nullptr);
    ~BackgroundTaskManager();
    
    void queueTask(BackgroundTask* task);
    bool hasActiveTasks() const;
    int activeTaskCount() const;
    
    void setStatusBar(QStatusBar* statusBar);
    
signals:
    void taskStarted(const QString& taskName);
    void taskFinished(const QString& taskName, bool success);
    void allTasksComplete();
    
private slots:
    void onTaskProgress(const QString& message);
    void onTaskFinished(bool success, const QString& message);
    void processNextTask();
    
private:
    struct TaskEntry {
        BackgroundTask* task;
        QThread* thread;
    };
    
    QQueue<BackgroundTask*> _pendingTasks;
    QList<TaskEntry> _activeTasks;
    mutable QMutex _mutex;
    int _maxConcurrentTasks;
    
    std::unique_ptr<TaskStatusWidget> _statusWidget;
};

// ============================================================================
// SpectraImportTask - Background task for importing spectra
// ============================================================================

struct SpectrumImportEntry {
    QString spectrumFile;
    std::shared_ptr<Star> matchedStar;
    int sourceRowIndex = -1;  // Index in mapping rows, -1 if from FITS scan
    
    // Metadata from mapping file (if applicable)
    std::optional<double> mjd;
    std::optional<double> bjd;
    std::optional<double> exposureTime;
    std::optional<QString> instrument;
    bool isBarycentricallyCorrected = false;
};

class SpectraImportTask : public BackgroundTask
{
    Q_OBJECT

public:
    SpectraImportTask(std::vector<SpectrumImportEntry> entries,
                      const QString& projectId,
                      ApplicationController* controller,
                      QObject* parent = nullptr);

    QString taskName() const override { return "Spectra Import"; }

public slots:
    void execute() override;

signals:
    void spectrumImported(std::shared_ptr<Star> star, std::shared_ptr<Spectrum> spectrum);
    void importComplete(int imported, int failed);

private:
    std::vector<SpectrumImportEntry> _entries;
    QString _projectId;
    ApplicationController* _controller;
};


// ============================================================================
// DiggaFitImportTask - Background task for importing DIGGA spectral fits
// ============================================================================

struct DiggaFitImportEntry {
    QString starId;
    QString spectrumId;
    std::shared_ptr<Spectrum> spectrum;
    std::shared_ptr<SpectralFit> fit;
    QString plotdataPath;
};

class DiggaFitImportTask : public BackgroundTask
{
    Q_OBJECT

public:
    DiggaFitImportTask(std::vector<DiggaFitImportEntry> entries,
                       ApplicationController* controller,
                       QObject* parent = nullptr);

    QString taskName() const override { return "DIGGA Fit Import"; }

public slots:
    void execute() override;

signals:
    void importComplete(int imported, int failed);

private:
    std::vector<DiggaFitImportEntry> _entries;
    ApplicationController* _controller;
};

// ============================================================================
// RVExtractionTask — Extract RV from spectral fits in background
// ============================================================================

struct RVExtractionResult {
    QString starId;
    std::shared_ptr<RadialVelocityCurve> curve;
    std::shared_ptr<RVFit> fit;  // optional orbital fit
};

class RVExtractionTask : public BackgroundTask
{
    Q_OBJECT

public:
    // Mode 1: From spectral fits
    static RVExtractionTask* createFromFits(
        std::vector<std::shared_ptr<Star>> stars,
        const QString& projectId,
        ApplicationController* controller,
        bool bestFitOnly,
        bool skipZeroRV,
        QObject* parent = nullptr);

    // Mode 2: From per-star folders
    struct FolderConfig {
        QString rootPath;
        QString namingType;   // "source_id", "alias", "ra_dec"
        QChar delimiter;
        bool hasHeader;
        int timeCol;
        int rvCol;
        int rvErrCol;
        bool isBJD;
    };

    static RVExtractionTask* createFromFolders(
        std::vector<std::shared_ptr<Star>> stars,
        const QString& projectId,
        ApplicationController* controller,
        const FolderConfig& config,
        QObject* parent = nullptr);

    // Mode 3: From single table
    struct TableConfig {
        QStringList columns;
        std::vector<QStringList> rows;
        QString idType;
        int idCol;
        int timeCol;
        int rvCol;
        int rvErrCol;
        bool isBJD;
    };

    static RVExtractionTask* createFromTable(
        std::vector<std::shared_ptr<Star>> stars,
        const QString& projectId,
        ApplicationController* controller,
        const TableConfig& config,
        QObject* parent = nullptr);

    QString taskName() const override { return _taskName; }

    const std::vector<RVExtractionResult>& results() const { return _results; }

public slots:
    void execute() override;

signals:
    void extractionComplete(int numCurves, int numPoints);

private:
    enum Mode { FromFits, FromFolders, FromTable };

    explicit RVExtractionTask(
        std::vector<std::shared_ptr<Star>> stars,
        const QString& projectId,
        ApplicationController* controller,
        Mode mode,
        QObject* parent = nullptr);

    void executeFromFits();
    void executeFromFolders();
    void executeFromTable();

    std::shared_ptr<Star> findStarByIdentifier(const QString& id, const QString& idType) const;
    void buildStarLookupIndex();

    // Shared
    std::vector<std::shared_ptr<Star>> _stars;
    QString _projectId;
    ApplicationController* _controller;
    Mode _mode;
    QString _taskName;
    std::vector<RVExtractionResult> _results;

    // Star lookup
    QHash<QString, std::shared_ptr<Star>> _sourceIdIndex;
    QHash<QString, std::shared_ptr<Star>> _aliasIndex;

    // Mode-specific config
    bool _bestFitOnly = true;
    bool _skipZeroRV = true;
    FolderConfig _folderConfig;
    TableConfig _tableConfig;

    void saveResultsToDatabase();
};

#endif // BACKGROUNDTASKMANAGER_H