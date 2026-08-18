#include "UpdateManager.h"

#include "Logger.h"
#include "astra_version.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUrl>

#include <cstdio>

namespace {
// GitHub repository ASTRA releases live in.
constexpr const char* kRepoOwner = "Fabmat1";
constexpr const char* kRepoName  = "ASTRA";

// QSettings keys (shared with AppSettings' group/keys for the skipped version).
constexpr const char* kSettingsGroup = "AppSettings";
constexpr const char* kSkippedKey    = "update/skippedVersion";

// File suffix of the release asset this platform installs from.
#if defined(Q_OS_MACOS)
constexpr const char* kPackageSuffix = ".dmg";
#else
constexpr const char* kPackageSuffix = ".AppImage";
#endif

/// Architecture markers release assets are named with. Used to reject an asset
/// built for a different CPU (e.g. the arm64 .dmg on an Intel Mac).
const char* const kArchTokens[] = {"arm64", "aarch64", "x86_64", "amd64", "i386"};

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

/// The architecture tokens this machine accepts in an asset name.
QStringList hostArchTokens()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm64"))
        return {QStringLiteral("arm64"), QStringLiteral("aarch64")};
    if (arch == QLatin1String("x86_64"))
        return {QStringLiteral("x86_64"), QStringLiteral("amd64")};
    return {arch};
}

bool namesArch(const QString& assetName)
{
    for (const char* t : kArchTokens)
        if (assetName.contains(QLatin1String(t), Qt::CaseInsensitive))
            return true;
    return false;
}

#if defined(Q_OS_MACOS)
/// Run a command to completion; returns false on a non-zero exit / failure to
/// start. `err` receives stderr (or a description of the failure).
bool runTool(const QString& program, const QStringList& args, QString* err,
             int timeoutMs = 15 * 60 * 1000)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(30000)) {
        if (err) *err = QObject::tr("Could not run %1.").arg(program);
        return false;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(5000);
        if (err) *err = QObject::tr("%1 timed out.").arg(program);
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err) {
            const QString msg = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
            *err = msg.isEmpty()
                       ? QObject::tr("%1 failed (exit code %2).")
                             .arg(program).arg(p.exitCode())
                       : msg;
        }
        return false;
    }
    return true;
}

struct MacInstallOutcome {
    bool    ok            = false;
    bool    manualFallback = false;  ///< download is fine, we just can't install it
    QString error;
};

/// Mount `dmg`, copy the ASTRA.app it contains over `bundle`, unmount again.
/// Runs on a worker thread - no Qt GUI types here.
MacInstallOutcome performMacInstall(const QString& dmg, const QString& bundle)
{
    MacInstallOutcome out;

    const QFileInfo bundleInfo(bundle);
    const QString   parent = bundleInfo.absolutePath();

    // Gatekeeper runs quarantined apps from a read-only shadow copy; updating
    // that copy would be pointless even if it were writable.
    if (bundle.contains(QLatin1String("/AppTranslocation/"))) {
        out.manualFallback = true;
        out.error = QObject::tr(
            "ASTRA is running from a temporary read-only copy (macOS app "
            "translocation). Move ASTRA to your Applications folder and launch "
            "it from there.");
        return out;
    }

    if (!QFileInfo(parent).isWritable() || !bundleInfo.isWritable()) {
        out.manualFallback = true;
        out.error = QObject::tr(
            "%1 is not writable by this user.").arg(QDir::toNativeSeparators(bundle));
        return out;
    }

    const QString pid   = QString::number(QCoreApplication::applicationPid());
    const QString mount = QDir::temp().absoluteFilePath("astra-update-mnt-" + pid);
    QDir().mkpath(mount);

    QString err;
    if (!runTool(QStringLiteral("/usr/bin/hdiutil"),
                 {QStringLiteral("attach"), dmg,
                  QStringLiteral("-nobrowse"), QStringLiteral("-readonly"),
                  QStringLiteral("-noautoopen"), QStringLiteral("-noverify"),
                  QStringLiteral("-mountpoint"), mount},
                 &err)) {
        QDir().rmdir(mount);
        out.manualFallback = true;
        out.error = QObject::tr("Could not mount the disk image: %1").arg(err);
        return out;
    }

    // Everything past this point must unmount before returning.
    auto detach = [&mount] {
        QString ignored;
        if (!runTool(QStringLiteral("/usr/bin/hdiutil"),
                     {QStringLiteral("detach"), mount, QStringLiteral("-quiet")},
                     &ignored, 60000)) {
            runTool(QStringLiteral("/usr/bin/hdiutil"),
                    {QStringLiteral("detach"), mount, QStringLiteral("-force"),
                     QStringLiteral("-quiet")}, &ignored, 60000);
        }
        QDir().rmdir(mount);
    };

    // Locate the .app inside the image (named ASTRA.app, but don't insist).
    QString srcApp;
    const QFileInfoList entries = QDir(mount).entryInfoList(
        {QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        if (fi.fileName().compare(QStringLiteral("ASTRA.app"), Qt::CaseInsensitive) == 0) {
            srcApp = fi.absoluteFilePath();
            break;
        }
        if (srcApp.isEmpty())
            srcApp = fi.absoluteFilePath();
    }
    if (srcApp.isEmpty()) {
        detach();
        out.manualFallback = true;
        out.error = QObject::tr("The disk image does not contain an application bundle.");
        return out;
    }

    // Stage the new bundle next to the old one so the final swap is two renames
    // within one directory (same filesystem, no half-copied app on disk).
    const QString staged = parent + QStringLiteral("/.ASTRA-update-") + pid + QStringLiteral(".app");
    QDir(staged).removeRecursively();
    if (!runTool(QStringLiteral("/usr/bin/ditto"), {srcApp, staged}, &err)) {
        QDir(staged).removeRecursively();
        detach();
        out.manualFallback = true;
        out.error = QObject::tr("Could not copy the new version into place: %1").arg(err);
        return out;
    }
    detach();

    if (!QFileInfo::exists(staged + QStringLiteral("/Contents/MacOS"))) {
        QDir(staged).removeRecursively();
        out.error = QObject::tr("The downloaded application bundle is incomplete.");
        return out;
    }

    // Downloads that never touched a browser carry no quarantine flag, but the
    // .dmg may still propagate one - strip it so Gatekeeper doesn't block the
    // update the user just approved. Best effort; not fatal.
    QString ignored;
    runTool(QStringLiteral("/usr/bin/xattr"),
            {QStringLiteral("-dr"), QStringLiteral("com.apple.quarantine"), staged},
            &ignored, 60000);

    // Swap. The running process keeps its already-mapped executable alive even
    // after the old bundle is unlinked, so replacing ourselves is safe; the user
    // is asked to restart right afterwards.
    const QString backup = parent + QStringLiteral("/.ASTRA-old-") + pid + QStringLiteral(".app");
    QDir(backup).removeRecursively();
    const QByteArray cur = QFile::encodeName(bundle);
    const QByteArray bak = QFile::encodeName(backup);
    const QByteArray neu = QFile::encodeName(staged);

    if (std::rename(cur.constData(), bak.constData()) != 0) {
        QDir(staged).removeRecursively();
        out.manualFallback = true;
        out.error = QObject::tr("Could not move the current version aside.");
        return out;
    }
    if (std::rename(neu.constData(), cur.constData()) != 0) {
        std::rename(bak.constData(), cur.constData());  // roll back
        QDir(staged).removeRecursively();
        out.manualFallback = true;
        out.error = QObject::tr("Could not put the new version in place.");
        return out;
    }
    QDir(backup).removeRecursively();

    out.ok = true;
    return out;
}
#endif // Q_OS_MACOS
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

QString UpdateManager::appBundlePath()
{
#if defined(Q_OS_MACOS)
    // <bundle>/Contents/MacOS/ASTRA -> <bundle>
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName() != QLatin1String("MacOS") || !dir.cdUp())
        return {};
    if (dir.dirName() != QLatin1String("Contents") || !dir.cdUp())
        return {};
    const QString path = dir.absolutePath();
    if (!path.endsWith(QLatin1String(".app")))
        return {};
    return QFileInfo::exists(path + QStringLiteral("/Contents/Info.plist")) ? path
                                                                            : QString();
#else
    return {};
#endif
}

bool UpdateManager::isAppBundle()
{
    return !appBundlePath().isEmpty();
}

bool UpdateManager::canSelfInstall()
{
    return isAppImage() || isAppBundle();
}

QString UpdateManager::packagingName()
{
    if (isAppImage())
        return QStringLiteral("AppImage");
    if (isAppBundle())
        return QStringLiteral("macOS app bundle");
    return {};
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

    // Locate this platform's asset: the .AppImage on Linux, the .dmg on macOS,
    // and only when its name matches this machine's architecture (an arm64 .dmg
    // is useless on an Intel Mac). Assets without any arch marker are accepted
    // when they are the only candidate.
    {
        const QStringList archs = hostArchTokens();
        QJsonObject match;
        QJsonObject archless;
        int archlessCount = 0;
        QStringList assetNames;

        for (const QJsonValue& av : obj.value("assets").toArray()) {
            const QJsonObject a = av.toObject();
            const QString name  = a.value("name").toString();
            assetNames << name;
            if (!name.endsWith(QLatin1String(kPackageSuffix), Qt::CaseInsensitive))
                continue;
            if (!namesArch(name)) {
                archless = a;
                ++archlessCount;
                continue;
            }
            for (const QString& t : archs) {
                if (name.contains(t, Qt::CaseInsensitive)) {
                    match = a;
                    break;
                }
            }
            if (!match.isEmpty())
                break;
        }
        if (match.isEmpty() && archlessCount == 1)
            match = archless;

        if (!match.isEmpty()) {
            info.packageUrl = match.value("browser_download_url").toString();
            info.assetName  = match.value("name").toString();
            info.assetSize  = static_cast<qint64>(match.value("size").toDouble());

            // Checksums are published as "<asset>.sha256" next to the asset -
            // match by name so a release carrying several packages (AppImage +
            // dmg) can't hand us the wrong one.
            const QString shaName = info.assetName + QStringLiteral(".sha256");
            for (const QJsonValue& av : obj.value("assets").toArray()) {
                const QJsonObject a = av.toObject();
                if (a.value("name").toString().compare(shaName, Qt::CaseInsensitive) == 0) {
                    info.sha256Url = a.value("browser_download_url").toString();
                    break;
                }
            }
        } else if (canSelfInstall()) {
            LOG_INFO("UpdateManager",
                     QString("Release %1 has no %2 asset for %3 (assets: %4)")
                         .arg(info.tagName, QLatin1String(kPackageSuffix),
                              QSysInfo::currentCpuArchitecture(),
                              assetNames.join(", ")));
        }
    }

    if (info.tagName.isEmpty()) {
        emit checkFailed(tr("GitHub returned a release without a version tag."));
        return;
    }

    _latest = info;

    const QString current = currentVersion();

    // Development builds (git-<hash>) can't be compared against a release. The
    // silent startup check stays quiet for them; a manual check offers the
    // latest release so a dev build can move onto the release channel.
    if (!isReleaseBuild()) {
        if (respectSkip) {
            LOG_INFO("UpdateManager",
                     QString("Development build %1; latest release is %2")
                         .arg(current, info.tagName));
            emit upToDate(current);
        } else {
            LOG_INFO("UpdateManager",
                     QString("Development build %1; offering release %2")
                         .arg(current, info.tagName));
            emit updateAvailable(info);
        }
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
    if (!canSelfInstall()) {
        emit installFailed(tr("Automatic install is only available when running "
                              "from the official AppImage (Linux) or app bundle "
                              "(macOS)."));
        return;
    }
    if (!info.hasPackage()) {
        emit installFailed(tr("This release has no %1 asset for %2 to download.")
                               .arg(QLatin1String(kPackageSuffix),
                                    QSysInfo::currentCpuArchitecture()));
        return;
    }

    _dlInfo = info;

    // First fetch the .sha256 (small) so we can verify the streamed download.
    if (info.sha256Url.isEmpty()) {
        LOG_WARNING("UpdateManager",
                    "No sha256 asset for " + info.assetName + "; skipping checksum");
        startPackageDownload(QString());
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
        startPackageDownload(expected);
    });
}

void UpdateManager::startPackageDownload(const QString& expectedSha256)
{
    if (isAppImage()) {
        // Stream into a sibling temp file so the final rename stays on the same
        // filesystem (atomic) and we never half-overwrite the live AppImage.
        const QString current = appImagePath();
        if (current.isEmpty()) {
            emit installFailed(tr("Lost track of the running AppImage."));
            return;
        }
        _dlPath = current + QStringLiteral(".astra-update-%1")
                                .arg(QCoreApplication::applicationPid());
    } else {
        // macOS: the .dmg is mounted, not renamed into place, so a temp dir is
        // fine - and stays usable when we have to fall back to a manual drag.
        const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QString name = _dlInfo.assetName;
        if (name.isEmpty())
            name = QStringLiteral("astra-update.dmg");
        _dlPath = QDir(tmp).absoluteFilePath(name);
        QFile::remove(_dlPath);
    }

    _dlFile = new QFile(_dlPath, this);
    if (!_dlFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = _dlFile->errorString();
        cleanupDownload();
        emit installFailed(tr("Cannot write to %1: %2").arg(
            QFileInfo(_dlPath).absolutePath(), err));
        return;
    }

    _dlHash = new QCryptographicHash(QCryptographicHash::Sha256);

    QNetworkRequest req((QUrl(_dlInfo.packageUrl)));
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
            failInstall(tr("Checksum mismatch, the download may be corrupt.\n"
                           "expected %1\n     got %2").arg(expected, got));
            return;
        }

        finishInstall();
    });
}

void UpdateManager::finishInstall()
{
    emit installStarted();
    if (isAppImage())
        installAppImage();
    else
        installMacBundle();
}

void UpdateManager::installAppImage()
{
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
        failInstall(tr("Could not replace %1 with the downloaded update.").arg(dest));
        return;
    }

    _dlPath.clear();  // consumed by rename; don't clean it up
    const QString version = _dlInfo.version;
    cleanupDownload();
    clearSkipMarker();

    LOG_INFO("UpdateManager", "Installed update " + version + " at " + dest);
    emit installFinished(dest);
}

void UpdateManager::installMacBundle()
{
#if defined(Q_OS_MACOS)
    const QString dmg    = _dlPath;
    const QString bundle = appBundlePath();
    if (bundle.isEmpty()) {
        requireManualInstall(tr("ASTRA is not running from an application bundle."));
        return;
    }

    // Mounting the image and copying a full app bundle takes seconds; do it off
    // the GUI thread and report back through a queued call to the main thread.
    QPointer<UpdateManager> self(this);
    const QString version = _dlInfo.version;
    QThread* worker = QThread::create([self, dmg, bundle, version] {
        const MacInstallOutcome r = performMacInstall(dmg, bundle);
        QMetaObject::invokeMethod(qApp, [self, r, bundle, version, dmg] {
            if (!self) {          // dialog closed while we were installing
                QFile::remove(dmg);
                return;
            }
            if (r.ok) {
                self->cleanupDownload();   // drops the .dmg we just installed from
                self->clearSkipMarker();
                LOG_INFO("UpdateManager",
                         "Installed update " + version + " at " + bundle);
                emit self->installFinished(bundle);
            } else if (r.manualFallback) {
                self->requireManualInstall(r.error);
            } else {
                self->failInstall(r.error);
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
#else
    requireManualInstall(tr("Unsupported platform."));
#endif
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

void UpdateManager::requireManualInstall(const QString& reason)
{
    // The download itself is good - hand it to the desktop so the user can
    // install it by hand, and keep the file around for them.
    QString path = _dlPath;
    if (!path.isEmpty()) {
        const QString downloads =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (!downloads.isEmpty() && QDir().mkpath(downloads)) {
            const QString target = QDir(downloads).absoluteFilePath(
                QFileInfo(path).fileName());
            QFile::remove(target);
            if (QFile::rename(path, target) || QFile::copy(path, target)) {
                QFile::remove(path);
                path = target;
            }
        }
    }

    _dlPath.clear();  // keep the package; it's the whole point of this branch
    cleanupDownload();

    LOG_WARNING("UpdateManager",
                "Automatic install not possible (" + reason + "); kept " + path);

    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    emit manualInstallRequired(path, reason);
}

void UpdateManager::clearSkipMarker()
{
    // Clear any skip marker now that the user has installed an update.
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.remove(kSkippedKey);
    s.endGroup();
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
        QCoreApplication::quit();
        return;
    }

    const QString bundle = appBundlePath();
    if (!bundle.isEmpty()) {
        // `open -n` goes through LaunchServices, which picks up the freshly
        // swapped bundle instead of the (now unlinked) one we are running from.
        QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                {QStringLiteral("-n"), bundle});
    }
    QCoreApplication::quit();
}
