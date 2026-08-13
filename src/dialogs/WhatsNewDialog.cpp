#include "WhatsNewDialog.h"

#include "astra_version.h"
#include "utils/UpdateManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr const char* kAutoShow   = "whatsNew/autoShow";
constexpr const char* kLastVersion = "whatsNew/lastSeenVersion";
constexpr const char* kLastDate    = "whatsNew/lastSeenEntryDate";

constexpr const char* kChangelogPath = ":/changelog/CHANGELOG.md";
constexpr const char* kNewsPath      = ":/changelog/NEWS.md";

// ── Colours ──────────────────────────────────────────────────────────────────
// The active theme's real colours live on qApp properties published by
// ThemeManager (a QSS background-color does not update the QPalette, so reading
// the palette here would return stale defaults).
struct Palette {
    QColor bg, fg, surface, muted, accent, newsBg, newBadgeBg, badgeFg;
    int    monoPt = 9;   ///< Point size for monospaced runs, see themePalette().
};

QColor blend(const QColor& a, const QColor& b, double t)
{
    return QColor(int(a.red()   + (b.red()   - a.red())   * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue()  + (b.blue()  - a.blue())  * t));
}

/// Resolve the colours (and the monospace point size) the document is rendered
/// with. `basePt` is the point size of the widget the text lands in; a bare
/// `font-family:monospace` is resolved by Qt at the fixed font's *own* size,
/// which is bigger than the surrounding text and breaks the line rhythm, so
/// monospaced runs are pinned just below the body text instead.
Palette themePalette(int basePt = -1)
{
    Palette p;
    const QVariant vbg = qApp->property("themeBg");
    const QVariant vfg = qApp->property("themeFg");
    const QVariant vsf = qApp->property("themeSurface");
    // Fall back to the application palette when no theme has been applied yet,
    // so the text never ends up dark-on-dark.
    const QPalette qp = QApplication::palette();
    p.bg      = vbg.isValid() ? vbg.value<QColor>() : qp.color(QPalette::Base);
    p.fg      = vfg.isValid() ? vfg.value<QColor>() : qp.color(QPalette::Text);
    p.surface = vsf.isValid() ? vsf.value<QColor>() : blend(p.bg, p.fg, 0.10);

    const bool dark = p.bg.lightness() < 128;
    p.muted      = blend(p.fg, p.bg, 0.42);
    p.accent     = dark ? QColor(0x8a, 0xb4, 0xf8) : QColor(0x1a, 0x5f, 0xb4);
    p.newsBg     = dark ? QColor(0x4b, 0x3a, 0x1e) : QColor(0xff, 0xec, 0xc7);
    p.newBadgeBg = dark ? QColor(0x2c, 0x4c, 0x33) : QColor(0xd4, 0xf2, 0xdd);
    p.badgeFg    = dark ? QColor(0xef, 0xef, 0xef) : QColor(0x2a, 0x2a, 0x2a);

    int base = basePt;
    if (base <= 0) base = QApplication::font().pointSize();
    p.monoPt = base > 0 ? qMax(7, base - 1) : 9;
    return p;
}

// ── Markdown to Qt rich text ─────────────────────────────────────────────────
// Qt's rich text engine understands a subset of HTML/CSS, and QTextDocument's
// own setMarkdown() gives us no control over colours or badges, so we render
// the small markdown dialect documented in CHANGELOG.md ourselves.

/// Monospaced runs carry no background of their own: the themes paint the text
/// area from their own QSS, and any colour we picked here would sit slightly
/// off it. The font switch alone marks the run as code.
QString monoStyle(const Palette& p)
{
    return QStringLiteral("font-family:'FiraCode','DejaVu Sans Mono',monospace; "
                          "font-size:%1pt; color:%2;")
        .arg(p.monoPt)
        .arg(p.fg.name());
}

QString renderInline(QString s, const Palette& p)
{
    s = s.toHtmlEscaped();

    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    s.replace(codeRe, QStringLiteral("<span style=\"%1\">\\1</span>")
                          .arg(monoStyle(p)));

    static const QRegularExpression linkRe(
        QStringLiteral("\\[([^\\]]+)\\]\\(([^)\\s]+)\\)"));
    s.replace(linkRe, QStringLiteral("<a href=\"\\2\" style=\"color:%1;\">\\1</a>")
                          .arg(p.accent.name()));

    static const QRegularExpression boldRe(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    s.replace(boldRe, QStringLiteral("<b>\\1</b>"));

    static const QRegularExpression italRe(
        QStringLiteral("(?<![\\w*])\\*([^*\\n]+)\\*(?![\\w*])"));
    s.replace(italRe, QStringLiteral("<i>\\1</i>"));

    return s;
}

/// Render an entry body (everything below a `##` header) to rich text.
/// Supports `###` sub-headings, `-`/`*` bullet lists with one nesting level,
/// ``` fenced code blocks and plain paragraphs.
QString renderBody(const QString& markdown, const Palette& p)
{
    QString out;
    QStringList paragraph;      // pending plain-text lines
    int  listDepth = 0;         // number of open <ul>
    bool inCode    = false;
    QStringList code;

    auto flushParagraph = [&] {
        if (paragraph.isEmpty())
            return;
        out += QStringLiteral("<p style=\"margin-top:4px; margin-bottom:8px; "
                              "color:%1;\">%2</p>")
                   .arg(p.fg.name(), renderInline(paragraph.join(' '), p));
        paragraph.clear();
    };
    auto closeLists = [&] {
        while (listDepth > 0) { out += QStringLiteral("</ul>"); --listDepth; }
    };

    const QStringList lines = markdown.split('\n');
    for (const QString& raw : lines) {
        const QString trimmed = raw.trimmed();

        if (trimmed.startsWith(QStringLiteral("```"))) {
            if (inCode) {
                out += QStringLiteral("<pre style=\"%1 margin-left:14px;\">"
                                      "%2</pre>")
                           .arg(monoStyle(p), code.join('\n').toHtmlEscaped());
                code.clear();
            } else {
                flushParagraph();
                closeLists();
            }
            inCode = !inCode;
            continue;
        }
        if (inCode) { code << raw; continue; }

        if (trimmed.isEmpty()) {
            flushParagraph();
            closeLists();
            continue;
        }

        if (trimmed.startsWith(QStringLiteral("###"))) {
            flushParagraph();
            closeLists();
            const QString head = trimmed.mid(3).trimmed();
            out += QStringLiteral("<p style=\"margin-top:10px; margin-bottom:2px; "
                                  "font-weight:bold; color:%1;\">%2</p>")
                       .arg(p.muted.name(), renderInline(head, p));
            continue;
        }

        static const QRegularExpression bulletRe(
            QStringLiteral("^(\\s*)[-*]\\s+(.*)$"));
        const auto m = bulletRe.match(raw);
        if (m.hasMatch()) {
            flushParagraph();
            // Two spaces of indentation per nesting level; clamp at one nested
            // level, which is all the changelog format promises.
            const int want =
                std::min(1 + static_cast<int>(m.captured(1).size()) / 2, 2);
            while (listDepth < want) {
                out += QStringLiteral("<ul style=\"margin-top:0px; "
                                      "margin-bottom:0px;\">");
                ++listDepth;
            }
            while (listDepth > want) { out += QStringLiteral("</ul>"); --listDepth; }
            out += QStringLiteral("<li style=\"margin-bottom:3px; color:%1;\">%2</li>")
                       .arg(p.fg.name(), renderInline(m.captured(2), p));
            continue;
        }

        if (listDepth > 0) {
            // A plain line directly under a bullet continues that bullet.
            out.chop(QStringLiteral("</li>").size());
            out += QStringLiteral(" ") + renderInline(trimmed, p)
                 + QStringLiteral("</li>");
            continue;
        }
        paragraph << trimmed;
    }

    if (inCode && !code.isEmpty())
        out += QStringLiteral("<pre style=\"%1 margin-left:14px;\">%2</pre>")
                   .arg(monoStyle(p), code.join('\n').toHtmlEscaped());
    flushParagraph();
    closeLists();
    return out;
}

// ── Changelog / news parsing ─────────────────────────────────────────────────

QString readResource(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString text = QString::fromUtf8(f.readAll());
    // The files lead with an HTML comment documenting the format; drop every
    // comment so it never leaks into the rendered output.
    static const QRegularExpression commentRe(
        QStringLiteral("<!--.*?-->"),
        QRegularExpression::DotMatchesEverythingOption);
    return text.remove(commentRe);
}

/// Split a markdown file into `## <title> (YYYY-MM-DD) [pinned]` entries.
QVector<ChangelogEntry> parseEntries(const QString& text, bool isNews)
{
    QVector<ChangelogEntry> entries;
    if (text.trimmed().isEmpty())
        return entries;

    static const QRegularExpression headerRe(QStringLiteral("^##\\s+(.*\\S)\\s*$"));
    static const QRegularExpression metaRe(
        QStringLiteral("^(.*?)\\s*\\((\\d{4}-\\d{2}-\\d{2})\\)\\s*(\\[pinned\\])?$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression versionRe(
        QStringLiteral("^v?(\\d+)\\.(\\d+)(?:\\.(\\d+))?$"));

    ChangelogEntry current;
    bool haveCurrent = false;
    QStringList body;

    auto flush = [&] {
        if (!haveCurrent)
            return;
        current.body = body.join('\n');
        entries.append(current);
        body.clear();
        haveCurrent = false;
    };

    const QStringList lines = text.split('\n');
    for (const QString& line : lines) {
        const auto hm = headerRe.match(line);
        if (!hm.hasMatch()) {
            if (haveCurrent)
                body << line;
            continue;
        }
        flush();

        current = ChangelogEntry{};
        current.isNews = isNews;
        current.title  = hm.captured(1).trimmed();

        if (const auto mm = metaRe.match(current.title); mm.hasMatch()) {
            current.title  = mm.captured(1).trimmed();
            current.date   = QDate::fromString(mm.captured(2), Qt::ISODate);
            current.pinned = !mm.captured(3).isEmpty();
        }
        if (const auto vm = versionRe.match(current.title); vm.hasMatch()) {
            current.version = QStringLiteral("%1.%2.%3")
                                  .arg(vm.captured(1), vm.captured(2),
                                       vm.captured(3).isEmpty()
                                           ? QStringLiteral("0")
                                           : vm.captured(3));
        }
        haveCurrent = true;
    }
    flush();
    return entries;
}

} // namespace

// ── Content ──────────────────────────────────────────────────────────────────

QVector<ChangelogEntry> WhatsNewDialog::loadEntries()
{
    QVector<ChangelogEntry> entries = parseEntries(readResource(kChangelogPath), false);
    entries += parseEntries(readResource(kNewsPath), true);

    // Pinned news first, then strictly newest-first. Entries without a date
    // (a malformed header) sink to the bottom rather than jumping to the top.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const ChangelogEntry& a, const ChangelogEntry& b) {
        if (a.pinned != b.pinned)          return a.pinned;
        if (a.date.isValid() != b.date.isValid()) return a.date.isValid();
        if (a.date != b.date)              return a.date > b.date;
        // Same day: an announcement explains the release next to it, so it
        // reads better above the release notes.
        if (a.isNews != b.isNews)          return a.isNews;
        return false;
    });
    return entries;
}

// ── Seen / auto-show bookkeeping ─────────────────────────────────────────────

bool WhatsNewDialog::autoShowEnabled()
{
    QSettings s;
    return s.value(kAutoShow, true).toBool();
}

void WhatsNewDialog::setAutoShowEnabled(bool on)
{
    QSettings s;
    s.setValue(kAutoShow, on);
    s.sync();
}

void WhatsNewDialog::markVersionSeen()
{
    QSettings s;
    s.setValue(kLastVersion, QString::fromLatin1(ASTRA_VERSION_STRING));
    s.sync();
}

void WhatsNewDialog::markSeen()
{
    QDate newest;
    for (const ChangelogEntry& e : loadEntries()) {
        if (e.date.isValid() && (!newest.isValid() || e.date > newest))
            newest = e.date;
    }

    QSettings s;
    s.setValue(kLastVersion, QString::fromLatin1(ASTRA_VERSION_STRING));
    if (newest.isValid())
        s.setValue(kLastDate, newest.toString(Qt::ISODate));
    s.sync();
}

bool WhatsNewDialog::shouldShowOnStartup()
{
    if (!autoShowEnabled())
        return false;

    QSettings s;
    const QString last = s.value(kLastVersion).toString();
    // Never recorded: this is a fresh install, not an update. The caller seeds
    // the marker via markVersionSeen() so the *next* version change pops up.
    if (last.isEmpty())
        return false;

    // One rule for both packaging kinds: the version string changed. For a
    // source build that is a new commit (git-<hash>), for a packaged build a
    // new release tag.
    return last != QString::fromLatin1(ASTRA_VERSION_STRING);
}

// ── UI ───────────────────────────────────────────────────────────────────────

WhatsNewDialog::WhatsNewDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("What's New in ASTRA"));
    setModal(true);
    setSizeGripEnabled(true);
    resize(780, 640);
    setMinimumSize(520, 380);

    {
        QSettings s;
        _seenUpTo = QDate::fromString(s.value(kLastDate).toString(), Qt::ISODate);
    }
    _autoShowAtOpen = autoShowEnabled();

    const Palette p = themePalette();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    auto* header = new QLabel(
        tr("<span style=\"font-size:15pt; font-weight:bold;\">What's New in ASTRA</span>"
           "<br><span style=\"color:%1;\">You are running <b>%2</b>. "
           "The full history is below, newest first.</span>")
            .arg(p.muted.name(),
                 QString::fromLatin1(ASTRA_VERSION_STRING).toHtmlEscaped()));
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    root->addWidget(header);

    _browser = new QTextBrowser(this);
    _browser->setOpenExternalLinks(true);
    _browser->setFrameShape(QFrame::StyledPanel);
    _browser->document()->setDocumentMargin(14);
    root->addWidget(_browser, 1);

    auto* footer = new QHBoxLayout;
    _dontShow = new QCheckBox(tr("Don't show this again"), this);
    _dontShow->setChecked(!_autoShowAtOpen);
    _dontShow->setToolTip(tr("Stop this window from opening by itself after an "
                             "update. It stays available under Help > What's New."));
    footer->addWidget(_dontShow);
    footer->addStretch(1);

    auto* buttons = new QDialogButtonBox(this);
    auto* niceBtn = buttons->addButton(tr("Nice!"), QDialogButtonBox::AcceptRole);
    niceBtn->setDefault(true);
    niceBtn->setMinimumWidth(110);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    footer->addWidget(buttons);
    root->addLayout(footer);

    buildDocument();

    connect(this, &QDialog::finished, this, [this](int) { onFinished(); });
}

void WhatsNewDialog::buildDocument()
{
    // Size monospaced runs against the font the text actually renders in, which
    // the theme's QSS may have changed from the application default.
    const Palette p = themePalette(_browser->font().pointSize());
    const QVector<ChangelogEntry> entries = loadEntries();

    if (entries.isEmpty()) {
        _browser->setHtml(QStringLiteral("<p style=\"color:%1;\">%2</p>")
                              .arg(p.muted.name(),
                                   tr("No version history is bundled with this "
                                      "build.")));
        return;
    }

    const QString running = UpdateManager::currentVersion();

    auto badge = [](const QString& text, const QColor& bg, const QColor& fg) {
        return QStringLiteral("<span style=\"background-color:%1; color:%2; "
                              "font-size:8pt; font-weight:bold;\">"
                              "&nbsp;%3&nbsp;</span>&nbsp;")
            .arg(bg.name(), fg.name(), text.toHtmlEscaped());
    };

    QString html;
    html.reserve(64 * 1024);

    for (const ChangelogEntry& e : entries) {
        QString badges;
        if (e.isNews)
            badges += badge(tr("News"), p.newsBg, p.badgeFg);
        if (!e.version.isEmpty() && e.version == running)
            badges += badge(tr("Installed"), p.surface, p.fg);
        if (e.date.isValid() && _seenUpTo.isValid() && e.date > _seenUpTo)
            badges += badge(tr("NEW"), p.newBadgeBg, p.badgeFg);

        html += QStringLiteral(
                    "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                    "<tr><td><span style=\"font-size:13pt; font-weight:bold; "
                    "color:%1;\">%2</span></td>"
                    "<td align=\"right\">%3<span style=\"color:%4;\">%5</span>"
                    "</td></tr></table>")
                    .arg(e.isNews ? p.accent.name() : p.fg.name(),
                         e.title.toHtmlEscaped(),
                         badges,
                         p.muted.name(),
                         e.date.isValid()
                             ? e.date.toString(QStringLiteral("d MMM yyyy"))
                             : QString());
        html += QStringLiteral("<hr style=\"height:1px;\" />");
        html += renderBody(e.body, p);
        html += QStringLiteral("<p style=\"margin-top:14px;\">&nbsp;</p>");
    }

    _browser->setHtml(html);
    _browser->verticalScrollBar()->setValue(0);
}

void WhatsNewDialog::onFinished()
{
    const bool dontShow = _dontShow->isChecked();
    setAutoShowEnabled(!dontShow);
    markSeen();

    // Only explain the consequence when the user just switched it off, not on
    // every close of an already-disabled window.
    if (dontShow && _autoShowAtOpen) {
        QWidget* anchor  = parentWidget();
        QObject* context = anchor ? static_cast<QObject*>(anchor) : qApp;
        QTimer::singleShot(0, context, [anchor] {
            QMessageBox::information(
                anchor, tr("What's New"),
                tr("ASTRA won't open this window automatically any more.\n\n"
                   "You can still read it at any time from Help → What's New, "
                   "and unticking \"Don't show this again\" there turns the "
                   "automatic pop-up back on."));
        });
    }
}
