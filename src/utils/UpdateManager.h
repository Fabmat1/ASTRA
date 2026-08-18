#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QCryptographicHash;

/// Describes a release fetched from GitHub.
struct UpdateInfo {
    QString version;       ///< Normalised semver, e.g. "0.4.0".
    QString tagName;       ///< Raw git tag, e.g. "v0.4.0".
    QString name;          ///< Release title.
    QString releaseNotes;  ///< Release body (markdown).
    QString htmlUrl;       ///< Browser URL of the release page.
    QString packageUrl;    ///< Download URL of the asset for *this* platform
                           ///< (*.AppImage on Linux, *.dmg on macOS).
    QString sha256Url;     ///< Download URL of that asset's .sha256 sibling.
    QString assetName;     ///< File name of the platform asset.
    qint64  assetSize = 0; ///< Size of the platform asset in bytes (0 if unknown).

    /// True when the release ships an installable asset for this platform.
    bool hasPackage() const { return !packageUrl.isEmpty(); }
};

/// Checks GitHub for newer ASTRA releases and, when running from a package we
/// know how to replace, downloads, verifies and installs the new version.
///
/// Two install targets are supported:
///   • Linux  - the AppImage we were launched from is swapped in place.
///   • macOS  - the .dmg is mounted and the new ASTRA.app replaces the bundle
///              we are running from. When that bundle is not writable (e.g. it
///              belongs to another admin user), the download is handed to the
///              Finder instead via manualInstallRequired().
///
/// The startup check is fire-and-forget: connect to the signals and call
/// checkForUpdates(). All network work is asynchronous.
class UpdateManager : public QObject
{
    Q_OBJECT
public:
    explicit UpdateManager(QObject* parent = nullptr);
    ~UpdateManager() override;

    // ── Environment helpers ──────────────────────────────────────────────
    /// The running ASTRA version, normalised to bare semver ("0.3.0") when it
    /// is a tagged release, otherwise the raw build string ("git-abc123").
    static QString currentVersion();

    /// True when the current build is a clean tagged release (parseable semver).
    /// Development builds (git-<hash>) return false; they are never auto-nagged,
    /// but manual checks still offer the latest release.
    static bool isReleaseBuild();

    /// Absolute path of the AppImage we were launched from, or "" when ASTRA is
    /// not running from an AppImage. Set by the AppImage runtime via $APPIMAGE
    /// (present even with --appimage-extract-and-run).
    static QString appImagePath();

    /// True when running from an AppImage that we can replace in place.
    static bool isAppImage();

    /// Absolute path of the .app bundle we are running from (macOS), or "" when
    /// ASTRA runs from a plain build tree / another platform.
    static QString appBundlePath();

    /// True when running from a macOS .app bundle we can replace.
    static bool isAppBundle();

    /// True when this build knows how to install an update over itself
    /// (AppImage on Linux, .app bundle on macOS).
    static bool canSelfInstall();

    /// Human-readable name of the packaging we are running from ("AppImage",
    /// "macOS app bundle"), or "" when ASTRA runs unpackaged.
    static QString packagingName();

    /// Compare two version strings. Returns <0, 0, >0 like strcmp. A leading
    /// 'v' and any pre-release suffix after '-'/'+' are ignored.
    static int compareVersions(const QString& a, const QString& b);

    /// The most recent release fetched by checkForUpdates() (empty until then).
    const UpdateInfo& latestInfo() const { return _latest; }

    // ── Operations ───────────────────────────────────────────────────────
    /// Query GitHub for the latest release. Emits updateAvailable / upToDate /
    /// checkFailed. When respectSkip is true, a release the user previously
    /// chose to skip is reported as upToDate (used for the silent startup
    /// check), and development builds report upToDate as well so they are never
    /// nagged. A manual check (respectSkip = false) offers the latest release to
    /// development builds too.
    void checkForUpdates(bool respectSkip = true);

    /// Download this platform's asset for `info`, verify its sha256 and install
    /// it over the running AppImage / app bundle. Emits downloadProgress, then
    /// one of installFinished, manualInstallRequired or installFailed.
    void downloadAndInstall(const UpdateInfo& info);

    /// Abort an in-progress download.
    void cancelDownload();

    /// Re-launch the (just updated) ASTRA and ask this instance to quit.
    static void relaunch();

signals:
    void checkStarted();
    void updateAvailable(const UpdateInfo& info);
    void upToDate(const QString& currentVersion);
    void checkFailed(const QString& error);

    void downloadProgress(qint64 received, qint64 total);
    /// The download finished and the (possibly slow) install step has begun.
    void installStarted();
    void installFinished(const QString& installedPath);
    /// The download is verified and usable, but ASTRA could not replace itself
    /// (typically an app bundle owned by another user). `packagePath` is the
    /// downloaded .dmg, which has been handed to the Finder for a manual drag.
    void manualInstallRequired(const QString& packagePath, const QString& reason);
    void installFailed(const QString& error);

private:
    void onReleaseReply(QNetworkReply* reply, bool respectSkip);
    void startPackageDownload(const QString& expectedSha256);
    void finishInstall();
    void installAppImage();
    void installMacBundle();
    void clearSkipMarker();
    void cleanupDownload();
    void failInstall(const QString& error);
    void requireManualInstall(const QString& reason);

    QNetworkAccessManager* _nam = nullptr;

    UpdateInfo _latest;

    // Active download state.
    UpdateInfo         _dlInfo;
    QNetworkReply*     _dlReply  = nullptr;
    QFile*             _dlFile   = nullptr;
    QString            _dlPath;
    QCryptographicHash* _dlHash  = nullptr;
};
