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
    QString appImageUrl;   ///< Download URL of the *-x86_64.AppImage asset.
    QString sha256Url;     ///< Download URL of the matching .sha256 asset.
    QString assetName;     ///< File name of the AppImage asset.
    qint64  assetSize = 0; ///< Size of the AppImage asset in bytes (0 if unknown).

    bool hasAppImage() const { return !appImageUrl.isEmpty(); }
};

/// Checks GitHub for newer ASTRA releases and, when running from an AppImage,
/// can download, verify and install the new version in place.
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
    /// Development builds (git-<hash>) return false and are never auto-nagged.
    static bool isReleaseBuild();

    /// Absolute path of the AppImage we were launched from, or "" when ASTRA is
    /// not running from an AppImage. Set by the AppImage runtime via $APPIMAGE
    /// (present even with --appimage-extract-and-run).
    static QString appImagePath();

    /// True when running from an AppImage that we can replace in place.
    static bool isAppImage();

    /// Compare two version strings. Returns <0, 0, >0 like strcmp. A leading
    /// 'v' and any pre-release suffix after '-'/'+' are ignored.
    static int compareVersions(const QString& a, const QString& b);

    /// The most recent release fetched by checkForUpdates() (empty until then).
    const UpdateInfo& latestInfo() const { return _latest; }

    // ── Operations ───────────────────────────────────────────────────────
    /// Query GitHub for the latest release. Emits updateAvailable / upToDate /
    /// checkFailed. When respectSkip is true, a release the user previously
    /// chose to skip is reported as upToDate (used for the silent startup check).
    void checkForUpdates(bool respectSkip = true);

    /// Download the AppImage for `info`, verify its sha256 and atomically swap
    /// it over the current AppImage. AppImage runs only. Emits downloadProgress,
    /// then installFinished or installFailed.
    void downloadAndInstall(const UpdateInfo& info);

    /// Abort an in-progress download.
    void cancelDownload();

    /// Re-launch the (just updated) AppImage and ask this instance to quit.
    static void relaunch();

signals:
    void checkStarted();
    void updateAvailable(const UpdateInfo& info);
    void upToDate(const QString& currentVersion);
    void checkFailed(const QString& error);

    void downloadProgress(qint64 received, qint64 total);
    void installFinished(const QString& installedPath);
    void installFailed(const QString& error);

private:
    void onReleaseReply(QNetworkReply* reply, bool respectSkip);
    void startAppImageDownload(const QString& expectedSha256);
    void cleanupDownload();
    void failInstall(const QString& error);

    QNetworkAccessManager* _nam = nullptr;

    UpdateInfo _latest;

    // Active download state.
    UpdateInfo         _dlInfo;
    QNetworkReply*     _dlReply  = nullptr;
    QFile*             _dlFile   = nullptr;
    QString            _dlPath;
    QCryptographicHash* _dlHash  = nullptr;
};
