#ifndef BACKGROUNDTASKMANAGER_H
#define BACKGROUNDTASKMANAGER_H

#pragma once

#include "controllers/ApplicationController.h"
#include "importWizard/ImportStagingArea.h"
#include "utils/Logger.h"

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
class ImportStagingArea;
class Instrument;

// Base class for background tasks
class BackgroundTask : public QObject
{
    Q_OBJECT

public:
    explicit BackgroundTask(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~BackgroundTask() = default;
    
    virtual QString taskName() const = 0;

    // Staging area support - when set, tasks stage instead of DB-writing
    void setStagingArea(ImportStagingArea* staging) { _stagingArea = staging; }
    ImportStagingArea* stagingArea() const { return _stagingArea; }
    
public slots:
    virtual void execute() = 0;
    
signals:
    void progress(const QString& message);
    void finished(bool success, const QString& message);

protected:
    ImportStagingArea* _stagingArea = nullptr;

};

class GaiaQueryTask : public BackgroundTask
{
    Q_OBJECT
public:
    // From staging (wizard path)
    GaiaQueryTask(ImportStagingArea* staging,
                  const QString& projectId,
                  ApplicationController* controller,
                  QObject* parent = nullptr);

    // Standalone (no staging)
    GaiaQueryTask(std::vector<std::shared_ptr<Star>> stars,
                  const QString& projectId,
                  ApplicationController* controller,
                  QObject* parent = nullptr);

    void execute() override;
    QString taskName() const override { return "Gaia Query"; }

signals:
    void queryComplete(int updated, int failed);

private:
    bool starNeedsGaiaData(const std::shared_ptr<Star> &star) const;

    // Source-id based query (batched)
    QString
    buildSourceIdQuery(const std::vector<std::shared_ptr<Star>> &stars) const;
    int parseSourceIdResponse(const QString &response,
                              const std::vector<std::shared_ptr<Star>> &stars,
                              std::vector<std::shared_ptr<Star>> &modified);

    // Positional cross-match (stars without a Gaia source id), via TAP upload
    QString buildPositionalQuery() const;
    QByteArray
    buildPositionVOTable(const std::vector<std::shared_ptr<Star>> &stars) const;
    int
    parsePositionalResponse(const QString                            &response,
                            const std::vector<std::shared_ptr<Star>> &posStars,
                            std::vector<std::shared_ptr<Star>>       &modified);

    // Shared
    bool    applyGaiaRow(const std::shared_ptr<Star> &star,
                         const QMap<QString, int>    &colIndex,
                         const QStringList &values, bool setSourceId);
    QString sendSyncQuery(const QString &adql, QString &error);
    QString sendUploadQuery(const QString &adql, const QByteArray &votable,
                            QString &error);

    std::vector<std::shared_ptr<Star>> _stars;
    ImportStagingArea                 *_stagingArea = nullptr;
    QString                            _projectId;
    ApplicationController             *_controller;
    QNetworkAccessManager             *_networkManager = nullptr;
};

class SimbadQueryTask : public BackgroundTask
{
    Q_OBJECT
public:
    // From staging (wizard path)
    SimbadQueryTask(ImportStagingArea* staging,
                    const QString& projectId,
                    ApplicationController* controller,
                    QObject* parent = nullptr);

    // Standalone (no staging)
    SimbadQueryTask(std::vector<std::shared_ptr<Star>> stars,
                    const QString& projectId,
                    ApplicationController* controller,
                    QObject* parent = nullptr);

    void execute() override;
    QString taskName() const override { return "SIMBAD Query"; }

signals:
    void queryComplete(int updated, int failed);

private:
    QString generateSimbadScript();
    QMap<QString, QStringList> parseSimbadResponse(const QString& response);

    std::vector<std::shared_ptr<Star>> _stars;
    ImportStagingArea* _stagingArea = nullptr;
    QString _projectId;
    ApplicationController* _controller;
    QNetworkAccessManager* _networkManager = nullptr;
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
    void setAutoDetectInstrument(bool enabled,
                                  std::vector<std::shared_ptr<Instrument>> instruments);

public slots:
    void execute() override;

signals:
    void spectrumImported(std::shared_ptr<Star> star, std::shared_ptr<Spectrum> spectrum);
    void importComplete(int imported, int failed);

private:
    std::vector<SpectrumImportEntry> _entries;
    QString _projectId;
    ApplicationController* _controller;
    bool _autoDetectInstrument = false;
    std::vector<std::shared_ptr<Instrument>> _instruments;
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
// IsisFitImportTask - Background task for importing ISIS spectral fits
// ============================================================================

struct IsisFitImportEntry {
    QString                      starId;
    QString                      spectrumId;
    std::shared_ptr<Spectrum>    spectrum;
    std::shared_ptr<SpectralFit> fit;
    QString                      modelDataPath; // <id>_id_<N>.dat
};

class IsisFitImportTask : public BackgroundTask {
    Q_OBJECT

  public:
    IsisFitImportTask(std::vector<IsisFitImportEntry> entries,
                      ApplicationController          *controller,
                      QObject                        *parent = nullptr);

    QString taskName() const override { return "ISIS Fit Import"; }

  public slots:
    void execute() override;

  signals:
    void importComplete(int imported, int failed);

  private:
    std::vector<IsisFitImportEntry> _entries;
    ApplicationController          *_controller;
};

// ============================================================================
// RVExtractionTask - Extract RV from spectral fits in background
// ============================================================================

struct RVExtractionResult {
    QString starId;
    std::shared_ptr<RadialVelocityCurve> curve;
    std::shared_ptr<RVFit> fit;  // optional orbital fit
};

struct RVSystematicConfig {
    bool enabled = false;

    struct ResolutionBand {
        double maxResolution;
        double systematicKmS;
    };
    std::vector<ResolutionBand> resolutionBands;

    QHash<QString, double> instrumentModeOverrides;

    double systematicForResolution(double R) const {
        for (const auto& band : resolutionBands) {
            if (R < band.maxResolution)
                return band.systematicKmS;
        }
        return resolutionBands.empty() ? 0.0 : resolutionBands.back().systematicKmS;
    }

    static RVSystematicConfig defaultConfig() {
        RVSystematicConfig cfg;
        cfg.enabled = true;
        cfg.resolutionBands.push_back({4000.0, 15.0});
        cfg.resolutionBands.push_back({20000.0, 10.0});
        cfg.resolutionBands.push_back({1e12, 3.0});
        return cfg;
    }
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
        QString namingType;
        QChar delimiter;
        bool hasHeader;
        int timeCol;
        int rvCol;
        int rvErrCol;
        int sysErrCol = -1;
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
        int sysErrCol = -1;
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
    void setSystematicConfig(const RVSystematicConfig& config,
                             std::vector<std::shared_ptr<Instrument>> instruments = {});

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

    RVSystematicConfig _systematicConfig;
    std::vector<std::shared_ptr<Instrument>> _instruments;
    double resolveResolutionForSpectrum(const std::shared_ptr<Spectrum>& spectrum) const;
    double determineSystematicForSpectrum(const std::shared_ptr<Spectrum>& spectrum) const;
};

class StagingSeedTask : public BackgroundTask {
    Q_OBJECT
  public:
    StagingSeedTask(ImportStagingArea *staging, QString projectId,
                    ApplicationController *controller,
                    QObject               *parent = nullptr)
        : BackgroundTask(parent), _staging(staging),
          _projectId(std::move(projectId)), _controller(controller) {}

    QString taskName() const override { return "Loading project stars"; }

  public slots:
    void execute() override {
        LOG_SET_THREAD_NAME("StagingSeed");
        _staging->seedFromDB(
            _controller->databaseManager(), _projectId,
            [this](int done, int total) {
                const int pct = total > 0 ? done * 100 / total : 100;
                emit progress(QString("Loading stars & spectra: %1 / %2 (%3%)")
                                  .arg(done)
                                  .arg(total)
                                  .arg(pct));
            });
        emit finished(true, "Project stars loaded");
    }

  private:
    ImportStagingArea     *_staging;
    QString                _projectId;
    ApplicationController *_controller;
};

#endif // BACKGROUNDTASKMANAGER_H