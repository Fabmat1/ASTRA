#pragma once

#include <QObject>
#include <QString>
#include <QMap>

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

signals:
    void resolved(const QString& bibcode, const BibcodeInfo& info);
    void fetchFailed(const QString& bibcode);

private slots:
    void onNetworkReply(QNetworkReply* reply);

private:
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

    bool isKnownFailed(const QString& bibcode);
    void markFailed(const QString& bibcode, const QString& reason);
    void clearFailed(const QString& bibcode);

    QNetworkAccessManager* _nam = nullptr;
    QString _dbPath;
    QString _connectionName;
    QMap<QNetworkReply*, QString> _pendingRequests;
    QMap<QNetworkReply*, QString> _adsRequests; 
};