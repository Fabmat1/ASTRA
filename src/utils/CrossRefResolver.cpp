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
#include <QTimer>
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

    _pumpTimer = new QTimer(this);
    _pumpTimer->setSingleShot(true);
    connect(_pumpTimer, &QTimer::timeout, this, &CrossRefResolver::pumpQueue);

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

    q.exec(R"(
        CREATE TABLE IF NOT EXISTS failed_lookups (
            bibcode TEXT PRIMARY KEY,
            reason TEXT,
            failed_at TEXT DEFAULT (datetime('now'))
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

QMap<QString, BibcodeInfo>
CrossRefResolver::lookupCacheBatch(const QStringList &bibcodes) {
    QMap<QString, BibcodeInfo> out;
    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen() || bibcodes.isEmpty())
        return out;

    // Single SELECT … WHERE bibcode IN (?,?,…). SQLite's variable limit
    // (999) is far above any realistic per-star reference count.
    QStringList placeholders;
    placeholders.reserve(bibcodes.size());
    for (int i = 0; i < bibcodes.size(); ++i)
        placeholders << "?";

    QSqlQuery q(db);
    q.prepare("SELECT bibcode, title, abstract, authors, doi "
              "FROM bibcode_cache WHERE bibcode IN (" +
              placeholders.join(",") + ")");
    for (const QString &b : bibcodes)
        q.addBindValue(b);

    if (q.exec()) {
        while (q.next()) {
            BibcodeInfo info;
            info.bibcode  = q.value(0).toString();
            info.title    = q.value(1).toString();
            info.abstract = q.value(2).toString();
            info.authors  = q.value(3).toString();
            info.doi      = q.value(4).toString();
            out.insert(info.bibcode, info);
        }
    }
    return out;
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

        if (isKnownFailed(bib)) {
            emit fetchFailed(bib);
            continue;
        }

        if (_attempted.contains(bib)) {
            if (!_inProgress.contains(bib))
                emit fetchFailed(bib);
            continue;
        }

        ParsedBibcode parsed = parseBibcode(bib);
        if (!parsed.valid) {
            _attempted.insert(bib);
            markFailed(bib, "unparseable");
            emit fetchFailed(bib);
            continue;
        }

        _attempted.insert(bib);
        _inProgress.insert(bib);
        enqueue(bib);
    }

    pumpQueue();
}

void CrossRefResolver::enqueue(const QString& bibcode)
{
    _queue.enqueue(bibcode);
}

void CrossRefResolver::onNetworkReply(QNetworkReply* reply)
{
        if (_adsRequests.contains(reply)) {
        QString bib = _adsRequests.take(reply);
        reply->deleteLater();
        handleADSReply(reply, bib);
        return;   

    }

    reply->deleteLater();
    QString bibcode = _pendingRequests.take(reply);
    if (bibcode.isEmpty()) return;

    --_inflight;
    _inProgress.remove(bibcode);
    applyRateLimitHeaders(reply);
    QTimer::singleShot(0, this, &CrossRefResolver::pumpQueue);

    if (reply->error() != QNetworkReply::NoError) {
        // Transient — do NOT mark as permanently failed.
        LOG_WARNING(CAT, QString("CrossRef request failed for %1: %2")
            .arg(bibcode, reply->errorString()));
        emit fetchFailed(bibcode);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        // Treat as transient; server hiccup, malformed response, etc.
        LOG_WARNING(CAT, "Invalid JSON response for " + bibcode);
        emit fetchFailed(bibcode);
        return;
    }

    QJsonArray items = doc.object()["message"].toObject()["items"].toArray();
    if (items.isEmpty()) {
        LOG_DEBUG(CAT, "No CrossRef results for " + bibcode);
        markFailed(bibcode, "no-results");
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
            markFailed(bibcode, "wrong-match");
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
        markFailed(bibcode, "empty-title");
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


bool CrossRefResolver::isKnownFailed(const QString& bibcode)
{
    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM failed_lookups WHERE bibcode = :bib LIMIT 1");
    q.bindValue(":bib", bibcode);
    if (q.exec() && q.next())
        return true;
    return false;
}

void CrossRefResolver::markFailed(const QString& bibcode, const QString& reason)
{
    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen()) return;

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO failed_lookups (bibcode, reason, failed_at)
        VALUES (:bib, :reason, datetime('now'))
    )");
    q.bindValue(":bib", bibcode);
    q.bindValue(":reason", reason);

    if (!q.exec())
        LOG_WARNING(CAT, "Failed-lookup store failed for " + bibcode + ": " + q.lastError().text());
}

void CrossRefResolver::resolveViaADS(const QString& bibcode)
{
    BibcodeInfo cached = lookupCache(bibcode);
    if (!cached.title.isEmpty()) {
        emit resolved(bibcode, cached);
        return;
    }

    if (_inProgress.contains(bibcode)) {
        // CrossRef or a previous ADS click is already in flight — just wait.
        return;
    }

    _attempted.insert(bibcode);
    _inProgress.insert(bibcode);

    QUrl url(QString("https://ui.adsabs.harvard.edu/abs/%1/abstract").arg(bibcode));

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "text/html");
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = _nam->get(request);
    _adsRequests[reply] = bibcode;

    LOG_DEBUG(CAT, QString("Scraping ADS for %1: %2").arg(bibcode, url.toString()));
}

static QString unescapeHtml(QString s)
{
    s.replace("&amp;",  "&");
    s.replace("&lt;",   "<");
    s.replace("&gt;",   ">");
    s.replace("&quot;", "\"");
    s.replace("&#39;",  "'");
    s.replace("&apos;", "'");
    s.replace("&nbsp;", " ");
    // numeric entities (basic)
    static QRegularExpression numEnt("&#(\\d+);");
    auto it = numEnt.globalMatch(s);
    while (it.hasNext()) {
        auto m = it.next();
        QChar c(m.captured(1).toInt());
        s.replace(m.captured(0), QString(c));
    }
    return s;
}

// Pulls <meta name="X" content="Y"> values, attribute-order-agnostic.
static QStringList extractMetaByName(const QString& html, const QString& metaName)
{
    QStringList out;
    static QRegularExpression metaTag("<meta\\b[^>]*>",
                                      QRegularExpression::CaseInsensitiveOption);
    QRegularExpression nameRe(
        QString("name\\s*=\\s*[\"']%1[\"']").arg(QRegularExpression::escape(metaName)),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression contentRe("content\\s*=\\s*[\"']([^\"']*)[\"']",
                                        QRegularExpression::CaseInsensitiveOption);

    auto it = metaTag.globalMatch(html);
    while (it.hasNext()) {
        QString tag = it.next().captured(0);
        if (!nameRe.match(tag).hasMatch()) continue;
        auto cm = contentRe.match(tag);
        if (cm.hasMatch()) out << unescapeHtml(cm.captured(1));
    }
    return out;
}

BibcodeInfo CrossRefResolver::parseADSHtml(const QString& bibcode, const QString& html)
{
    BibcodeInfo info;
    info.bibcode = bibcode;

    // Title — Google Scholar-style meta tag, present on every ADS abstract page.
    auto titles = extractMetaByName(html, "citation_title");
    if (!titles.isEmpty())
        info.title = cleanTitle(titles.first());

    // Authors — one meta tag per author; ADS uses "Family, Given".
    auto authors = extractMetaByName(html, "citation_author");
    QStringList authorList;
    int maxA = std::min<int>(5, authors.size());
    for (int i = 0; i < maxA; ++i)
        authorList << authors[i].trimmed();
    if (authors.size() > 5)
        authorList << "et al.";
    info.authors = authorList.join("; ");

    // DOI
    auto dois = extractMetaByName(html, "citation_doi");
    if (!dois.isEmpty())
        info.doi = dois.first().trimmed();

    // Abstract — ADS embeds it as a JSON-escaped block. Try meta description
    // first (truncated, but always there), then a more generous regex.
    auto descs = extractMetaByName(html, "description");
    if (!descs.isEmpty())
        info.abstract = cleanTitle(descs.first());

    // Try to find the full abstract in the embedded React state.
    static QRegularExpression absRe(
        "\"abstract\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"",
        QRegularExpression::CaseInsensitiveOption);
    auto am = absRe.match(html);
    if (am.hasMatch()) {
        QString a = am.captured(1);
        a.replace("\\\"", "\"");
        a.replace("\\n", " ");
        a.replace("\\t", " ");
        a.replace("\\\\", "\\");
        a = cleanTitle(a);
        if (a.length() > info.abstract.length())   // prefer longer
            info.abstract = a;
    }

    return info;
}

void CrossRefResolver::clearFailed(const QString& bibcode)
{
    QSqlDatabase db = QSqlDatabase::database(_connectionName, false);
    if (!db.isOpen()) return;
    QSqlQuery q(db);
    q.prepare("DELETE FROM failed_lookups WHERE bibcode = :bib");
    q.bindValue(":bib", bibcode);
    q.exec();
}

void CrossRefResolver::handleADSReply(QNetworkReply* reply, const QString& bibcode)
{
    _inProgress.remove(bibcode);

    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(CAT, QString("ADS scrape failed for %1: %2")
            .arg(bibcode, reply->errorString()));
        emit fetchFailed(bibcode);
        return;
    }

    const QString html = QString::fromUtf8(reply->readAll());
    BibcodeInfo info = parseADSHtml(bibcode, html);

    if (info.title.isEmpty()) {
        LOG_WARNING(CAT, "ADS HTML had no parseable title for " + bibcode);
        emit fetchFailed(bibcode);
        return;
    }

    storeInCache(info);
    clearFailed(bibcode);   // it's no longer a failure
    emit resolved(bibcode, info);
    LOG_INFO(CAT, QString("Resolved %1 via ADS → \"%2\"").arg(bibcode, info.title));
}

void CrossRefResolver::pumpQueue()
{
    while (!_queue.isEmpty() && _inflight < _maxConcurrent) {
        const qint64 elapsed = _lastDispatch.isValid()
                               ? _lastDispatch.elapsed()
                               : _minIntervalMs;

        if (elapsed < _minIntervalMs) {
            const int waitMs = int(_minIntervalMs - elapsed);
            if (!_pumpTimer->isActive() || _pumpTimer->remainingTime() > waitMs)
                _pumpTimer->start(waitMs);
            return;
        }

        QString bib = _queue.dequeue();
        dispatchCrossRef(bib);
        _lastDispatch.restart();
    }
}

void CrossRefResolver::dispatchCrossRef(const QString& bibcode)
{
    ParsedBibcode parsed = parseBibcode(bibcode);  // re-parse; cheap

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
        markFailed(bibcode, "no-query-terms");
        _inProgress.remove(bibcode);
        emit fetchFailed(bibcode);
        return;
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
    _pendingRequests[reply] = bibcode;
    ++_inflight;

    LOG_DEBUG(CAT, QString("Querying CrossRef for %1 (queue=%2, inflight=%3): %4")
        .arg(bibcode).arg(_queue.size()).arg(_inflight).arg(url.toString()));
}

void CrossRefResolver::applyRateLimitHeaders(QNetworkReply* reply)
{
    // 429 → temporarily pause the queue.
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) {
        int retryS = reply->rawHeader("Retry-After").toInt();
        if (retryS <= 0) retryS = 5;
        LOG_WARNING(CAT, QString("CrossRef 429 — pausing queue %1s").arg(retryS));
        // Block the pump for that long by pretending we just dispatched.
        _lastDispatch.restart();
        _minIntervalMs = std::max(_minIntervalMs, retryS * 1000);
        QTimer::singleShot(retryS * 1000, this, [this]() {
            _minIntervalMs = 100;   // restore default
            pumpQueue();
        });
        return;
    }

    // Tune our pacing from the advertised limit if possible.
    QByteArray lim = reply->rawHeader("X-Rate-Limit-Limit");
    QByteArray iv  = reply->rawHeader("X-Rate-Limit-Interval");
    if (!lim.isEmpty() && !iv.isEmpty()) {
        bool ok1 = false, ok2 = false;
        int allowed = lim.toInt(&ok1);
        // Interval is "1s" / "1m" — strip suffix and convert.
        QString ivStr = QString::fromLatin1(iv).trimmed();
        int seconds = 1;
        if (ivStr.endsWith('s', Qt::CaseInsensitive))
            seconds = ivStr.left(ivStr.size()-1).toInt(&ok2);
        else if (ivStr.endsWith('m', Qt::CaseInsensitive))
            seconds = ivStr.left(ivStr.size()-1).toInt(&ok2) * 60;

        if (ok1 && ok2 && allowed > 0 && seconds > 0) {
            // Use half the allowed rate as a safety margin.
            int safeMs = (seconds * 1000) / std::max(1, allowed / 2);
            _minIntervalMs = std::clamp(safeMs, 50, 2000);
        }
    }
}

bool CrossRefResolver::isPending(const QString& bib) const
{ return _inProgress.contains(bib); }

bool CrossRefResolver::wasAttempted(const QString& bib) const
{ return _attempted.contains(bib); }