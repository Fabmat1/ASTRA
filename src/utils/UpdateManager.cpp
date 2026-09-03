#include "UpdateManager.h"

#include "Logger.h"
#include "astra_version.h"

#include <algorithm>
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
#elif defined(Q_OS_WIN)
constexpr const char* kPackageSuffix = ".exe";
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

QString UpdateManager::windowsInstallRoot()
{
#if defined(Q_OS_WIN)
    // The installer lays the tree out as <root>/bin/ASTRA.exe and drops its
    // uninstaller at <root>/unins000.exe. That uninstaller is the marker: it
    // exists only for a tree Inno Setup created, which is exactly the tree the
    // next setup .exe knows how to replace. A build tree has none, and running
    // a release installer over one would be wrong.
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName().compare(QLatin1String("bin"), Qt::CaseInsensitive) != 0)
        return {};
    if (!dir.cdUp())
        return {};
    // unins000 unless another Inno-installed program shares the directory, in
    // which case the number goes up.
    if (dir.entryList({QStringLiteral("unins*.exe")}, QDir::Files).isEmpty())
        return {};
    return dir.absolutePath();
#else
    return {};
#endif
}

bool UpdateManager::isWindowsInstall()
{
    return !windowsInstallRoot().isEmpty();
}

bool UpdateManager::canSelfInstall()
{
    return isAppImage() || isAppBundle() || isWindowsInstall();
}

bool UpdateManager::installRunsAfterExit()
{
    // Windows holds an open image section on every running .exe and .dll, so
    // the installer cannot touch the tree until this process is gone.
    return isWindowsInstall();
}

QString UpdateManager::packagingName()
{
    if (isAppImage())
        return QStringLiteral("AppImage");
    if (isAppBundle())
        return QStringLiteral("macOS app bundle");
    if (isWindowsInstall())
        return QStringLiteral("Windows installation");
    return {};
}

UpdateManager::FinishedPrompt
UpdateManager::installFinishedPrompt(const QString& version)
{
    if (installRunsAfterExit())
        return {tr("Finish the update"),
                tr("ASTRA %1 has been downloaded and verified.\n\n"
                   "Windows cannot replace a program while it is running, so "
                   "ASTRA has to close for the installer to do its work. It "
                   "starts again on its own once the installer is done.\n\n"
                   "Close ASTRA and install now?").arg(version)};

    return {tr("Update installed"),
            tr("ASTRA %1 has been installed.\n\n"
               "Restart now to use the new version?").arg(version)};
}

QString UpdateManager::manualInstallHint()
{
#if defined(Q_OS_WIN)
    return tr("The installer has been opened. Run it to finish the update.");
#elif defined(Q_OS_MACOS)
    return tr("The disk image has been opened. Drag ASTRA to your Applications "
              "folder to finish, then restart ASTRA.");
#else
    return tr("The package has been opened. Install it to finish, then restart "
              "ASTRA.");
#endif
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
                              "from the official AppImage (Linux), app bundle "
                              "(macOS) or installed build (Windows)."));
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
        // macOS mounts the .dmg and Windows runs the setup .exe, so neither is
        // renamed into place: a temp dir is fine, and stays usable when we have
        // to fall back to installing by hand.
        const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QString name = _dlInfo.assetName;
        if (name.isEmpty())
            name = QStringLiteral("astra-update") + QLatin1String(kPackageSuffix);
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
    else if (isWindowsInstall())
        prepareWindowsSetup();
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

namespace {
#if defined(Q_OS_WIN)
// Path of a verified setup .exe waiting for this process to exit. relaunch() is
// static - it is the process saying goodbye, not the object - so the hand-over
// has to outlive the UpdateManager instance.
QString g_pendingWindowsSetup;
QString g_pendingWindowsRoot;

/// Write the hand-over script and start it. False when nothing was launched.
///
/// A script rather than the setup .exe directly, because two things have to
/// happen around an event this process cannot outlive: the installer must run
/// after we exit, and ASTRA must come back once it is done. cmd is the only
/// thing on a stock Windows that can sequence that.
bool launchWindowsInstaller(const QString& setup, const QString& root)
{
    const QString exe = QDir(root).absoluteFilePath(QStringLiteral("bin/ASTRA.exe"));
    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString script = QDir(tmp).absoluteFilePath(
        QStringLiteral("astra-update-%1.cmd").arg(QCoreApplication::applicationPid()));

    // cd out of the install tree first: the script inherits our working
    // directory, and a directory handle held there can stop the installer from
    // replacing what is in it.
    //
    // /SILENT rather than /VERYSILENT keeps Inno's own progress window, which
    // is the only thing on screen once ASTRA is gone. /CLOSEAPPLICATIONS covers
    // the seconds between this launch and our own exit. /DIR pins the target to
    // the tree we run from: the installer is PrivilegesRequired=lowest, so a
    // silent run left to guess would put a second copy under the user profile
    // instead of upgrading this one.
    const QString cmd = QStringLiteral(
        "@echo off\r\n"
        "cd /d \"%TEMP%\"\r\n"
        "\"%1\" /SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /CLOSEAPPLICATIONS /DIR=\"%2\"\r\n"
        "if exist \"%3\" start \"\" \"%3\"\r\n"
        "del \"%1\" >nul 2>&1\r\n"
        "del \"%~f0\"\r\n")
        .arg(QDir::toNativeSeparators(setup),
             QDir::toNativeSeparators(root),
             QDir::toNativeSeparators(exe));

    QFile f(script);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_WARNING("UpdateManager",
                    "Cannot write update script " + script + ": " + f.errorString());
        return false;
    }
    // cmd reads a batch file in the console code page, not UTF-8.
    f.write(cmd.toLocal8Bit());
    f.close();

    return QProcess::startDetached(
        QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QDir::toNativeSeparators(script)});
}
#endif // Q_OS_WIN
} // namespace

void UpdateManager::prepareWindowsSetup()
{
#if defined(Q_OS_WIN)
    const QString root = windowsInstallRoot();
    if (root.isEmpty()) {
        requireManualInstall(tr("ASTRA is not running from an installed copy."));
        return;
    }

    // The silent install writes straight into the tree we are running from, so
    // establish now that we are allowed to: a per-machine install under Program
    // Files needs administrator rights, and a silent run has no way to ask for
    // them - it would simply fail after ASTRA had already quit. Handing the
    // package to Explorer instead lets Windows raise the elevation prompt.
    QFile probe(QDir(root).absoluteFilePath(QStringLiteral(".astra-update-probe")));
    if (!probe.open(QIODevice::WriteOnly)) {
        requireManualInstall(tr("%1 cannot be written to by this user, so the "
                                "installer needs administrator rights.").arg(root));
        return;
    }
    probe.close();
    probe.remove();

    // Hold on to the package - relaunch() runs it once we are gone - and keep
    // cleanupDownload() from deleting it on the way out.
    g_pendingWindowsSetup = _dlPath;
    g_pendingWindowsRoot  = root;
    _dlPath.clear();

    const QString version = _dlInfo.version;
    cleanupDownload();
    clearSkipMarker();

    LOG_INFO("UpdateManager",
             "Update " + version + " verified and ready to install over " + root);
    emit installFinished(root);
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
        QCoreApplication::quit();
        return;
    }

#if defined(Q_OS_WIN)
    // Nothing has been installed yet on Windows: this is the hand-over. The
    // script waits for nobody - it is the installer's /CLOSEAPPLICATIONS and our
    // quit() below that get us out of the way - and starts ASTRA again at the
    // end, so there is no relaunch to do here.
    if (!g_pendingWindowsSetup.isEmpty()) {
        const QString setup = g_pendingWindowsSetup;
        const QString root  = g_pendingWindowsRoot;
        g_pendingWindowsSetup.clear();
        g_pendingWindowsRoot.clear();
        if (launchWindowsInstaller(setup, root)) {
            LOG_INFO("UpdateManager", "Handed " + setup + " to the installer");
        } else {
            // The package is good even when the hand-over is not; let the user
            // finish it by hand rather than quitting with nothing to show.
            LOG_WARNING("UpdateManager",
                        "Could not start the installer; opening " + setup);
            QDesktopServices::openUrl(QUrl::fromLocalFile(setup));
        }
    }
#endif

    QCoreApplication::quit();
}
