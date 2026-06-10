#include "UpdateManager.h"

#include "Logger.h"
#include "astra_version.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

#include <cstdio>

namespace {
// GitHub repository ASTRA releases live in.
constexpr const char* kRepoOwner = "Fabmat1";
constexpr const char* kRepoName  = "ASTRA";

// QSettings keys (shared with AppSettings' group/keys for the skipped version).
constexpr const char* kSettingsGroup = "AppSettings";
constexpr const char* kSkippedKey    = "update/skippedVersion";

/// Strip a leading 'v' and any pre-release/build suffix, returning the numeric
/// release components (major, minor, patch, …).
QVector<int> versionComponents(QString v)
{
    v = v.trimmed();
    if (v.startsWith('v') || v.startsWith('V'))
        v.remove(0, 1);
    // Drop pre-release / build metadata: 1.2.3-rc1, 1.2.3+meta.
    const int cut = v.indexOf(QRegularExpression("[-+]"));
    if (cut >= 0)
        v.truncate(cut);

    QVector<int> parts;
    const QStringList toks = v.split('.', Qt::SkipEmptyParts);
    for (const QString& t : toks) {
        bool ok = false;
        const int n = t.toInt(&ok);
        if (!ok)
            return {};  // not a clean numeric version
        parts.push_back(n);
    }
    return parts;
}
} // namespace

UpdateManager::UpdateManager(QObject* parent)
    : QObject(parent), _nam(new QNetworkAccessManager(this))
{
}

UpdateManager::~UpdateManager()
{
    cleanupDownload();
}

// ── Environment helpers ──────────────────────────────────────────────────

QString UpdateManager::currentVersion()
{
    return QString::fromLatin1(ASTRA_VERSION_STRING);
}

bool UpdateManager::isReleaseBuild()
{
    return !versionComponents(currentVersion()).isEmpty();
}

QString UpdateManager::appImagePath()
{
    const QString p = qEnvironmentVariable("APPIMAGE");
    if (p.isEmpty())
        return {};
    QFileInfo fi(p);
    return fi.exists() ? fi.absoluteFilePath() : QString();
}

bool UpdateManager::isAppImage()
{
    return !appImagePath().isEmpty();
}

int UpdateManager::compareVersions(const QString& a, const QString& b)
{
    const QVector<int> va = versionComponents(a);
    const QVector<int> vb = versionComponents(b);
    const int n = std::max(va.size(), vb.size());
    for (int i = 0; i < n; ++i) {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

// ── Update check ─────────────────────────────────────────────────────────

void UpdateManager::checkForUpdates(bool respectSkip)
{
    emit checkStarted();

    const QString url = QStringLiteral(
        "https://api.github.com/repos/%1/%2/releases/latest")
        .arg(kRepoOwner, kRepoName);

    QNetworkRequest req((QUrl(url)));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "ASTRA-UpdateManager");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, respectSkip] {
        onReleaseReply(reply, respectSkip);
    });
}

void UpdateManager::onReleaseReply(QNetworkReply* reply, bool respectSkip)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        LOG_WARNING("UpdateManager", "Release check failed: " + err);
        emit checkFailed(err);
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit checkFailed(tr("Could not parse the release information from GitHub."));
        return;
    }

    const QJsonObject obj = doc.object();

    UpdateInfo info;
    info.tagName      = obj.value("tag_name").toString();
    info.name         = obj.value("name").toString();
    info.releaseNotes = obj.value("body").toString();
    info.htmlUrl      = obj.value("html_url").toString();

    const QVector<int> comps = versionComponents(info.tagName);
    QStringList nums;
    for (int c : comps) nums << QString::number(c);
    info.version = nums.join('.');

    // Locate the AppImage asset (and its sha256 sibling) for x86_64.
    for (const QJsonValue& av : obj.value("assets").toArray()) {
        const QJsonObject a = av.toObject();
        const QString name  = a.value("name").toString();
        const QString dlUrl = a.value("browser_download_url").toString();
        if (name.endsWith(".AppImage", Qt::CaseInsensitive)) {
            info.appImageUrl = dlUrl;
            info.assetName   = name;
            info.assetSize   = static_cast<qint64>(a.value("size").toDouble());
        } else if (name.endsWith(".sha256", Qt::CaseInsensitive)) {
            info.sha256Url = dlUrl;
        }
    }

    if (info.tagName.isEmpty()) {
        emit checkFailed(tr("GitHub returned a release without a version tag."));
        return;
    }

    _latest = info;

    const QString current = currentVersion();

    // Development builds (git-<hash>) can't be compared meaningfully; report
    // up to date so the silent startup check never nags, while the Settings
    // page can still show the latest release via latestInfo().
    if (!isReleaseBuild()) {
        LOG_INFO("UpdateManager",
                 QString("Development build %1; latest release is %2")
                     .arg(current, info.tagName));
        emit upToDate(current);
        return;
    }

    if (compareVersions(info.version, current) <= 0) {
        LOG_INFO("UpdateManager", "ASTRA is up to date (" + current + ")");
        emit upToDate(current);
        return;
    }

    if (respectSkip) {
        QSettings s;
        s.beginGroup(kSettingsGroup);
        const QString skipped = s.value(kSkippedKey).toString();
        s.endGroup();
        if (!skipped.isEmpty() && compareVersions(skipped, info.version) >= 0) {
            LOG_INFO("UpdateManager",
                     "Update " + info.version + " available but skipped by user");
            emit upToDate(current);
            return;
        }
    }

    LOG_INFO("UpdateManager",
             QString("Update available: %1 -> %2").arg(current, info.version));
    emit updateAvailable(info);
}

// ── Download & install ───────────────────────────────────────────────────

void UpdateManager::downloadAndInstall(const UpdateInfo& info)
{
    if (_dlReply) {
        LOG_WARNING("UpdateManager", "Download already in progress");
        return;
    }
    if (!isAppImage()) {
        emit installFailed(tr("Automatic install is only available when running "
                              "from an AppImage."));
        return;
    }
    if (!info.hasAppImage()) {
        emit installFailed(tr("This release has no AppImage asset to download."));
        return;
    }

    _dlInfo = info;

    // First fetch the .sha256 (small) so we can verify the streamed download.
    if (info.sha256Url.isEmpty()) {
        LOG_WARNING("UpdateManager",
                    "No sha256 asset for " + info.version + "; skipping checksum");
        startAppImageDownload(QString());
        return;
    }

    QNetworkRequest req((QUrl(info.sha256Url)));
    req.setRawHeader("User-Agent", "ASTRA-UpdateManager");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        QString expected;
        if (reply->error() == QNetworkReply::NoError) {
            // Format: "<hex>  astra-x.y.z-x86_64.AppImage"
            const QString txt = QString::fromLatin1(reply->readAll()).trimmed();
            expected = txt.section(QRegularExpression("\\s+"), 0, 0).toLower();
        } else {
            LOG_WARNING("UpdateManager",
                        "Could not fetch sha256: " + reply->errorString());
        }
        startAppImageDownload(expected);
    });
}

void UpdateManager::startAppImageDownload(const QString& expectedSha256)
{
    const QString current = appImagePath();
    if (current.isEmpty()) {
        emit installFailed(tr("Lost track of the running AppImage."));
        return;
    }

    // Stream into a sibling temp file so the final rename stays on the same
    // filesystem (atomic) and we never half-overwrite the live AppImage.
    _dlPath = current + QStringLiteral(".astra-update-%1")
                            .arg(QCoreApplication::applicationPid());
    _dlFile = new QFile(_dlPath, this);
    if (!_dlFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = _dlFile->errorString();
        cleanupDownload();
        emit installFailed(tr("Cannot write to %1: %2").arg(
            QFileInfo(current).absolutePath(), err));
        return;
    }

    _dlHash = new QCryptographicHash(QCryptographicHash::Sha256);

    QNetworkRequest req((QUrl(_dlInfo.appImageUrl)));
    req.setRawHeader("User-Agent", "ASTRA-UpdateManager");
    req.setRawHeader("Accept", "application/octet-stream");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    _dlReply = _nam->get(req);

    connect(_dlReply, &QNetworkReply::downloadProgress, this,
            &UpdateManager::downloadProgress);

    connect(_dlReply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray chunk = _dlReply->readAll();
        _dlHash->addData(chunk);
        if (_dlFile->write(chunk) != chunk.size()) {
            const QString err = _dlFile->errorString();
            failInstall(tr("Write error while downloading: %1").arg(err));
        }
    });

    const QString expected = expectedSha256;
    connect(_dlReply, &QNetworkReply::finished, this, [this, expected] {
        if (!_dlReply)  // aborted
            return;

        QNetworkReply* reply = _dlReply;
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString netErr = reply->errorString();

        if (!ok) {
            failInstall(tr("Download failed: %1").arg(netErr));
            return;
        }

        _dlFile->flush();
        _dlFile->close();

        const QString got = QString::fromLatin1(_dlHash->result().toHex());
        if (!expected.isEmpty() && got != expected) {
            failInstall(tr("Checksum mismatch — the download may be corrupt.\n"
                           "expected %1\n     got %2").arg(expected, got));
            return;
        }

        const QString dest = appImagePath();

        // Make the new file executable (rwxr-xr-x).
        _dlFile->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner  | QFileDevice::ReadGroup |
                                QFileDevice::ExeGroup  | QFileDevice::ReadOther |
                                QFileDevice::ExeOther);

        // Atomic in-place replacement: rename() over an existing file replaces
        // it. The running process keeps its own (already-extracted) copy, so
        // swapping the on-disk file is safe; the next launch picks up the new
        // version under the same path the user invokes.
        const QByteArray src = QFile::encodeName(_dlPath);
        const QByteArray dst = QFile::encodeName(dest);
        if (std::rename(src.constData(), dst.constData()) != 0) {
            failInstall(tr("Could not replace %1 with the downloaded update.")
                            .arg(dest));
            return;
        }

        _dlPath.clear();  // consumed by rename; don't clean it up
        cleanupDownload();

        LOG_INFO("UpdateManager",
                 "Installed update " + _dlInfo.version + " at " + dest);

        // Clear any skip marker now that the user has installed an update.
        QSettings s;
        s.beginGroup(kSettingsGroup);
        s.remove(kSkippedKey);
        s.endGroup();

        emit installFinished(dest);
    });
}

void UpdateManager::cancelDownload()
{
    if (_dlReply) {
        QNetworkReply* r = _dlReply;
        _dlReply = nullptr;  // mark aborted before the finished handler runs
        r->abort();
        r->deleteLater();
    }
    cleanupDownload();
}

void UpdateManager::failInstall(const QString& error)
{
    LOG_WARNING("UpdateManager", "Install failed: " + error);
    cleanupDownload();
    emit installFailed(error);
}

void UpdateManager::cleanupDownload()
{
    if (_dlReply) {
        _dlReply->deleteLater();
        _dlReply = nullptr;
    }
    if (_dlFile) {
        if (_dlFile->isOpen())
            _dlFile->close();
        _dlFile->deleteLater();
        _dlFile = nullptr;
    }
    delete _dlHash;
    _dlHash = nullptr;

    if (!_dlPath.isEmpty()) {
        QFile::remove(_dlPath);
        _dlPath.clear();
    }
}

void UpdateManager::relaunch()
{
    const QString img = appImagePath();
    if (!img.isEmpty()) {
        // Re-run exactly the documented way so all bundled binaries resolve.
        QProcess::startDetached(img, {QStringLiteral("--appimage-extract-and-run")});
    }
    QCoreApplication::quit();
}
