#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QQueue>
#include <QElapsedTimer>
#include <QSet>

class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

struct BibcodeInfo {
    QString bibcode;
    QString title;
    QString abstract;
    QString authors;
    QString doi;
};

class CrossRefResolver : public QObject
{
    Q_OBJECT

public:
    explicit CrossRefResolver(const QString& dbPath, QObject* parent = nullptr);
    ~CrossRefResolver() override;

    void resolve(const QStringList& bibcodes);
    void resolveViaADS(const QString& bibcode);
    BibcodeInfo lookupCache(const QString& bibcode);
    QMap<QString, BibcodeInfo> lookupCacheBatch(const QStringList &bibcodes);

    bool isPending(const QString& bibcode)     const;
    bool wasAttempted(const QString& bibcode)  const;
    bool isKnownFailed(const QString& bibcode); 

signals:
    void resolved(const QString& bibcode, const BibcodeInfo& info);
    void fetchFailed(const QString& bibcode);

private slots:
    void onNetworkReply(QNetworkReply* reply);

private:
    void enqueue(const QString& bibcode);
    void pumpQueue();
    void dispatchCrossRef(const QString& bibcode);

    void applyRateLimitHeaders(QNetworkReply* reply);
    void handleADSReply(QNetworkReply* reply, const QString& bibcode);
    static BibcodeInfo parseADSHtml(const QString& bibcode, const QString& html);

    struct ParsedBibcode {
        int year = 0;
        QString journalAbbrev;
        QString journalName;
        QString volume;
        QString page;
        bool valid = false;
    };

    ParsedBibcode parseBibcode(const QString& bibcode) const;
    QString journalToSearchName(const QString& abbrev) const;

    bool openCache();
    void storeInCache(const BibcodeInfo& info);

    void markFailed(const QString& bibcode, const QString& reason);
    void clearFailed(const QString& bibcode);

    QNetworkAccessManager* _nam = nullptr;
    QString _dbPath;
    QString _connectionName;
    QMap<QNetworkReply*, QString> _pendingRequests;
    QMap<QNetworkReply*, QString> _adsRequests; 

    QSet<QString> _attempted;  
    QSet<QString> _inProgress; 
    QQueue<QString>  _queue;
    int              _inflight       = 0;
    int              _maxConcurrent  = 4;   
    int              _minIntervalMs  = 100;     
    QElapsedTimer    _lastDispatch;
    QTimer*          _pumpTimer      = nullptr;
};