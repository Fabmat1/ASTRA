#pragma once

#include <QDate>
#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QTextBrowser;

/// One rendered entry of the "What's New" timeline: either a release taken from
/// resources/changelog/CHANGELOG.md or a dev-authored announcement taken from
/// resources/changelog/NEWS.md.
struct ChangelogEntry {
    QString title;          ///< Header text, e.g. "v0.5.6" or "Grid server moved".
    QString version;        ///< Normalised semver ("0.5.6") when the title is one, else "".
    QDate   date;           ///< Date from the header's trailing "(YYYY-MM-DD)".
    QString body;           ///< Markdown body below the header.
    bool    isNews  = false;///< True for NEWS.md entries (badged, not a release).
    bool    pinned  = false;///< News marked "[pinned]" sort above everything else.
};

/**
 * Scrollable "What's New" window showing ASTRA's complete version history plus
 * any custom news the developer added, newest first.
 *
 * Content lives in two bundled markdown files (see the comment headers there
 * for the exact format):
 *   * `:/changelog/CHANGELOG.md`: the release history.
 *   * `:/changelog/NEWS.md`:     free-form announcements, merged in by date.
 *
 * The window opens by itself on the first launch after the running version
 * changed, which for a source build means a new git commit and for a packaged
 * build means a new release. It is always reachable from Help → What's New, and
 * the "Don't show this again" checkbox at the bottom turns the automatic pop-up
 * off (unticking it in a manually opened window turns it back on).
 */
class WhatsNewDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WhatsNewDialog(QWidget* parent = nullptr);

    /// True when the window should pop up on its own right now: the automatic
    /// pop-up is enabled, we have shown it at least once before (a fresh
    /// install is not an "update"), and the running version differs from the
    /// one recorded at the last showing.
    static bool shouldShowOnStartup();

    /// Record the running version *and* the newest entry date as seen. Called
    /// when the window closes: the window won't reappear until the next update,
    /// and nothing currently listed counts as "new" any more.
    static void markSeen();

    /// Record only the running version as seen. Used at startup when the
    /// window is *not* shown (fresh install, or the pop-up is switched off), so
    /// entries the user never actually saw keep their "NEW" badge for whenever
    /// they open the window from the Help menu.
    static void markVersionSeen();

    /// Whether the window opens by itself after an update.
    static bool autoShowEnabled();
    static void setAutoShowEnabled(bool on);

    /// Parse the bundled changelog + news into a single timeline, newest first.
    /// Exposed for reuse (and testing); the dialog renders exactly this.
    static QVector<ChangelogEntry> loadEntries();

private:
    void buildDocument();
    void onFinished();

    /// Newest entry date the user has already seen; entries after it are badged
    /// "NEW". Captured at construction, before markSeen() overwrites it.
    QDate _seenUpTo;
    /// Auto-show state when the dialog opened, to detect the user turning it off.
    bool  _autoShowAtOpen = true;

    QTextBrowser* _browser  = nullptr;
    QCheckBox*    _dontShow = nullptr;
};
