#include "BackgroundTaskManager.h"
#include "controllers/ApplicationController.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"    
#include "models/Project.h"        
#include "Logger.h"
#include "utils/DatabaseManager.h"
#include "ImportStagingArea.h"
#include "Logger.h"  

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <QHttpMultiPart>
#include <QDebug>
#include <QStatusBar>
#include <QLabel>
#include <QHBoxLayout>
#include <QtConcurrent>
#include <QFutureSynchronizer>
#include "SpectrumReader.h"
#include "SpectralFitImportPage.h" 

// ============================================================================
// TaskStatusWidget Implementation
// ============================================================================

const QStringList TaskStatusWidget::_spinnerFrames = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

TaskStatusWidget::TaskStatusWidget(QStatusBar* statusBar, QObject* parent)
    : QObject(parent)
    , _statusBar(statusBar)
    , _spinnerLabel(nullptr)
    , _messageLabel(nullptr)
    , _spinnerTimer(new QTimer(this))
    , _cycleTimer(new QTimer(this))
    , _currentTaskIndex(0)
    , _spinnerFrame(0)
    , _temporaryTimer(new QTimer(this))
{
    // Create widgets
    _spinnerLabel = new QLabel();
    _spinnerLabel->setFixedWidth(20);
    _spinnerLabel->setStyleSheet("QLabel { font-size: 14px; }");
    _spinnerLabel->hide();
    
    _messageLabel = new QLabel();
    _messageLabel->hide();
    
    // Add to status bar
    _statusBar->addPermanentWidget(_spinnerLabel);
    _statusBar->addPermanentWidget(_messageLabel, 1);
    
    // Setup timers
    connect(_spinnerTimer, &QTimer::timeout, this, &TaskStatusWidget::updateSpinner);
    connect(_cycleTimer, &QTimer::timeout, this, &TaskStatusWidget::cycleMessages);
    connect(_temporaryTimer, &QTimer::timeout, this, [this]() {
        _temporaryMessage.clear();
        updateDisplay();
    });
    
    _spinnerTimer->setInterval(80);  // Fast spinner
    _cycleTimer->setInterval(3000);  // Cycle messages every 3 seconds
    _temporaryTimer->setSingleShot(true);
}

TaskStatusWidget::~TaskStatusWidget()
{
    // Widgets are owned by status bar, don't delete them
}

void TaskStatusWidget::addTask(const QString& taskName)
{
    TaskInfo info;
    info.name = taskName;
    info.message = QString("%1: Starting...").arg(taskName);
    _activeTasks.append(info);
    
    if (_activeTasks.size() == 1) {
        _spinnerTimer->start();
        _cycleTimer->start();
        _spinnerLabel->show();
        _messageLabel->show();
    }
    
    updateDisplay();
}

void TaskStatusWidget::updateTaskMessage(const QString& taskName, const QString& message)
{
    for (int i = 0; i < _activeTasks.size(); ++i) {
        if (_activeTasks[i].name == taskName) {
            _activeTasks[i].message = message;
            if (i == _currentTaskIndex) {
                updateDisplay();
            }
            break;
        }
    }
}

void TaskStatusWidget::removeTask(const QString& taskName)
{
    for (int i = 0; i < _activeTasks.size(); ++i) {
        if (_activeTasks[i].name == taskName) {
            _activeTasks.removeAt(i);
            if (_currentTaskIndex >= _activeTasks.size()) {
                _currentTaskIndex = 0;
            }
            break;
        }
    }
    
    if (_activeTasks.isEmpty()) {
        _spinnerTimer->stop();
        _cycleTimer->stop();
        _spinnerLabel->hide();
        _messageLabel->hide();
        _statusBar->showMessage("Ready");
    } else {
        updateDisplay();
    }
}

void TaskStatusWidget::showTemporaryMessage(const QString& message, int timeout)
{
    _temporaryMessage = message;
    _temporaryTimer->start(timeout);
    updateDisplay();
}

void TaskStatusWidget::updateSpinner()
{
    _spinnerFrame = (_spinnerFrame + 1) % _spinnerFrames.size();
    _spinnerLabel->setText(_spinnerFrames[_spinnerFrame]);
}

void TaskStatusWidget::cycleMessages()
{
    if (_activeTasks.size() > 1) {
        _currentTaskIndex = (_currentTaskIndex + 1) % _activeTasks.size();
        updateDisplay();
    }
}

void TaskStatusWidget::updateDisplay()
{
    if (!_temporaryMessage.isEmpty()) {
        _messageLabel->setText(_temporaryMessage);
        return;
    }
    
    if (_activeTasks.isEmpty()) {
        return;
    }
    
    QString message = _activeTasks[_currentTaskIndex].message;
    
    if (_activeTasks.size() > 1) {
        message += QString("  [%1/%2 tasks]").arg(_currentTaskIndex + 1).arg(_activeTasks.size());
    }
    
    _messageLabel->setText(message);
}

// ============================================================================
// BackgroundTaskManager Implementation
// ============================================================================

BackgroundTaskManager::BackgroundTaskManager(QObject* parent)
    : QObject(parent)
    , _maxConcurrentTasks(2)
    , _statusWidget(nullptr)
{
}

BackgroundTaskManager::~BackgroundTaskManager()
{
    QMutexLocker locker(&_mutex);
    
    while (!_pendingTasks.isEmpty()) {
        delete _pendingTasks.dequeue();
    }
    
    for (auto& entry : _activeTasks) {
        if (entry.thread && entry.thread->isRunning()) {
            entry.thread->quit();
            entry.thread->wait(5000);
        }
        delete entry.task;
        delete entry.thread;
    }
    _activeTasks.clear();
}

void BackgroundTaskManager::setStatusBar(QStatusBar* statusBar)
{
    _statusWidget = std::make_unique<TaskStatusWidget>(statusBar, this);
}

void BackgroundTaskManager::queueTask(BackgroundTask* task)
{
    QMutexLocker locker(&_mutex);
    _pendingTasks.enqueue(task);
    locker.unlock();
    
    QMetaObject::invokeMethod(this, "processNextTask", Qt::QueuedConnection);
}

bool BackgroundTaskManager::hasActiveTasks() const
{
    QMutexLocker locker(&_mutex);
    return !_activeTasks.isEmpty() || !_pendingTasks.isEmpty();
}

int BackgroundTaskManager::activeTaskCount() const
{
    QMutexLocker locker(&_mutex);
    return _activeTasks.size();
}

void BackgroundTaskManager::processNextTask()
{
    QMutexLocker locker(&_mutex);
    
    if (_activeTasks.size() >= _maxConcurrentTasks || _pendingTasks.isEmpty()) {
        return;
    }
    
    BackgroundTask* task = _pendingTasks.dequeue();
    QThread* thread = new QThread;
    
    TaskEntry entry{task, thread};
    _activeTasks.append(entry);
    
    task->moveToThread(thread);
    
    connect(thread, &QThread::started, task, &BackgroundTask::execute);
    connect(task, &BackgroundTask::progress, this, &BackgroundTaskManager::onTaskProgress);
    connect(task, &BackgroundTask::finished, this, &BackgroundTaskManager::onTaskFinished);
    
    QString taskName = task->taskName();
    
    locker.unlock();
    
    emit taskStarted(taskName);
    
    if (_statusWidget) {
        _statusWidget->addTask(taskName);
    }
    
    thread->start();
}

void BackgroundTaskManager::onTaskProgress(const QString& message)
{
    BackgroundTask* task = qobject_cast<BackgroundTask*>(sender());
    if (!task) return;
    
    if (_statusWidget) {
        _statusWidget->updateTaskMessage(task->taskName(), message);
    }
}

void BackgroundTaskManager::onTaskFinished(bool success, const QString& message)
{
    BackgroundTask* task = qobject_cast<BackgroundTask*>(sender());
    if (!task) return;
    
    QString taskName = task->taskName();
    
    QMutexLocker locker(&_mutex);
    
    for (int i = 0; i < _activeTasks.size(); ++i) {
        if (_activeTasks[i].task == task) {
            QThread* thread = _activeTasks[i].thread;
            _activeTasks.removeAt(i);
            
            thread->quit();
            thread->wait();
            thread->deleteLater();
            task->deleteLater();
            break;
        }
    }
    
    bool hasPending = !_pendingTasks.isEmpty();
    bool hasActive = !_activeTasks.isEmpty();
    
    locker.unlock();
    
    if (_statusWidget) {
        _statusWidget->removeTask(taskName);
        _statusWidget->showTemporaryMessage(message, 5000);
    }
    
    emit taskFinished(taskName, success);
    
    if (!hasActive && !hasPending) {
        emit allTasksComplete();
    } else {
        QMetaObject::invokeMethod(this, "processNextTask", Qt::QueuedConnection);
    }
}

// ============================================================================
// GaiaQueryTask Implementation
// ============================================================================
GaiaQueryTask::GaiaQueryTask(ImportStagingArea* staging,
                             const QString& projectId,
                             ApplicationController* controller,
                             QObject* parent)
    : BackgroundTask(parent)
    , _stagingArea(staging)
    , _projectId(projectId)
    , _controller(controller)
{
    if (_stagingArea)
        _stars = _stagingArea->allStars();
}

GaiaQueryTask::GaiaQueryTask(std::vector<std::shared_ptr<Star>> stars,
                             const QString& projectId,
                             ApplicationController* controller,
                             QObject* parent)
    : BackgroundTask(parent)
    , _stars(std::move(stars))
    , _stagingArea(nullptr)
    , _projectId(projectId)
    , _controller(controller)
{
}

bool GaiaQueryTask::starNeedsGaiaData(const std::shared_ptr<Star>& star) const
{
    if (star->getSourceId().isEmpty()) return false;

    return !Star::isSet(star->getRa()) ||
           !Star::isSet(star->getDec()) ||
           !Star::isSet(star->getPmra()) ||
           !Star::isSet(star->getPmdec()) ||
           !Star::isSet(star->getPlx()) ||
           !Star::isSet(star->getGmag()) ||
           !Star::isSet(star->getBp()) ||
           !Star::isSet(star->getRp()) ||
           !Star::isSet(star->getEPmra()) ||
           !Star::isSet(star->getEPmdec()) ||
           !Star::isSet(star->getEPlx());
}

QString GaiaQueryTask::buildADQLQuery()
{
    QStringList sourceIds;
    
    for (const auto& star : _stars) {
        if (!starNeedsGaiaData(star)) continue;
        
        QString sourceId = star->getSourceId().trimmed();
        
        if (sourceId.contains("DR3", Qt::CaseInsensitive)) {
            QRegularExpression re("(\\d{10,})");
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) {
                sourceId = match.captured(1);
            }
        }
        
        if (!sourceId.isEmpty()) {
            sourceIds << sourceId;
        }
    }
    
    if (sourceIds.isEmpty()) {
        return QString();
    }
    
    QString query = "SELECT Source, RA_ICRS, DE_ICRS, pmRA, pmDE, e_pmRA, e_pmDE, "
                   "Plx, e_Plx, Gmag, BPmag, RPmag, "
                   "pmRApmDEcor, PlxpmRAcor, PlxpmDEcor "
                   "FROM \"I/355/gaiadr3\" WHERE Source IN (";
    
    query += sourceIds.join(",");
    query += ")";
    
    return query;
}

void GaiaQueryTask::execute()
{
    LOG_SET_THREAD_NAME("GaiaQuery");
    LOG_INFO("Gaia", "Starting Gaia DR3 query task");

    _networkManager = new QNetworkAccessManager();

    emit progress("Gaia: Checking which stars need data...");

    int starsNeedingData = 0;
    for (const auto& star : _stars) {
        if (starNeedsGaiaData(star))
            starsNeedingData++;
    }

    if (starsNeedingData == 0) {
        emit finished(true, "Gaia: All stars already have complete astrometry data");
        _networkManager->deleteLater();
        return;
    }

    emit progress(QString("Gaia: Building query for %1 stars...").arg(starsNeedingData));

    QString adqlQuery = buildADQLQuery();
    if (adqlQuery.isEmpty()) {
        emit finished(true, "Gaia: No valid source IDs to query");
        _networkManager->deleteLater();
        return;
    }

    emit progress(QString("Gaia: Querying VizieR for %1 stars...").arg(starsNeedingData));

    QUrl url("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", "ASTRA/1.0");

    QUrlQuery postParams;
    postParams.addQueryItem("REQUEST", "doQuery");
    postParams.addQueryItem("LANG", "ADQL");
    postParams.addQueryItem("FORMAT", "csv");
    postParams.addQueryItem("QUERY", adqlQuery);

    QNetworkReply* reply = _networkManager->post(request, postParams.toString(QUrl::FullyEncoded).toUtf8());

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(300000);
    loop.exec();

    if (!timeoutTimer.isActive()) {
        reply->abort();
        emit finished(false, "Gaia: Query timed out");
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        emit finished(false, QString("Gaia: Network error - %1").arg(reply->errorString()));
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }

    emit progress("Gaia: Parsing response...");
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    auto modifiedStars = parseVizierResponse(response);

    if (_stagingArea) {
        for (const auto& star : modifiedStars)
            _stagingArea->markStarDirty(star->getId());
        LOG_INFO("Gaia", QString("Marked %1 stars dirty").arg(modifiedStars.size()));
        emit finished(true, QString("Gaia: Updated %1 stars (staged)").arg(modifiedStars.size()));
    } else {
        // Legacy: save directly
        if (!modifiedStars.empty() && _controller) {
            emit progress(QString("Gaia: Saving %1 updated stars...").arg(modifiedStars.size()));
            QMetaObject::invokeMethod(_controller, [this, modifiedStars]() {
                auto project = _controller->getCurrentProject();
                if (project)
                    _controller->saveStarsToProject(project, modifiedStars);
            }, Qt::QueuedConnection);
        }
        emit finished(true, QString("Gaia: Updated %1 stars").arg(modifiedStars.size()));
    }

    _networkManager->deleteLater();
}

std::vector<std::shared_ptr<Star>> GaiaQueryTask::parseVizierResponse(const QString& response)
{
    std::vector<std::shared_ptr<Star>> modifiedStars;

    QStringList lines = response.split('\n', Qt::SkipEmptyParts);

    if (lines.size() < 2) {
        LOG_WARNING("Gaia", "No matching data found in Gaia DR3");
        return modifiedStars;
    }

    QStringList headers = lines[0].split(',');
    QMap<QString, int> colIndex;
    for (int i = 0; i < headers.size(); ++i) {
        QString header = headers[i].trimmed().toLower().remove('"');
        colIndex[header] = i;
    }

    QMap<QString, QStringList> gaiaData;
    for (int i = 1; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        QStringList values = line.split(',');
        int sourceIdx = colIndex.value("source", -1);
        if (sourceIdx >= 0 && sourceIdx < values.size()) {
            QString sourceId = values[sourceIdx].trimmed().remove('"');
            gaiaData[sourceId] = values;
        }
    }

    for (auto& star : _stars) {
        QString sourceId = star->getSourceId().trimmed();
        if (sourceId.contains("DR3", Qt::CaseInsensitive)) {
            QRegularExpression re("(\\d{10,})");
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) sourceId = match.captured(1);
        }

        if (!gaiaData.contains(sourceId)) continue;

        const QStringList& values = gaiaData[sourceId];
        bool updated = false;

        auto getValue = [&](const QString& col) -> double {
            int idx = colIndex.value(col.toLower(), -1);
            if (idx >= 0 && idx < values.size()) {
                QString valStr = values[idx].trimmed().remove('"');
                if (valStr.isEmpty()) return 0.0;
                bool ok;
                double val = valStr.toDouble(&ok);
                return ok ? val : 0.0;
            }
            return 0.0;
        };

        if (!Star::isSet(star->getRa()))            { double v = getValue("ra_icrs");     if (v != 0.0) { star->setRa(v);            updated = true; } }
        if (!Star::isSet(star->getDec()))           { double v = getValue("de_icrs");     if (v != 0.0) { star->setDec(v);           updated = true; } }
        if (!Star::isSet(star->getPmra()))          { double v = getValue("pmra");        if (v != 0.0) { star->setPmra(v);          updated = true; } }
        if (!Star::isSet(star->getPmdec()))         { double v = getValue("pmde");        if (v != 0.0) { star->setPmdec(v);         updated = true; } }
        if (!Star::isSet(star->getEPmra()))         { double v = getValue("e_pmra");      if (v != 0.0) { star->setEPmra(v);         updated = true; } }
        if (!Star::isSet(star->getEPmdec()))        { double v = getValue("e_pmde");      if (v != 0.0) { star->setEPmdec(v);        updated = true; } }
        if (!Star::isSet(star->getPlx()))           { double v = getValue("plx");         if (v != 0.0) { star->setPlx(v);           updated = true; } }
        if (!Star::isSet(star->getEPlx()))          { double v = getValue("e_plx");       if (v != 0.0) { star->setEPlx(v);          updated = true; } }
        if (!Star::isSet(star->getGmag()))          { double v = getValue("gmag");        if (v != 0.0) { star->setGmag(v);          updated = true; } }
        if (!Star::isSet(star->getBp()))            { double v = getValue("bpmag");       if (v != 0.0) { star->setBp(v);            updated = true; } }
        if (!Star::isSet(star->getRp()))            { double v = getValue("rpmag");       if (v != 0.0) { star->setRp(v);            updated = true; } }
        if (!Star::isSet(star->getPmraPmdecCorr())) { double v = getValue("pmrapmdecor"); if (v != 0.0) { star->setPmraPmdecCorr(v); updated = true; } }
        if (!Star::isSet(star->getPlxPmraCorr()))   { double v = getValue("plxpmracor");  if (v != 0.0) { star->setPlxPmraCorr(v);  updated = true; } }
        if (!Star::isSet(star->getPlxPmdecCorr()))  { double v = getValue("plxpmdecor");  if (v != 0.0) { star->setPlxPmdecCorr(v); updated = true; } }

        if (!Star::isSet(star->getBpRp()) && Star::isSet(star->getBp()) && Star::isSet(star->getRp())) {
            star->setBpRp(star->getBp() - star->getRp());
            updated = true;
        }

        if (updated) {
            modifiedStars.push_back(star);
        }
    }

    LOG_INFO("Gaia", QString("Updated %1 stars from VizieR response").arg(modifiedStars.size()));
    // ── FIX: NO emit finished() here, NO direct DB save ──
    return modifiedStars;
}

// ============================================================================
// SimbadQueryTask Implementation
// ============================================================================

SimbadQueryTask::SimbadQueryTask(ImportStagingArea* staging,
                                 const QString& projectId,
                                 ApplicationController* controller,
                                 QObject* parent)
    : BackgroundTask(parent)
    , _stagingArea(staging)
    , _projectId(projectId)
    , _controller(controller)
{
    if (_stagingArea)
        _stars = _stagingArea->allStars();
}

SimbadQueryTask::SimbadQueryTask(std::vector<std::shared_ptr<Star>> stars,
                                 const QString& projectId,
                                 ApplicationController* controller,
                                 QObject* parent)
    : BackgroundTask(parent)
    , _stars(std::move(stars))
    , _stagingArea(nullptr)
    , _projectId(projectId)
    , _controller(controller)
{
}

QString SimbadQueryTask::generateSimbadScript()
{
    QString script = "format object f1 \"start %IDLIST(Gaia DR3)\\n\"+\n";
    script += "\"%BIBCODELIST\"\n";
    
    int validStars = 0;
    for (const auto& star : _stars) {
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            sourceId = sourceId.trimmed();
            if (!sourceId.contains("DR3", Qt::CaseInsensitive)) {
                sourceId = "Gaia DR3 " + sourceId;
            }
            script += QString("query id %1\n").arg(sourceId);
            validStars++;
        } else if (!star->getAlias().isEmpty()) {
            script += QString("query id %1\n").arg(star->getAlias());
            validStars++;
        }
    }
    
    return validStars > 0 ? script : QString();
}

QMap<QString, QStringList> SimbadQueryTask::parseSimbadResponse(const QString& response)
{
    QMap<QString, QStringList> bibcodesMap;
    QStringList lines = response.split('\n');
    
    QString currentStar;
    QStringList currentBibcodes;
    
    for (const QString& line : lines) {
        QString trimmedLine = line.trimmed();
        
        if (trimmedLine.startsWith("start ")) {
            if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
                bibcodesMap[currentStar] = currentBibcodes;
            }
            
            QString starId = trimmedLine.mid(6).trimmed();
            
            QRegularExpression re("(\\d{10,})");
            QRegularExpressionMatch match = re.match(starId);
            if (match.hasMatch()) {
                currentStar = match.captured(1);
            } else {
                currentStar = starId;
            }
            
            currentBibcodes.clear();
        } else if (!trimmedLine.isEmpty() && 
                   !trimmedLine.startsWith("::") && 
                   !trimmedLine.startsWith("#") &&
                   trimmedLine.length() >= 15 &&
                   !currentStar.isEmpty()) {
            QString yearStr = trimmedLine.left(4);
            bool yearOk;
            int year = yearStr.toInt(&yearOk);
            if (yearOk && year >= 1800 && year <= 2100) {
                currentBibcodes.append(trimmedLine);
            }
        }
    }
    
    if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
        bibcodesMap[currentStar] = currentBibcodes;
    }
    
    return bibcodesMap;
}

void SimbadQueryTask::execute()
{
    LOG_SET_THREAD_NAME("SimbadQuery");
    LOG_INFO("SIMBAD", "Starting SIMBAD bibliography query task");
    
    _networkManager = new QNetworkAccessManager();
    
    emit progress("SIMBAD: Generating query script...");
    
    QString script = generateSimbadScript();
    if (script.isEmpty()) {
        LOG_INFO("SIMBAD", "No stars with valid IDs to query");
        emit finished(true, "SIMBAD: No stars with valid IDs to query");
        _networkManager->deleteLater();
        return;
    }
    
    LOG_DEBUG("SIMBAD", QString("Generated script for %1 stars").arg(_stars.size()));
    
    emit progress(QString("SIMBAD: Querying bibliography for %1 stars...").arg(_stars.size()));
    
    QNetworkRequest request(QUrl("http://simbad.u-strasbg.fr/simbad/sim-script"));
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    
    QHttpPart scriptPart;
    scriptPart.setHeader(QNetworkRequest::ContentDispositionHeader, 
        QVariant("form-data; name=\"scriptFile\"; filename=\"script.txt\""));
    scriptPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain"));
    scriptPart.setBody(script.toUtf8());
    multiPart->append(scriptPart);
    
    LOG_DEBUG("SIMBAD", "Sending POST request to SIMBAD");
    
    QNetworkReply* reply = _networkManager->post(request, multiPart);
    multiPart->setParent(reply);
    
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start(300000);
    loop.exec();
    
    if (!timeoutTimer.isActive()) {
        reply->abort();
        LOG_ERROR("SIMBAD", "Query timed out after 5 minutes");
        emit finished(false, "SIMBAD: Query timed out");
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }
    
    if (reply->error() != QNetworkReply::NoError) {
        LOG_ERROR("SIMBAD", QString("Network error: %1").arg(reply->errorString()));
        emit finished(false, QString("SIMBAD: Network error - %1").arg(reply->errorString()));
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }
    
    emit progress("SIMBAD: Parsing response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    LOG_DEBUG("SIMBAD", QString("Received response: %1 bytes").arg(response.size()));
    
    QMap<QString, QStringList> bibcodes = parseSimbadResponse(response);
    
    LOG_DEBUG("SIMBAD", QString("Parsed bibliography for %1 stars").arg(bibcodes.size()));
    
    int updatedCount = 0;
    std::vector<std::shared_ptr<Star>> starsToSave;
    
    for (auto& star : _stars) {
        QString sourceId = star->getSourceId().trimmed();
        
        QRegularExpression re("(\\d{10,})");
        QRegularExpressionMatch match = re.match(sourceId);
        if (match.hasMatch()) {
            sourceId = match.captured(1);
        }
        
        if (bibcodes.contains(sourceId)) {
            const QStringList& starBibcodes = bibcodes[sourceId];
            bool updated = false;
            for (const QString& bibcode : starBibcodes) {
                if (!star->getBibcodes().empty()) {
                    bool found = false;
                    for (const auto& existing : star->getBibcodes()) {
                        if (existing == bibcode) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        star->addBibcode(bibcode);
                        updated = true;
                    }
                } else {
                    star->addBibcode(bibcode);
                    updated = true;
                }
            }
            if (updated) {
                updatedCount++;
                starsToSave.push_back(star);
            }
        }
    }
    
    if (_stagingArea) {
        for (const auto& star : starsToSave)
            _stagingArea->markStarDirty(star->getId());
        LOG_INFO("SIMBAD", QString("Marked %1 stars dirty").arg(starsToSave.size()));
    } else {
        // Legacy: save directly
        if (!starsToSave.empty() && _controller) {
            emit progress(QString("SIMBAD: Saving %1 updated stars...").arg(starsToSave.size()));
            QMetaObject::invokeMethod(_controller, [this, starsToSave]() {
                auto project = _controller->getCurrentProject();
                if (project)
                    _controller->saveStarsToProject(project, starsToSave);
            }, Qt::QueuedConnection);
        }
    }
    
    LOG_INFO("SIMBAD", QString("Task complete: Added bibcodes for %1 stars").arg(updatedCount));
    emit finished(true, QString("SIMBAD: Added bibliography codes for %1 stars").arg(updatedCount));
    _networkManager->deleteLater();
}

// ============================================================================
// SpectraImportTask Implementation
// ============================================================================

SpectraImportTask::SpectraImportTask(std::vector<SpectrumImportEntry> entries,
                                     const QString& projectId,
                                     ApplicationController* controller,
                                     QObject* parent)
    : BackgroundTask(parent)
    , _entries(std::move(entries))
    , _projectId(projectId)
    , _controller(controller)
{
}

void SpectraImportTask::execute()
{
    LOG_SET_THREAD_NAME("SpectraImport");
    LOG_INFO("SpectraImport", QString("Starting spectra import task with %1 entries").arg(_entries.size()));

    const int total = static_cast<int>(_entries.size());
    std::atomic<int> imported{0};
    std::atomic<int> failed{0};

    auto& registry = SpectrumReaderRegistry::instance();

    // ── Phase 1: Parallel spectrum reading (I/O bound) ──────────
    // (unchanged from original)

    struct ReadSlot {
        std::shared_ptr<Spectrum> spectrum;
        int entryIndex;
    };

    std::vector<int> workIndices;
    workIndices.reserve(total);
    for (int i = 0; i < total; ++i) {
        const auto& entry = _entries[i];
        if (!entry.matchedStar || !QFile::exists(entry.spectrumFile)) {
            failed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        workIndices.push_back(i);
    }

    const int workCount = static_cast<int>(workIndices.size());
    std::vector<std::shared_ptr<Spectrum>> readResults(total);

    emit progress(QString("Spectra Import: Reading %1 files...").arg(workCount));

    auto readFunc = [&](int idx) {
        const auto& entry = _entries[idx];

        auto reader = registry.getReaderForFile(entry.spectrumFile);
        if (!reader) {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::shared_ptr<SpectrumReader> localReader = reader;
        if (auto asciiReader = std::dynamic_pointer_cast<AsciiSpectrumReader>(reader)) {
            auto threadLocalReader = std::make_shared<AsciiSpectrumReader>();
            SpectrumMetadata meta;
            meta.filepath = entry.spectrumFile;
            if (entry.mjd.has_value()) meta.mjd = entry.mjd.value();
            if (entry.bjd.has_value()) meta.bjd = entry.bjd.value();
            if (entry.exposureTime.has_value()) meta.exposureTime = entry.exposureTime.value();
            if (entry.instrument.has_value()) meta.instrument = entry.instrument.value();
            threadLocalReader->setExternalMetadata(meta);
            localReader = threadLocalReader;
        }

        SpectrumReadResult readResult = localReader->readSpectrum(entry.spectrumFile);
        if (!readResult.success) {
            failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        readResult.spectrum->setBarycentricallyCorrected(entry.isBarycentricallyCorrected);

        if (entry.mjd.has_value() && readResult.spectrum->getMJD() == 0.0) {
            readResult.spectrum->setMJD(entry.mjd.value());
        }
        if (entry.bjd.has_value() && readResult.spectrum->getBJD() == 0.0) {
            readResult.spectrum->setBJD(entry.bjd.value());
        }
        if (entry.exposureTime.has_value() && readResult.spectrum->getExposureTime() == 0.0) {
            readResult.spectrum->setExposureTime(entry.exposureTime.value());
        }
        if (entry.instrument.has_value() && readResult.spectrum->getInstrument().isEmpty()) {
            readResult.spectrum->setInstrument(entry.instrument.value());
        }

        readResults[idx] = readResult.spectrum;
    };

    auto future = QtConcurrent::map(workIndices, readFunc);

    while (!future.isFinished()) {
        QThread::msleep(250);
        int readSoFar = 0;
        for (int idx : workIndices) {
            if (readResults[idx]) readSoFar++;
        }
        int pct = workCount > 0 ? (readSoFar * 100 / workCount) : 0;
        emit progress(QString("Spectra Import: Reading files %1%...").arg(pct));
    }
    future.waitForFinished();

    // ── Phase 2: In-memory linking + staging or DB save ─────────

    emit progress("Spectra Import: Processing results...");

    struct SaveEntry {
        std::shared_ptr<Star> star;
        std::shared_ptr<Spectrum> spectrum;
    };
    std::vector<SaveEntry> toSave;
    toSave.reserve(workCount);

    for (int idx : workIndices) {
        if (readResults[idx]) {
            const auto& entry = _entries[idx];
            // Always do in-memory linking
            entry.matchedStar->addSpectrum(readResults[idx]);
            toSave.push_back({entry.matchedStar, readResults[idx]});
        }
    }

    if (_stagingArea) {
        for (const auto& entry : toSave) {
            // Ensure spectrum has a UUID before tracking
            if (entry.spectrum->getId().isEmpty())
                entry.spectrum->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
            _stagingArea->markSpectrumNew(entry.spectrum->getId());
        }
    
        int totalImported = static_cast<int>(toSave.size());
        int totalFailed = total - totalImported;
    
        LOG_INFO("SpectraImport", QString("Staged %1 spectra (%2 failed)")
                 .arg(totalImported).arg(totalFailed));
    
        emit importComplete(totalImported, totalFailed);
        emit finished(true, QString("Spectra Import: %1 staged, %2 failed")
                      .arg(totalImported).arg(totalFailed));
    } else {
        // ── Legacy mode: direct DB writes (original behavior) ───
        emit progress("Spectra Import: Saving to database...");

        const int batchSize = 50;
        int saved = 0;

        for (size_t batchStart = 0; batchStart < toSave.size(); batchStart += batchSize) {
            size_t batchEnd = std::min(batchStart + batchSize, toSave.size());

            std::vector<SaveEntry> batch(toSave.begin() + batchStart,
                                          toSave.begin() + batchEnd);
            QString projectId = _projectId;

            QMetaObject::invokeMethod(_controller, [this, batch, projectId]() {
                for (const auto& entry : batch) {
                    _controller->saveSpectrumToProject(projectId, entry.star->getId(), entry.spectrum);
                }
            }, Qt::BlockingQueuedConnection);

            saved += static_cast<int>(batch.size());
            int pct = toSave.size() > 0 ? (saved * 100 / static_cast<int>(toSave.size())) : 100;
            emit progress(QString("Spectra Import: Saving %1/%2 (%3%)")
                         .arg(saved).arg(toSave.size()).arg(pct));
        }

        int totalImported = static_cast<int>(toSave.size());
        int totalFailed = total - totalImported;

        LOG_INFO("SpectraImport", QString("Import complete: %1 imported, %2 failed")
                 .arg(totalImported).arg(totalFailed));

        emit importComplete(totalImported, totalFailed);
        emit finished(true, QString("Spectra Import: %1 imported, %2 failed")
                      .arg(totalImported).arg(totalFailed));
    }
}




// ════════════════════════════════════════════════════════════════
// DiggaFitImportTask
// ════════════════════════════════════════════════════════════════

DiggaFitImportTask::DiggaFitImportTask(
    std::vector<DiggaFitImportEntry> entries,
    ApplicationController* controller,
    QObject* parent)
    : BackgroundTask(parent)
    , _entries(std::move(entries))
    , _controller(controller)
{}

void DiggaFitImportTask::execute()
{
    int total = static_cast<int>(_entries.size());
    int imported = 0;
    int failed = 0;

    emit progress(QString("Loading plotdata for %1 fits...").arg(total));

    // Phase 1: Read plotdata files (heavy I/O) — unchanged
    for (int i = 0; i < total; ++i) {
        auto& entry = _entries[i];
        if (!entry.plotdataPath.isEmpty()) {
            std::vector<double> wl, mf;
            if (SpectralFitImportPage::loadPlotdata(entry.plotdataPath, wl, mf)) {
                entry.fit->modelWavelengths = std::move(wl);
                entry.fit->modelFluxes      = std::move(mf);
            }
        }

        if (i % 500 == 0)
            emit progress(QString("Read plotdata %1/%2").arg(i).arg(total));
    }

    // Phase 2: In-memory linking + staging or DB save
    emit progress(QString("Processing %1 fits...").arg(total));

    if (_stagingArea) {
        for (int i = 0; i < total; ++i) {
            auto& entry = _entries[i];
            // Ensure fit has a UUID before tracking
            if (entry.fit->getId().isEmpty())
                entry.fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
            entry.spectrum->addSpectralFit(entry.fit);
            _stagingArea->markFitNew(entry.fit->getId());
            imported++;
    
            if (i % 500 == 0)
                emit progress(QString("Staged %1/%2 fits").arg(i).arg(total));
        }
    
        emit importComplete(imported, failed);
        emit finished(true, QString("Staged %1 fits (%2 failed)").arg(imported).arg(failed));
    } else {
        // ── Legacy mode: direct DB writes ───────────────────────
        emit progress(QString("Saving %1 fits to database...").arg(total));
        auto* dbManager = _controller->databaseManager();

        for (int i = 0; i < total; ++i) {
            auto& entry = _entries[i];
            entry.spectrum->addSpectralFit(entry.fit);

            if (dbManager->saveSpectralFit(entry.starId, entry.spectrumId, entry.fit))
                imported++;
            else
                failed++;

            if (i % 500 == 0)
                emit progress(QString("Saved %1/%2 fits").arg(i).arg(total));
        }

        emit importComplete(imported, failed);
        emit finished(failed == 0,
                      QString("Imported %1 fits (%2 failed)").arg(imported).arg(failed));
    }
}

// ============================================================================
// RVExtractionTask Implementation
// ============================================================================

RVExtractionTask::RVExtractionTask(
    std::vector<std::shared_ptr<Star>> stars,
    const QString& projectId,
    ApplicationController* controller,
    Mode mode,
    QObject* parent)
    : BackgroundTask(parent)
    , _stars(std::move(stars))
    , _projectId(projectId)
    , _controller(controller)
    , _mode(mode)
{
    switch (mode) {
        case FromFits:    _taskName = "RV Extraction (Fits)";    break;
        case FromFolders: _taskName = "RV Extraction (Folders)"; break;
        case FromTable:   _taskName = "RV Extraction (Table)";   break;
    }
}

RVExtractionTask* RVExtractionTask::createFromFits(
    std::vector<std::shared_ptr<Star>> stars,
    const QString& projectId,
    ApplicationController* controller,
    bool bestFitOnly,
    bool skipZeroRV,
    QObject* parent)
{
    auto* task = new RVExtractionTask(
        std::move(stars), projectId, controller, FromFits, parent);
    task->_bestFitOnly = bestFitOnly;
    task->_skipZeroRV = skipZeroRV;
    return task;
}

RVExtractionTask* RVExtractionTask::createFromFolders(
    std::vector<std::shared_ptr<Star>> stars,
    const QString& projectId,
    ApplicationController* controller,
    const FolderConfig& config,
    QObject* parent)
{
    auto* task = new RVExtractionTask(
        std::move(stars), projectId, controller, FromFolders, parent);
    task->_folderConfig = config;
    return task;
}

RVExtractionTask* RVExtractionTask::createFromTable(
    std::vector<std::shared_ptr<Star>> stars,
    const QString& projectId,
    ApplicationController* controller,
    const TableConfig& config,
    QObject* parent)
{
    auto* task = new RVExtractionTask(
        std::move(stars), projectId, controller, FromTable, parent);
    task->_tableConfig = config;
    return task;
}

void RVExtractionTask::buildStarLookupIndex()
{
    _sourceIdIndex.clear();
    _aliasIndex.clear();

    QRegularExpression numRe("(\\d{10,})");

    for (const auto& star : _stars) {
        QString sid = star->getSourceId();
        if (!sid.isEmpty()) {
            _sourceIdIndex[sid] = star;
            _sourceIdIndex[sid.trimmed()] = star;
            QRegularExpressionMatch m = numRe.match(sid);
            if (m.hasMatch())
                _sourceIdIndex[m.captured(1)] = star;
        }
        QString alias = star->getAlias();
        if (!alias.isEmpty()) {
            _aliasIndex[alias.trimmed().toLower()] = star;
            QString crushed = alias.toUpper().remove(' ').remove('-').remove('_');
            _aliasIndex[crushed.toLower()] = star;
        }
    }
}

std::shared_ptr<Star> RVExtractionTask::findStarByIdentifier(
    const QString& id, const QString& idType) const
{
    QString clean = id.trimmed();
    if (clean.isEmpty()) return nullptr;

    if (idType == "source_id" || idType == "Gaia Source ID") {
        auto it = _sourceIdIndex.find(clean);
        if (it != _sourceIdIndex.end()) return it.value();
        QRegularExpression numRe("(\\d{10,})");
        QRegularExpressionMatch m = numRe.match(clean);
        if (m.hasMatch()) {
            it = _sourceIdIndex.find(m.captured(1));
            if (it != _sourceIdIndex.end()) return it.value();
        }
    } else if (idType == "alias" || idType == "Alias/Name") {
        auto it = _aliasIndex.find(clean.toLower());
        if (it != _aliasIndex.end()) return it.value();
        QString crushed = clean.toUpper().remove(' ').remove('-').remove('_').toLower();
        it = _aliasIndex.find(crushed);
        if (it != _aliasIndex.end()) return it.value();
    } else if (idType == "ra_dec" || idType.startsWith("RA")) {
        QRegularExpression coordRe("([\\d.]+)[_+\\-\\s]([+\\-]?[\\d.]+)");
        QRegularExpressionMatch m = coordRe.match(clean);
        if (m.hasMatch()) {
            bool okRa, okDec;
            double ra  = m.captured(1).toDouble(&okRa);
            double dec = m.captured(2).toDouble(&okDec);
            if (okRa && okDec) {
                double bestDist = 5.0 / 3600.0;
                std::shared_ptr<Star> best;
                for (const auto& star : _stars) {
                    double dRa = (ra - star->getRa())
                                 * std::cos(star->getDec() * M_PI / 180.0);
                    double dDec = dec - star->getDec();
                    double dist = std::sqrt(dRa * dRa + dDec * dDec);
                    if (dist < bestDist) {
                        bestDist = dist;
                        best = star;
                    }
                }
                return best;
            }
        }
    }
    return nullptr;
}

void RVExtractionTask::execute()
{
    LOG_SET_THREAD_NAME("RVExtract");
    LOG_INFO("RVExtract", QString("Starting %1").arg(_taskName));

    buildStarLookupIndex();

    switch (_mode) {
        case FromFits:    executeFromFits();    break;
        case FromFolders: executeFromFolders(); break;
        case FromTable:   executeFromTable();   break;
    }
}

void RVExtractionTask::executeFromFits()
{
    emit progress("RV Extraction: Processing spectra and fits...");

    int totalPoints = 0;
    int starsWithRV = 0;
    int starsProcessed = 0;
    int total = static_cast<int>(_stars.size());

    int starsWithSpectra = 0, totalSpectra = 0, totalFits = 0;
    int fitsWithZeroRV = 0, fitsWithNonZeroRV = 0;
    int bestFitNull = 0, bestFitFound = 0;

    // Only use DB fallback in legacy (non-staging) mode
    DatabaseManager* dbm = _stagingArea ? nullptr : _controller->databaseManager();

    for (const auto& star : _stars) {
        if (++starsProcessed % 200 == 0) {
            emit progress(QString("RV Extraction: %1/%2 stars...")
                .arg(starsProcessed).arg(total));
        }

        auto spectra = star->getSpectra();

        // Legacy mode only: load from DB if not in memory
        if (spectra.empty() && dbm) {
            spectra = dbm->loadSpectra(star->getId());
            for (const auto& sp : spectra)
                star->addSpectrum(sp);
        }

        if (spectra.empty()) continue;

        starsWithSpectra++;
        totalSpectra += static_cast<int>(spectra.size());

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (const auto& spectrum : spectra) {
            auto fits = spectrum->getSpectralFits();

            // Legacy mode only: load fits from DB if not in memory
            if (fits.empty() && dbm) {
                fits = dbm->loadSpectralFits(spectrum->getId());
                for (const auto& f : fits)
                    spectrum->addSpectralFit(f);
            }

            totalFits += static_cast<int>(fits.size());

            if (_bestFitOnly) {
                std::shared_ptr<SpectralFit> best = spectrum->getBestFit();

                if (!best && !fits.empty()) {
                    double lowestErr = std::numeric_limits<double>::max();
                    for (const auto& f : fits) {
                        if (f->radialVelocityError >= 0 &&
                            f->radialVelocityError < lowestErr) {
                            lowestErr = f->radialVelocityError;
                            best = f;
                        }
                    }
                    if (!best) best = fits.front();
                }

                if (!best) { bestFitNull++; continue; }
                bestFitFound++;

                if (_skipZeroRV && std::abs(best->radialVelocity) < 1e-10) {
                    fitsWithZeroRV++;
                    continue;
                }
                fitsWithNonZeroRV++;

                auto rvPt = RadialVelocityPoint::createFromSpectralFit(best, spectrum);
                if (rvPt) {
                    rvPt->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
                    curve->addRVPoint(rvPt);
                }
            } else {
                for (const auto& fit : fits) {
                    if (std::abs(fit->radialVelocity) < 1e-10) {
                        fitsWithZeroRV++;
                        if (_skipZeroRV) continue;
                    }
                    fitsWithNonZeroRV++;

                    auto rvPt = RadialVelocityPoint::createFromSpectralFit(fit, spectrum);
                    if (rvPt) {
                        rvPt->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
                        curve->addRVPoint(rvPt);
                    }
                }
            }
        }

        if (curve->getNumPoints() > 0) {
            RVExtractionResult result;
            result.starId = star->getId();
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            starsWithRV++;
        }
    }

    LOG_INFO("RVExtract", QString("Diagnostics: %1 stars with spectra, "
        "%2 total spectra, %3 total fits")
        .arg(starsWithSpectra).arg(totalSpectra).arg(totalFits));
    LOG_INFO("RVExtract", QString("Fits: %1 zero RV, %2 non-zero RV, "
        "bestFitFound=%3, bestFitNull=%4")
        .arg(fitsWithZeroRV).arg(fitsWithNonZeroRV)
        .arg(bestFitFound).arg(bestFitNull));

    emit progress(QString("RV Extraction: Linking %1 curves...")
        .arg(_results.size()));

    saveResultsToDatabase();

    LOG_INFO("RVExtract", QString("Complete: %1 RV points for %2 stars")
        .arg(totalPoints).arg(starsWithRV));

    emit extractionComplete(starsWithRV, totalPoints);
    emit finished(true, QString("RV Extraction: %1 points for %2 stars")
        .arg(totalPoints).arg(starsWithRV));
}

void RVExtractionTask::executeFromFolders()
{
    const auto& cfg = _folderConfig;

    QDir root(cfg.rootPath);
    QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    if (subDirs.isEmpty()) {
        emit finished(true, "RV Extraction: No subdirectories found.");
        return;
    }

    int matched = 0, unmatched = 0, totalPoints = 0;
    int total = subDirs.size();

    for (int i = 0; i < total; ++i) {
        if (i % 50 == 0) {
            emit progress(QString("RV Extraction: Scanning folder %1/%2...")
                .arg(i).arg(total));
        }

        QString dirName = subDirs[i];
        QDir sd(root.filePath(dirName));

        auto star = findStarByIdentifier(dirName, cfg.namingType);
        if (!star) { unmatched++; continue; }

        QStringList files = sd.entryList(
            {"*.csv", "*.txt", "*.dat", "*.tsv"}, QDir::Files);
        if (files.isEmpty()) { unmatched++; continue; }

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (const QString& fileName : files) {
            QFile file(sd.filePath(fileName));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            QTextStream in(&file);
            QStringList lines;
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                lines << line;
            }
            file.close();
            if (lines.isEmpty()) continue;

            QChar fileDelim = cfg.delimiter;
            if (fileDelim == '\0') {
                // Inline auto-detect
                int commas = lines.first().count(',');
                int tabs   = lines.first().count('\t');
                int semis  = lines.first().count(';');
                fileDelim = ',';
                if (tabs > commas)  fileDelim = '\t';
                if (semis > commas && semis > tabs) fileDelim = ';';
            }

            int startRow = cfg.hasHeader ? 1 : 0;

            for (int r = startRow; r < lines.size(); ++r) {
                QStringList fields;
                if (fileDelim == ' ')
                    fields = lines[r].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                else
                    fields = lines[r].split(fileDelim);

                if (cfg.timeCol >= fields.size() || cfg.rvCol >= fields.size())
                    continue;

                bool okTime, okRV;
                double time = fields[cfg.timeCol].trimmed().toDouble(&okTime);
                double rv   = fields[cfg.rvCol].trimmed().toDouble(&okRV);
                if (!okTime || !okRV) continue;

                double rvErr = 0.0;
                if (cfg.rvErrCol >= 0 && cfg.rvErrCol < fields.size()) {
                    bool okErr;
                    rvErr = fields[cfg.rvErrCol].trimmed().toDouble(&okErr);
                    if (!okErr) rvErr = 0.0;
                }

                auto rvPt = std::make_shared<RadialVelocityPoint>();
                rvPt->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

                if (cfg.isBJD) {
                    rvPt->setBJD(time);
                } else {
                    rvPt->setMJD(time);
                }
                rvPt->setRV(rv);
                rvPt->setRVError(rvErr);
                rvPt->setSource(fileName);

                curve->addRVPoint(rvPt);
            }
        }

        if (curve->getNumPoints() > 0) {
            RVExtractionResult result;
            result.starId = star->getId();
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            matched++;
        } else {
            unmatched++;
        }
    }

    emit progress(QString("RV Extraction: Saving %1 curves...").arg(matched));
    saveResultsToDatabase();

    LOG_INFO("RVExtract", QString("Folders: %1 matched (%2 pts), %3 unmatched")
        .arg(matched).arg(totalPoints).arg(unmatched));

    emit extractionComplete(matched, totalPoints);
    emit finished(true, QString("RV Extraction: %1 stars (%2 points), %3 unmatched")
        .arg(matched).arg(totalPoints).arg(unmatched));
}

void RVExtractionTask::executeFromTable()
{
    const auto& cfg = _tableConfig;

    // Group rows by star identifier
    QHash<QString, std::vector<int>> groupedRows;
    for (int i = 0; i < static_cast<int>(cfg.rows.size()); ++i) {
        const QStringList& row = cfg.rows[i];
        if (cfg.idCol >= row.size()) continue;
        QString key = row[cfg.idCol].trimmed();
        if (!key.isEmpty())
            groupedRows[key].push_back(i);
    }

    int matched = 0, unmatched = 0, totalPoints = 0;
    int total = groupedRows.size();
    int processed = 0;

    for (auto it = groupedRows.cbegin(); it != groupedRows.cend(); ++it) {
        if (++processed % 200 == 0) {
            emit progress(QString("RV Extraction: %1/%2 identifiers...")
                .arg(processed).arg(total));
        }

        auto star = findStarByIdentifier(it.key(), cfg.idType);
        if (!star) { unmatched++; continue; }

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (int ri : it.value()) {
            const QStringList& row = cfg.rows[ri];
            if (cfg.timeCol >= row.size() || cfg.rvCol >= row.size())
                continue;

            bool okTime, okRV;
            double time = row[cfg.timeCol].toDouble(&okTime);
            double rv   = row[cfg.rvCol].toDouble(&okRV);
            if (!okTime || !okRV) continue;

            double rvErr = 0.0;
            if (cfg.rvErrCol >= 0 && cfg.rvErrCol < row.size()) {
                bool okErr;
                rvErr = row[cfg.rvErrCol].toDouble(&okErr);
                if (!okErr) rvErr = 0.0;
            }

            auto rvPt = std::make_shared<RadialVelocityPoint>();
            rvPt->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

            if (cfg.isBJD)
                rvPt->setBJD(time);
            else
                rvPt->setMJD(time);

            rvPt->setRV(rv);
            rvPt->setRVError(rvErr);
            rvPt->setSource("table_import");

            curve->addRVPoint(rvPt);
        }

        if (curve->getNumPoints() > 0) {
            RVExtractionResult result;
            result.starId = star->getId();
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            matched++;
        }
    }

    emit progress(QString("RV Extraction: Saving %1 curves...").arg(matched));
    saveResultsToDatabase();

    emit extractionComplete(matched, totalPoints);
    emit finished(true, QString("RV Extraction: %1 stars (%2 points), %3 unmatched")
        .arg(matched).arg(totalPoints).arg(unmatched));
}


void RVExtractionTask::saveResultsToDatabase()
{
    if (_results.empty()) return;

    if (_stagingArea) {
        QHash<QString, std::shared_ptr<Star>> canonicalStars;
        for (const auto& star : _stars)
            canonicalStars[star->getId()] = star;
    
        for (auto& result : _results) {
            auto canonical = canonicalStars.value(result.starId);
            if (!canonical) continue;
    
            auto& curve = result.curve;
            curve->setStarId(canonical->getId());
            curve->setLogP(curve->computeLogP());
    
            if (result.fit) {
                result.fit->setCurveId(curve->getId());
                result.fit->setBestFit(true);
                curve->addRVFit(result.fit);
            }
    
            canonical->setRVCurve(curve);
            canonical->updateRVMetricsFromCurve();
    
            _stagingArea->markRVCurveNew(curve->getId());
            _stagingArea->markStarDirty(canonical->getId());
        }
    
        LOG_INFO("RVExtract", QString("Staged %1 RV curves").arg(_results.size()));
        return;
    }


    // ── Legacy mode: direct DB writes (original behavior) ───────
    const int batchSize = 20;
    int totalResults = static_cast<int>(_results.size());

    for (int batchStart = 0; batchStart < totalResults; batchStart += batchSize) {
        int batchEnd = std::min(batchStart + batchSize, totalResults);
        int start = batchStart;
        int end = batchEnd;

        emit progress(QString("RV Extraction: Saving %1/%2...")
            .arg(batchEnd).arg(totalResults));

        QMetaObject::invokeMethod(_controller, [this, start, end]() {
            DatabaseManager* dbm = _controller->databaseManager();
            if (!dbm) return;

            auto project = _controller->getCurrentProject();
            if (!project) return;

            QHash<QString, std::shared_ptr<Star>> canonicalStars;
            for (const auto& star : project->getAllStars())
                canonicalStars[star->getId()] = star;

            for (int i = start; i < end; ++i) {
                auto& result = _results[i];

                auto canonical = canonicalStars.value(result.starId);
                if (!canonical) continue;

                auto& curve = result.curve;
                curve->setStarId(canonical->getId());

                if (!dbm->saveRadialVelocityCurve(curve, canonical->getId()))
                    continue;

                for (const auto& pt : curve->getRVPoints()) {
                    pt->setCurveId(curve->getId());
                    dbm->saveRadialVelocityPoint(pt, curve->getId());
                }

                if (result.fit) {
                    result.fit->setCurveId(curve->getId());
                    result.fit->setBestFit(true);
                    curve->addRVFit(result.fit);
                    dbm->saveRVFit(result.fit, curve->getId());
                }

                curve->setLogP(curve->computeLogP());
                canonical->setRVCurve(curve);
                canonical->updateRVMetricsFromCurve();
                dbm->saveStar(_projectId, canonical);
            }
        }, Qt::BlockingQueuedConnection);
    }

    LOG_INFO("RVExtract",
        QString("Saved %1 curves to database").arg(totalResults));
}