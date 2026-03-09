// src/utils/CatalogQueryWorkers.cpp

#include "CatalogQueryWorkers.h"
#include "models/Star.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTemporaryFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QEventLoop>
#include <QTimer>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QDebug>

#include <limits>

// ============================================================================
// SimbadWorker Implementation
// ============================================================================

SimbadWorker::SimbadWorker(const std::vector<std::shared_ptr<Star>>& stars, QObject* parent)
    : QObject(parent)
    , _stars(stars)
    , _networkManager(new QNetworkAccessManager(this))
{
}

void SimbadWorker::process()
{
    emit progress(0, _stars.size(), "Generating SIMBAD script...");
    
    QString script = generateSimbadScript();
    if (script.isEmpty()) {
        emit error("No stars with valid Gaia IDs to query");
        return;
    }
    
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        emit error("Failed to create temporary script file");
        return;
    }
    scriptFile.write(script.toUtf8());
    scriptFile.flush();
    QString scriptPath = scriptFile.fileName();
    scriptFile.close();
    
    emit progress(0, _stars.size(), "Sending query to SIMBAD...");
    
    QNetworkRequest request(QUrl("http://simbad.u-strasbg.fr/simbad/sim-script"));
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    
    QFile* file = new QFile(scriptPath);
    if (!file->open(QIODevice::ReadOnly)) {
        emit error("Failed to open script file");
        delete multiPart;
        return;
    }
    
    QHttpPart scriptPart;
    scriptPart.setHeader(QNetworkRequest::ContentDispositionHeader, 
        QVariant("form-data; name=\"scriptFile\"; filename=\"script.txt\""));
    scriptPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain"));
    scriptPart.setBody(script.toUtf8());
    multiPart->append(scriptPart);
    
    QNetworkReply* reply = _networkManager->post(request, multiPart);
    multiPart->setParent(reply);
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    
    loop.exec();
    
    if (reply->error() != QNetworkReply::NoError) {
        emit error(QString("Network error: %1").arg(reply->errorString()));
        reply->deleteLater();
        return;
    }
    
    emit progress(_stars.size() / 2, _stars.size(), "Parsing SIMBAD response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    QMap<QString, QStringList> bibcodes = parseSimbadResponse(response);
    
    emit progress(_stars.size(), _stars.size(), "Complete");
    emit finished(bibcodes);
}

QString SimbadWorker::generateSimbadScript()
{
    QString script = "format object f1 \"start %OBJECT\\n\"+\n";
    script += "\"%BIBCODELIST\"\n";
    
    int validStars = 0;
    for (const auto& star : _stars) {
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            script += QString("query id GAIA DR3 %1\n").arg(sourceId);
            validStars++;
        } else if (!star->getAlias().isEmpty()) {
            script += QString("query id %1\n").arg(star->getAlias());
            validStars++;
        }
    }
    
    return validStars > 0 ? script : QString();
}

QMap<QString, QStringList> SimbadWorker::parseSimbadResponse(const QString& response)
{
    QMap<QString, QStringList> bibcodesMap;
    QStringList lines = response.split('\n');
    
    int dataIndex = -1;
    
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains("::data::")) {
            dataIndex = i;
        }
    }
    
    if (dataIndex == -1) {
        for (const QString& line : lines) {
            if (line.trimmed().startsWith("start ")) {
                dataIndex = 0;
                break;
            }
        }
        
        if (dataIndex == -1) {
            emit error("Invalid SIMBAD response format - no data section found");
            return bibcodesMap;
        }
    }
    
    QString currentStar;
    QStringList currentBibcodes;
    
    for (const QString& line : lines) {
        QString trimmedLine = line.trimmed();
        
        if (trimmedLine.startsWith("start GAIA DR3 ")) {
            if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
                bibcodesMap[currentStar] = currentBibcodes;
            }
            
            currentStar = trimmedLine.mid(15).trimmed();
            if (currentStar.contains(':')) {
                currentStar = currentStar.left(currentStar.indexOf(':'));
            }
            currentBibcodes.clear();
        } else if (trimmedLine.startsWith("start ")) {
            if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
                bibcodesMap[currentStar] = currentBibcodes;
            }
            
            currentStar = trimmedLine.mid(6).trimmed();
            if (currentStar.contains(':')) {
                currentStar = currentStar.left(currentStar.indexOf(':'));
            }
            currentBibcodes.clear();
        } else if (!trimmedLine.isEmpty() && 
                   !trimmedLine.startsWith("::") && 
                   !trimmedLine.startsWith("#") &&
                   trimmedLine.length() > 10 &&
                   !currentStar.isEmpty()) {
            bool looksLikeBibcode = false;
            if (trimmedLine.length() >= 19) {
                QString yearStr = trimmedLine.left(4);
                bool yearOk;
                int year = yearStr.toInt(&yearOk);
                if (yearOk && year >= 1800 && year <= 2100) {
                    looksLikeBibcode = true;
                }
            }
            
            if (looksLikeBibcode || trimmedLine.contains("...")) {
                currentBibcodes.append(trimmedLine);
            }
        }
    }
    
    if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
        bibcodesMap[currentStar] = currentBibcodes;
    }
    
    return bibcodesMap;
}

// ============================================================================
// GaiaWorker Implementation
// ============================================================================

GaiaWorker::GaiaWorker(std::vector<std::shared_ptr<Star>>& stars, QObject* parent)
    : QObject(parent)
    , _stars(stars)
    , _networkManager(new QNetworkAccessManager(this))
{
}

bool GaiaWorker::starNeedsGaiaData(const std::shared_ptr<Star>& star) const
{
    return (star->getRa() == 0.0 && star->getDec() == 0.0) ||
           star->getPmra() == 0.0 ||
           star->getPmdec() == 0.0 ||
           star->getPlx() == 0.0 ||
           star->getGmag() == 0.0 ||
           star->getBp() == 0.0 ||
           star->getRp() == 0.0 ||
           star->getEPmra() == 0.0 ||
           star->getEPmdec() == 0.0 ||
           star->getEPlx() == 0.0 ||
           star->getPmraPmdecCorr() == 0.0 ||
           star->getPlxPmdecCorr() == 0.0 ||
           star->getPlxPmraCorr() == 0.0;
}

QString GaiaWorker::buildADQLQuery()
{
    QStringList sourceIds;
    
    for (const auto& star : _stars) {
        if (!starNeedsGaiaData(star)) continue;
        
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            sourceId = sourceId.trimmed();
            if (sourceId.contains("DR3")) {
                QRegularExpression re("\\d{10,}");
                QRegularExpressionMatch match = re.match(sourceId);
                if (match.hasMatch()) {
                    sourceId = match.captured(0);
                }
            }
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

void GaiaWorker::process()
{
    emit progress(0, 100, "Checking which stars need Gaia data...");
    
    int starsNeedingData = 0;
    for (const auto& star : _stars) {
        if (starNeedsGaiaData(star)) {
            starsNeedingData++;
        }
    }
    
    if (starsNeedingData == 0) {
        emit progress(100, 100, "All stars already have complete data");
        emit finished(0);
        return;
    }
    
    emit progress(5, 100, QString("Building query for %1 stars...").arg(starsNeedingData));
    
    QString adqlQuery = buildADQLQuery();
    if (adqlQuery.isEmpty()) {
        emit progress(100, 100, "No valid source IDs to query");
        emit finished(0);
        return;
    }
    
    qDebug() << "Gaia ADQL query:" << adqlQuery.left(500) << "...";
    
    emit progress(10, 100, "Sending query to VizieR TAP...");
    
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
    
    QNetworkReply* reply = _networkManager->post(request, postData);
    
    connect(reply, &QNetworkReply::downloadProgress, this, 
            [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = 10 + (received * 40 / total);
            emit progress(pct, 100, QString("Downloading... %1 KB").arg(received / 1024));
        }
    });
    
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start(300000);
    loop.exec();
    
    if (!timeoutTimer.isActive()) {
        reply->abort();
        emit error("Gaia query timed out after 5 minutes");
        reply->deleteLater();
        return;
    }
    
    if (reply->error() != QNetworkReply::NoError) {
        QString errorDetails = reply->errorString();
        QByteArray responseData = reply->readAll();
        if (!responseData.isEmpty()) {
            errorDetails += "\nResponse: " + QString::fromUtf8(responseData.left(500));
        }
        emit error(QString("VizieR error: %1").arg(errorDetails));
        reply->deleteLater();
        return;
    }
    
    emit progress(50, 100, "Parsing Gaia response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    qDebug() << "Gaia response size:" << response.size() << "bytes";
    
    if (response.contains("Error") || response.contains("error")) {
        qDebug() << "Gaia response (first 1000 chars):" << response.left(1000);
    }
    
    parseVizierResponse(response);
}

void GaiaWorker::parseVizierResponse(const QString& response)
{
    QStringList lines = response.split('\n', Qt::SkipEmptyParts);
    
    if (lines.size() < 2) {
        qDebug() << "Gaia response too short:" << response;
        emit progress(100, 100, "No Gaia data found");
        emit finished(0);
        return;
    }
    
    QStringList headers = lines[0].split(',');
    QMap<QString, int> colIndex;
    for (int i = 0; i < headers.size(); ++i) {
        QString header = headers[i].trimmed().toLower();
        header.remove('"');
        colIndex[header] = i;
    }
    
    qDebug() << "Gaia columns found:" << colIndex.keys();
    
    QMap<QString, QStringList> gaiaData;
    for (int i = 1; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        
        QStringList values = line.split(',');
        int sourceIdx = colIndex.value("source", -1);
        if (sourceIdx >= 0 && sourceIdx < values.size()) {
            QString sourceId = values[sourceIdx].trimmed();
            sourceId.remove('"');
            gaiaData[sourceId] = values;
        }
    }
    
    qDebug() << "Parsed" << gaiaData.size() << "Gaia records";
    
    emit progress(70, 100, "Updating star data...");
    
    int updatedCount = 0;
    
    for (auto& star : _stars) {
        QString sourceId = star->getSourceId();
        if (sourceId.isEmpty()) continue;
        
        sourceId = sourceId.trimmed();
        if (sourceId.contains("DR3")) {
            QRegularExpression re("\\d{10,}");
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) {
                sourceId = match.captured(0);
            }
        }
        
        if (!gaiaData.contains(sourceId)) {
            continue;
        }
        
        const QStringList& values = gaiaData[sourceId];
        bool updated = false;
        
        auto getValue = [&](const QString& col) -> double {
            int idx = colIndex.value(col.toLower(), -1);
            if (idx >= 0 && idx < values.size()) {
                QString valStr = values[idx].trimmed();
                valStr.remove('"');
                if (valStr.isEmpty()) return 0.0;
                bool ok;
                double val = valStr.toDouble(&ok);
                return ok ? val : 0.0;
            }
            return 0.0;
        };
        
        if (star->getRa() == 0.0 && star->getDec() == 0.0) {
            double ra = getValue("ra_icrs");
            double dec = getValue("de_icrs");
            if (ra != 0.0 || dec != 0.0) {
                star->setRa(ra);
                star->setDec(dec);
                updated = true;
            }
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
        
        if (star->getPlxPmdecCorr() == 0.0) {
            double val = getValue("plxpmdecor");
            if (val != 0.0) { star->setPlxPmdecCorr(val); updated = true; }
        }
        
        if (star->getPlxPmraCorr() == 0.0) {
            double val = getValue("plxpmracor");
            if (val != 0.0) { star->setPlxPmraCorr(val); updated = true; }
        }
        
        if (star->getBpRp() == 0.0 && star->getBp() != 0.0 && star->getRp() != 0.0) {
            star->setBpRp(star->getBp() - star->getRp());
            updated = true;
        }
        
        if (updated) updatedCount++;
    }
    
    emit progress(100, 100, QString("Updated %1 stars").arg(updatedCount));
    emit finished(updatedCount);
}