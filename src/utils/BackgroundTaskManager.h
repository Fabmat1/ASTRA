#ifndef BACKGROUNDTASKMANAGER_H
#define BACKGROUNDTASKMANAGER_H

#include <QObject>
#include <QThread>
#include <QQueue>
#include <QMutex>
#include <QTimer>
#include <memory>
#include <vector>
#include <functional>
#include <optional>

class Star;
class Spectrum;
class SpectralFit;
class ApplicationController;
class QNetworkAccessManager;
class QLabel;
class QStatusBar;

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

#endif // BACKGROUNDTASKMANAGER_H