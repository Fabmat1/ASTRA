#include "BackgroundTaskManager.h"
#include "controllers/ApplicationController.h"
#include "models/Star.h"
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

GaiaQueryTask::GaiaQueryTask(std::vector<std::shared_ptr<Star>> stars,
                             const QString& projectId,
                             ApplicationController* controller,
                             QObject* parent)
    : BackgroundTask(parent)
    , _stars(std::move(stars))
    , _projectId(projectId)
    , _controller(controller)
    , _networkManager(nullptr)
{
}

bool GaiaQueryTask::starNeedsGaiaData(const std::shared_ptr<Star>& star) const
{
    if (star->getSourceId().isEmpty()) {
        return false;
    }
    
    return star->getRa() == 0.0 ||
           star->getDec() == 0.0 ||
           star->getPmra() == 0.0 ||
           star->getPmdec() == 0.0 ||
           star->getPlx() == 0.0 ||
           star->getGmag() == 0.0 ||
           star->getBp() == 0.0 ||
           star->getRp() == 0.0 ||
           star->getEPmra() == 0.0 ||
           star->getEPmdec() == 0.0 ||
           star->getEPlx() == 0.0;
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
        if (starNeedsGaiaData(star)) {
            starsNeedingData++;
        }
    }
    
    LOG_DEBUG("Gaia", QString("Found %1 stars needing Gaia data out of %2 total")
              .arg(starsNeedingData).arg(_stars.size()));
    
    if (starsNeedingData == 0) {
        LOG_INFO("Gaia", "No stars need Gaia data - task complete");
        emit finished(true, "Gaia: All stars already have complete astrometry data");
        _networkManager->deleteLater();
        return;
    }
    
    emit progress(QString("Gaia: Building query for %1 stars...").arg(starsNeedingData));
    
    QString adqlQuery = buildADQLQuery();
    if (adqlQuery.isEmpty()) {
        LOG_WARNING("Gaia", "No valid source IDs found for query");
        emit finished(true, "Gaia: No valid source IDs to query");
        _networkManager->deleteLater();
        return;
    }
    
    LOG_DEBUG("Gaia", QString("ADQL query length: %1 characters").arg(adqlQuery.length()));
    
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
    
    QByteArray postData = postParams.toString(QUrl::FullyEncoded).toUtf8();
    
    LOG_DEBUG("Gaia", "Sending POST request to VizieR TAP");
    
    QNetworkReply* reply = _networkManager->post(request, postData);
    
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start(300000);
    loop.exec();
    
    if (!timeoutTimer.isActive()) {
        reply->abort();
        LOG_ERROR("Gaia", "Query timed out after 5 minutes");
        emit finished(false, "Gaia: Query timed out");
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }
    
    if (reply->error() != QNetworkReply::NoError) {
        LOG_ERROR("Gaia", QString("Network error: %1").arg(reply->errorString()));
        emit finished(false, QString("Gaia: Network error - %1").arg(reply->errorString()));
        reply->deleteLater();
        _networkManager->deleteLater();
        return;
    }
    
    emit progress("Gaia: Parsing response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    LOG_DEBUG("Gaia", QString("Received response: %1 bytes").arg(response.size()));
    
    parseVizierResponse(response);
    
    _networkManager->deleteLater();
}

void GaiaQueryTask::parseVizierResponse(const QString& response)
{
    QStringList lines = response.split('\n', Qt::SkipEmptyParts);
    
    LOG_DEBUG("Gaia", QString("Parsing %1 lines from response").arg(lines.size()));
    
    if (lines.size() < 2) {
        LOG_WARNING("Gaia", "No matching data found in Gaia DR3");
        emit finished(true, "Gaia: No matching data found in Gaia DR3");
        return;
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
    
    emit progress("Gaia: Updating star data...");
    
    int updatedCount = 0;
    std::vector<std::shared_ptr<Star>> starsToSave;
    
    for (auto& star : _stars) {
        QString sourceId = star->getSourceId().trimmed();
        
        if (sourceId.contains("DR3", Qt::CaseInsensitive)) {
            QRegularExpression re("(\\d{10,})");
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) {
                sourceId = match.captured(1);
            }
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
        
        if (star->getRa() == 0.0) {
            double val = getValue("ra_icrs");
            if (val != 0.0) { star->setRa(val); updated = true; }
        }
        
        if (star->getDec() == 0.0) {
            double val = getValue("de_icrs");
            if (val != 0.0) { star->setDec(val); updated = true; }
        }
        
        if (star->getPmra() == 0.0) {
            double val = getValue("pmra");
            if (val != 0.0) { star->setPmra(val); updated = true; }
        }
        
        if (star->getPmdec() == 0.0) {
            double val = getValue("pmde");
            if (val != 0.0) { star->setPmdec(val); updated = true; }
        }
        
        if (star->getEPmra() == 0.0) {
            double val = getValue("e_pmra");
            if (val != 0.0) { star->setEPmra(val); updated = true; }
        }
        
        if (star->getEPmdec() == 0.0) {
            double val = getValue("e_pmde");
            if (val != 0.0) { star->setEPmdec(val); updated = true; }
        }
        
        if (star->getPlx() == 0.0) {
            double val = getValue("plx");
            if (val != 0.0) { star->setPlx(val); updated = true; }
        }
        
        if (star->getEPlx() == 0.0) {
            double val = getValue("e_plx");
            if (val != 0.0) { star->setEPlx(val); updated = true; }
        }
        
        if (star->getGmag() == 0.0) {
            double val = getValue("gmag");
            if (val != 0.0) { star->setGmag(val); updated = true; }
        }
        
        if (star->getBp() == 0.0) {
            double val = getValue("bpmag");
            if (val != 0.0) { star->setBp(val); updated = true; }
        }
        
        if (star->getRp() == 0.0) {
            double val = getValue("rpmag");
            if (val != 0.0) { star->setRp(val); updated = true; }
        }
        
        if (star->getPmraPmdecCorr() == 0.0) {
            double val = getValue("pmrapmdecor");
            if (val != 0.0) { star->setPmraPmdecCorr(val); updated = true; }
        }
        
        if (star->getPlxPmraCorr() == 0.0) {
            double val = getValue("plxpmracor");
            if (val != 0.0) { star->setPlxPmraCorr(val); updated = true; }
        }
        
        if (star->getPlxPmdecCorr() == 0.0) {
            double val = getValue("plxpmdecor");
            if (val != 0.0) { star->setPlxPmdecCorr(val); updated = true; }
        }
        
        if (star->getBpRp() == 0.0 && star->getBp() != 0.0 && star->getRp() != 0.0) {
            star->setBpRp(star->getBp() - star->getRp());
            updated = true;
        }
        
        if (updated) {
            updatedCount++;
            starsToSave.push_back(star);
        }
    }
    
    if (!starsToSave.empty() && _controller) {
        LOG_INFO("Gaia", QString("Saving %1 updated stars to database").arg(starsToSave.size()));
        emit progress(QString("Gaia: Saving %1 updated stars...").arg(starsToSave.size()));
        QMetaObject::invokeMethod(_controller, [this, starsToSave]() {
            auto project = _controller->getCurrentProject();
            if (project) {
                _controller->saveStarsToProject(project, starsToSave);
            }
        }, Qt::QueuedConnection);
    }
    
    LOG_INFO("Gaia", QString("Task complete: Updated %1 stars").arg(updatedCount));
    emit finished(true, QString("Gaia: Updated %1 stars with astrometry data").arg(updatedCount));
}

// ============================================================================
// SimbadQueryTask Implementation
// ============================================================================

SimbadQueryTask::SimbadQueryTask(std::vector<std::shared_ptr<Star>> stars,
                                 const QString& projectId,
                                 ApplicationController* controller,
                                 QObject* parent)
    : BackgroundTask(parent)
    , _stars(std::move(stars))
    , _projectId(projectId)
    , _controller(controller)
    , _networkManager(nullptr)
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
    
    if (!starsToSave.empty() && _controller) {
        emit progress(QString("SIMBAD: Saving %1 updated stars...").arg(starsToSave.size()));
        QMetaObject::invokeMethod(_controller, [this, starsToSave]() {
            auto project = _controller->getCurrentProject();
            if (project) {
                _controller->saveStarsToProject(project, starsToSave);
            }
        }, Qt::QueuedConnection);
    }
    
    LOG_INFO("SIMBAD", QString("Task complete: Added bibcodes for %1 stars").arg(updatedCount));
    emit finished(true, QString("SIMBAD: Added bibliography codes for %1 stars").arg(updatedCount));
    _networkManager->deleteLater();
}