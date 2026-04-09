#include "CrossRefResolver.h"
#include "utils/Logger.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>

#include <algorithm>

static const char* CAT = "CrossRefResolver";

static QString cleanTitle(const QString& raw)
{
    QString s = raw;
    static QRegularExpression htmlTags("<[^>]*>");
    s.remove(htmlTags);
    s.replace(QChar('\n'), QChar(' '));
    s.replace(QChar('\r'), QChar(' '));
    static QRegularExpression multiSpace("\\s+");
    s.replace(multiSpace, " ");
    return s.trimmed();
}

CrossRefResolver::CrossRefResolver(const QString& dbPath, QObject* parent)
    : QObject(parent)
    , _nam(new QNetworkAccessManager(this))
    , _dbPath(dbPath)
    , _connectionName(QString("crossref_cache_%1").arg(quintptr(this), 0, 16))
{
    connect(_nam, &QNetworkAccessManager::finished,
            this, &CrossRefResolver::onNetworkReply);
    openCache();
}

CrossRefResolver::~CrossRefResolver()
{
    {
        QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(_connectionName);
}

bool CrossRefResolver::openCache()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", _connectionName);
    db.setDatabaseName(_dbPath);
    if (!db.open()) {
        LOG_ERROR(CAT, "Failed to open cache database: " + db.lastError().text());
        return false;
    }

    QSqlQuery q(db);
    q.exec(R"(
        CREATE TABLE IF NOT EXISTS bibcode_cache (
            bibcode TEXT PRIMARY KEY,
            title TEXT,
            abstract TEXT,
            authors TEXT,
            doi TEXT,
            fetched_at TEXT DEFAULT (datetime('now'))
        )
    )");

    return true;
}

BibcodeInfo CrossRefResolver::lookupCache(const QString& bibcode)
{
    BibcodeInfo info;
    info.bibcode = bibcode;

    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen()) return info;

    QSqlQuery q(db);
    q.prepare("SELECT title, abstract, authors, doi FROM bibcode_cache WHERE bibcode = :bib");
    q.bindValue(":bib", bibcode);
    if (q.exec() && q.next()) {
        info.title    = q.value(0).toString();
        info.abstract = q.value(1).toString();
        info.authors  = q.value(2).toString();
        info.doi      = q.value(3).toString();
    }
    return info;
}

void CrossRefResolver::storeInCache(const BibcodeInfo& info)
{
    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO bibcode_cache (bibcode, title, abstract, authors, doi)
        VALUES (:bib, :title, :abstract, :authors, :doi)
    )");
    q.bindValue(":bib", info.bibcode);
    q.bindValue(":title", info.title);
    q.bindValue(":abstract", info.abstract);
    q.bindValue(":authors", info.authors);
    q.bindValue(":doi", info.doi);

    if (!q.exec())
        LOG_WARNING(CAT, "Cache store failed for " + info.bibcode + ": " + q.lastError().text());
}

void CrossRefResolver::resolve(const QStringList& bibcodes)
{
    for (const QString& bib : bibcodes) {
        BibcodeInfo cached = lookupCache(bib);
        if (!cached.title.isEmpty()) {
            emit resolved(bib, cached);
            continue;
        }

        ParsedBibcode parsed = parseBibcode(bib);
        if (!parsed.valid) {
            LOG_DEBUG(CAT, "Could not parse bibcode: " + bib);
            emit fetchFailed(bib);
            continue;
        }

        QUrl url("https://api.crossref.org/works");
        QUrlQuery query;

        QString bibTerms;
        if (!parsed.volume.isEmpty()) bibTerms += parsed.volume;
        if (!parsed.page.isEmpty())   bibTerms += " " + parsed.page;
        bibTerms = bibTerms.trimmed();

        if (!bibTerms.isEmpty())
            query.addQueryItem("query.bibliographic", bibTerms);

        if (!parsed.journalName.isEmpty())
            query.addQueryItem("query.container-title", parsed.journalName);

        if (bibTerms.isEmpty() && parsed.journalName.isEmpty()) {
            emit fetchFailed(bib);
            continue;
        }

        query.addQueryItem("filter",
            QString("from-pub-date:%1,until-pub-date:%1").arg(parsed.year));
        query.addQueryItem("rows", "5");
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("User-Agent",
            "ASTRA/1.0 (mailto:astra-tool@astro.dev)");
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(15000);

        QNetworkReply* reply = _nam->get(request);
        _pendingRequests[reply] = bib;

        LOG_DEBUG(CAT, QString("Querying CrossRef for %1: %2").arg(bib, url.toString()));
    }
}

void CrossRefResolver::onNetworkReply(QNetworkReply* reply)
{
    reply->deleteLater();

    QString bibcode = _pendingRequests.take(reply);
    if (bibcode.isEmpty()) return;

    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(CAT, QString("CrossRef request failed for %1: %2")
            .arg(bibcode, reply->errorString()));
        emit fetchFailed(bibcode);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        LOG_WARNING(CAT, "Invalid JSON response for " + bibcode);
        emit fetchFailed(bibcode);
        return;
    }

    QJsonArray items = doc.object()["message"].toObject()["items"].toArray();
    if (items.isEmpty()) {
        LOG_DEBUG(CAT, "No CrossRef results for " + bibcode);
        emit fetchFailed(bibcode);
        return;
    }

    ParsedBibcode parsed = parseBibcode(bibcode);
    QChar authorInitial = (bibcode.length() == 19) ? bibcode.at(18) : QChar();

    int bestScore = -1;
    QJsonObject bestItem;

    for (const auto& itemVal : items) {
        QJsonObject item = itemVal.toObject();
        int score = 0;

        QString itemVol = item["volume"].toString().trimmed();
        if (!parsed.volume.isEmpty() && itemVol == parsed.volume)
            score += 10;

        QString itemPage   = item["page"].toString().split("-").first().trimmed();
        QString itemArtNum = item["article-number"].toString().trimmed();
        bool pageMatch = false;
        if (!parsed.page.isEmpty()) {
            if (itemPage == parsed.page || itemArtNum == parsed.page)
                pageMatch = true;
        }
        if (pageMatch)
            score += 10;

        QJsonObject pub = item["published-print"].toObject();
        if (pub.isEmpty()) pub = item["published-online"].toObject();
        if (pub.isEmpty()) pub = item["created"].toObject();
        QJsonArray dp = pub["date-parts"].toArray();
        if (!dp.isEmpty() && !dp[0].toArray().isEmpty()) {
            if (dp[0].toArray()[0].toInt() == parsed.year)
                score += 5;
        }

        if (!authorInitial.isNull()) {
            QJsonArray authors = item["author"].toArray();
            if (!authors.isEmpty()) {
                QString family = authors[0].toObject()["family"].toString();
                if (!family.isEmpty() &&
                    family[0].toUpper() == authorInitial.toUpper())
                    score += 3;
            }
        }

        if (score > bestScore) {
            bestScore = score;
            bestItem = item;
        }
    }

    // Strict sanity check: volume AND page must match exactly
    {
        QString matchVol    = bestItem["volume"].toString().trimmed();
        QString matchPage   = bestItem["page"].toString().split("-").first().trimmed();
        QString matchArtNum = bestItem["article-number"].toString().trimmed();

        bool volOk  = parsed.volume.isEmpty() || matchVol == parsed.volume;
        bool pageOk = parsed.page.isEmpty()
                      || matchPage == parsed.page
                      || matchArtNum == parsed.page;

        if (!volOk || !pageOk) {
            LOG_DEBUG(CAT, QString("Rejecting CrossRef match for %1: "
                "expected vol=%2 page=%3, got vol=%4 page=%5 art=%6")
                .arg(bibcode, parsed.volume, parsed.page,
                     matchVol, matchPage, matchArtNum));
            emit fetchFailed(bibcode);
            return;
        }
    }

    BibcodeInfo info;
    info.bibcode = bibcode;

    // Join all title parts — CrossRef can split across array elements
    QJsonArray titles    = bestItem["title"].toArray();
    QJsonArray subtitles = bestItem["subtitle"].toArray();
    QStringList titleParts;
    for (const auto& t : titles)
        titleParts << t.toString();
    info.title = titleParts.join(". ");

    for (const auto& s : subtitles) {
        QString sub = s.toString().trimmed();
        if (!sub.isEmpty() && !info.title.isEmpty()) {
            if (!info.title.endsWith('.') && !info.title.endsWith('?') &&
                !info.title.endsWith('!') && !info.title.endsWith(':'))
                info.title += ".";
            info.title += " " + sub;
        }
    }
    info.title = cleanTitle(info.title);

    info.abstract = cleanTitle(bestItem["abstract"].toString());

    QJsonArray authors = bestItem["author"].toArray();
    QStringList authorNames;
    int maxA = std::min(5, static_cast<int>(authors.size()));
    for (int i = 0; i < maxA; ++i) {
        QJsonObject a = authors[i].toObject();
        QString family = a["family"].toString();
        QString given  = a["given"].toString();
        if (!family.isEmpty() && !given.isEmpty())
            authorNames << family + ", " + given;
        else if (!family.isEmpty())
            authorNames << family;
    }
    if (authors.size() > 5)
        authorNames << "et al.";
    info.authors = authorNames.join("; ");

    info.doi = bestItem["DOI"].toString();

    if (!info.title.isEmpty()) {
        storeInCache(info);
        emit resolved(bibcode, info);
        LOG_INFO(CAT, QString("Resolved %1 → \"%2\"").arg(bibcode, info.title));
    } else {
        emit fetchFailed(bibcode);
    }
}

CrossRefResolver::ParsedBibcode CrossRefResolver::parseBibcode(const QString& bib) const
{
    ParsedBibcode result;

    QString trimmed = bib.trimmed();
    if (trimmed.length() != 19)
        return result;

    bool ok;
    result.year = trimmed.mid(0, 4).toInt(&ok);
    if (!ok || result.year < 1800 || result.year > 2100)
        return result;

    result.journalAbbrev = trimmed.mid(4, 5);
    result.journalName   = journalToSearchName(result.journalAbbrev);

    result.volume = trimmed.mid(9, 4);
    result.volume.remove('.');
    result.volume = result.volume.trimmed();

    QChar section = trimmed.at(13);
    QString pageRaw = trimmed.mid(14, 4);
    pageRaw.remove('.');
    pageRaw = pageRaw.trimmed();

    if (section.isLetter())
        result.page = QString(section) + pageRaw;
    else
        result.page = pageRaw;

    if (result.journalName.isEmpty()) {
        QString cleaned = result.journalAbbrev;
        cleaned.remove('.');
        result.journalName = cleaned.trimmed();
    }

    result.valid = !result.volume.isEmpty() || !result.page.isEmpty();
    return result;
}

QString CrossRefResolver::journalToSearchName(const QString& abbrev) const
{
    static const QMap<QString, QString> journalMap = {
        {"A&A..",  "Astronomy and Astrophysics"},
        {"A&AS.",  "Astronomy and Astrophysics Supplement"},
        {"A&ARv",  "Astronomy and Astrophysics Review"},
        {"AJ...",  "Astronomical Journal"},
        {"ApJ..",  "Astrophysical Journal"},
        {"ApJS.",  "Astrophysical Journal Supplement"},
        {"ApJL.",  "Astrophysical Journal Letters"},
        {"MNRAS",  "Monthly Notices Royal Astronomical Society"},
        {"PASP.",  "Publications Astronomical Society Pacific"},
        {"Natur",  "Nature"},
        {"NatAs",  "Nature Astronomy"},
        {"Sci..",  "Science"},
        {"ARA&A",  "Annual Review Astronomy Astrophysics"},
        {"NewAR",  "New Astronomy Reviews"},
        {"NewA.",  "New Astronomy"},
        {"PASA.",  "Publications Astronomical Society Australia"},
        {"AN...",  "Astronomische Nachrichten"},
        {"Ap&SS",  "Astrophysics and Space Science"},
        {"AcA..",  "Acta Astronomica"},
        {"BaltA",  "Baltic Astronomy"},
        {"IBVS.",  "Information Bulletin Variable Stars"},
        {"RMxAA",  "Revista Mexicana Astronomia Astrofisica"},
        {"JAVSO",  "Journal American Association Variable Star Observers"},
        {"CoSka",  "Contributions Astronomical Observatory Skalnate"},
    };

    auto it = journalMap.find(abbrev);
    if (it != journalMap.end())
        return it.value();

    QString cleaned = abbrev;
    cleaned.remove('.');
    return cleaned.trimmed();
}