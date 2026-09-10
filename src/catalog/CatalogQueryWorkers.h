// src/utils/CatalogQueryWorkers.h

#ifndef CATALOGQUERYWORKERS_H
#define CATALOGQUERYWORKERS_H

#include <QObject>
#include <QMap>
#include <QStringList>
#include <memory>
#include <vector>

class Star;
class QNetworkAccessManager;

class SimbadWorker : public QObject
{
    Q_OBJECT

public:
    SimbadWorker(const std::vector<std::shared_ptr<Star>>& stars, QObject* parent = nullptr);
    
public slots:
    void process();
    
signals:
    void progress(int current, int total, const QString& message);
    void finished(const QMap<QString, QStringList>& bibcodes);
    void error(const QString& message);
    
private:
    std::vector<std::shared_ptr<Star>> _stars;
    QNetworkAccessManager* _networkManager;
    
    QString generateSimbadScript();
    QMap<QString, QStringList> parseSimbadResponse(const QString& response);
};

class GaiaWorker : public QObject
{
    Q_OBJECT

public:
    GaiaWorker(std::vector<std::shared_ptr<Star>>& stars, QObject* parent = nullptr);
    
public slots:
    void process();
    
signals:
    void progress(int current, int total, const QString& message);
    void finished(int updatedCount);
    void error(const QString& message);
    
private:
    std::vector<std::shared_ptr<Star>>& _stars;
    QNetworkAccessManager* _networkManager;
    
    QString buildADQLQuery();
    void parseVizierResponse(const QString& response);
    bool starNeedsGaiaData(const std::shared_ptr<Star>& star) const;
};

#endif // CATALOGQUERYWORKERS_H