// In src/utils/ThemeManager.cpp - create new file

#include "ThemeManager.h"
#include "UiIcons.h"
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDebug>

namespace {

// Default fallbacks used when a stylesheet's primary QWidget rule cannot be
// parsed. These mirror the hardcoded light/dark values used elsewhere in the
// app (e.g. PanelUtils::stylePlot).
const QColor kDefaultDarkBg (42, 42, 42);
const QColor kDefaultDarkFg (210, 210, 210);
const QColor kDefaultLightBg(255, 255, 255);
const QColor kDefaultLightFg(42, 42, 42);

// Derive a slightly raised "card" surface from the window background so cards
// read as elevated against the theme. Light themes nudge toward white; dark
// themes lighten a touch. Mirrors the intent behind Rosé Pine Dawn's #fffaf3
// surface sitting one step off its #faf4ed base.
QColor deriveSurface(const QColor& bg, bool isDark)
{
    if (isDark) {
        return bg.lighter(118);
    }
    // Blend the base background a little toward white for a subtle lift.
    const double t = 0.45;
    int r = static_cast<int>(bg.red()   + (255 - bg.red())   * t);
    int g = static_cast<int>(bg.green() + (255 - bg.green()) * t);
    int b = static_cast<int>(bg.blue()  + (255 - bg.blue())  * t);
    return QColor(qMin(r, 255), qMin(g, 255), qMin(b, 255));
}

// Parse the primary `QWidget { ... }` rule near the top of every theme .qss and
// publish the real background/foreground/surface colours as qApp properties so
// the rest of the app can read the active theme reliably (a QSS background-color
// does NOT update a widget's QPalette, so palette reads return stale values).
void publishThemeColors(const QString& styleSheet, bool isDark)
{
    QColor bg = isDark ? kDefaultDarkBg : kDefaultLightBg;
    QColor fg = isDark ? kDefaultDarkFg : kDefaultLightFg;

    // Non-greedy + a leading whitespace/separator anchor so we match the bare
    // `background-color`/`color` declarations and not `selection-background-color`
    // or `selection-color` which also appear in the same QWidget rule.
    static const QRegularExpression bgRe(
        QStringLiteral("QWidget\\s*\\{[^}]*?[\\s;{]background-color\\s*:\\s*(#[0-9a-fA-F]{3,8})"));
    static const QRegularExpression fgRe(
        QStringLiteral("QWidget\\s*\\{[^}]*?[\\s;{]color\\s*:\\s*(#[0-9a-fA-F]{3,8})"));

    auto mbg = bgRe.match(styleSheet);
    if (mbg.hasMatch()) bg = QColor(mbg.captured(1));
    auto mfg = fgRe.match(styleSheet);
    if (mfg.hasMatch()) fg = QColor(mfg.captured(1));

    qApp->setProperty("themeBg", bg);
    qApp->setProperty("themeFg", fg);
    qApp->setProperty("themeSurface", deriveSurface(bg, isDark));
}

} // namespace

ThemeManager::ThemeManager(QObject *parent)
    : QObject(parent)
    , _settings("ASTRA", "ASTRA")
{
    registerDefaultThemes();
    loadSavedTheme();
}

ThemeManager::~ThemeManager()
{
}

void ThemeManager::registerDefaultThemes()
{
    // Light themes
    registerTheme("rose_pine_dawn", "Rosé Pine Dawn", ":/themes/rose_pine_dawn.qss", false);
    registerTheme("catppuccin_latte", "Catppuccin Latte", ":/themes/catppuccin_latte.qss", false);
    registerTheme("github_light", "GitHub Light", ":/themes/github_light.qss", false);
    registerTheme("solarized_light", "Solarized Light", ":/themes/solarized_light.qss", false);
    registerTheme("gruvbox_light", "Gruvbox Light", ":/themes/gruvbox_light.qss", false);
    registerTheme("nord_light", "Nord Light", ":/themes/nord_light.qss", false);
    registerTheme("one_light", "One Light", ":/themes/one_light.qss", false);

    // Dark themes
    registerTheme("catppuccin_mocha", "Catppuccin Mocha", ":/themes/catppuccin_mocha.qss", true);
    registerTheme("dracula", "Dracula", ":/themes/dracula.qss", true);
    registerTheme("nord", "Nord", ":/themes/nord.qss", true);
    registerTheme("gruvbox_dark", "Gruvbox Dark", ":/themes/gruvbox_dark.qss", true);
    registerTheme("tokyo_night", "Tokyo Night", ":/themes/tokyo_night.qss", true);
    registerTheme("solarized_dark", "Solarized Dark", ":/themes/solarized_dark.qss", true);
    registerTheme("one_dark", "One Dark", ":/themes/one_dark.qss", true);
}

void ThemeManager::registerTheme(const ThemeInfo& theme)
{
    // Check if theme with this ID already exists
    for (int i = 0; i < _themes.size(); ++i) {
        if (_themes[i].id == theme.id) {
            _themes[i] = theme;  // Update existing
            return;
        }
    }
    _themes.append(theme);
}

void ThemeManager::registerTheme(const QString& id, const QString& name, const QString& filePath, bool isDark)
{
    registerTheme(ThemeInfo(id, name, filePath, isDark));
}

QVector<ThemeInfo> ThemeManager::getAvailableThemes() const
{
    return _themes;
}

ThemeInfo ThemeManager::getTheme(const QString& id) const
{
    for (const auto& theme : _themes) {
        if (theme.id == id) {
            return theme;
        }
    }
    return ThemeInfo();
}

ThemeInfo ThemeManager::getCurrentTheme() const
{
    return getTheme(_currentThemeId);
}

QString ThemeManager::getCurrentThemeId() const
{
    return _currentThemeId;
}

bool ThemeManager::isCurrentThemeDark() const
{
    return getCurrentTheme().isDark;
}

bool ThemeManager::applyTheme(const QString& themeId)
{
    ThemeInfo theme = getTheme(themeId);
    if (theme.id.isEmpty()) {
        qWarning() << "Theme not found:" << themeId;
        return false;
    }
    
    QString styleSheet = loadStyleSheet(theme.filePath);
    if (styleSheet.isEmpty()) {
        qWarning() << "Failed to load theme stylesheet:" << theme.filePath;
        return false;
    }

    styleSheet = prepareThemeIcons(themeId, styleSheet);

    qApp->setStyleSheet(styleSheet);
    _currentThemeId = themeId;

    qApp->setProperty("isDarkTheme", theme.isDark);
    publishThemeColors(styleSheet, theme.isDark);

    saveCurrentTheme();

    // Re-render C++-side icons in the new theme's arrow colour.
    UiIcons::refresh();

    emit themeChanged(themeId);
    emit themeApplied(theme);
    return true;
}

bool ThemeManager::applyThemeFromFile(const QString& filePath)
{
    QString styleSheet = loadStyleSheet(filePath);
    if (styleSheet.isEmpty()) {
        return false;
    }

    styleSheet = prepareThemeIcons(QFileInfo(filePath).baseName(), styleSheet);

    qApp->setStyleSheet(styleSheet);

    // No registered ThemeInfo here, so infer dark/light from the parsed window
    // background's lightness before publishing the theme colours.
    static const QRegularExpression bgRe(
        QStringLiteral("QWidget\\s*\\{[^}]*?[\\s;{]background-color\\s*:\\s*(#[0-9a-fA-F]{3,8})"));
    bool isDark = qApp->property("isDarkTheme").toBool();
    auto mbg = bgRe.match(styleSheet);
    if (mbg.hasMatch()) isDark = QColor(mbg.captured(1)).lightnessF() < 0.5;

    qApp->setProperty("isDarkTheme", isDark);
    publishThemeColors(styleSheet, isDark);

    UiIcons::refresh();
    return true;
}

void ThemeManager::saveCurrentTheme()
{
    _settings.setValue("appearance/theme", _currentThemeId);
    _settings.sync();
}

void ThemeManager::loadSavedTheme()
{
    QString savedThemeId = getSavedThemeId();
    
    if (savedThemeId.isEmpty() || getTheme(savedThemeId).id.isEmpty()) {
        // Default to first registered theme
        if (!_themes.isEmpty()) {
            savedThemeId = _themes.first().id;
        }
    }
    
    if (!savedThemeId.isEmpty()) {
        applyTheme(savedThemeId);
    }
}

QString ThemeManager::getSavedThemeId() const
{
    return _settings.value("appearance/theme", "rose_pine_dawn").toString();
}

QString ThemeManager::loadStyleSheet(const QString& filePath) const
{
    QFile file(filePath);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        qWarning() << "Cannot open stylesheet file:" << filePath;
        return QString();
    }
    
    QString styleSheet = QString::fromUtf8(file.readAll());
    file.close();

    return styleSheet;
}

QString ThemeManager::prepareThemeIcons(const QString& themeId, const QString& styleSheet) const
{
    // The stylesheets reference subcontrol images via the __ICONDIR__ placeholder,
    // e.g. `image: url(__ICONDIR__/check.svg);`. The actual icon files are
    // monochrome SVG templates in the :/icons/ resource that use the literal token
    // CURRENTCOLOR as their fill/stroke colour. Here we recolour them to match the
    // active theme and write them out to a temp directory, then point __ICONDIR__
    // at that directory.

    // Fallback colours (used when a theme omits the @icons header block) - mid grey
    // foreground reads acceptably on both light and dark backgrounds, and white
    // checkmarks read on an accent-filled indicator box.
    QString checkColor = "#ffffff";  // checkmark / radio dot (shown on accent fill)
    QString arrowColor = "#808080";  // arrows / branches (shown on theme background)

    // Parse the optional @icons header block, e.g.:
    //   /* @icons
    //      check: #faf4ed;
    //      arrow: #575279;
    //   */
    static const QRegularExpression icoCheck(
        QStringLiteral("@icons\\b[\\s\\S]*?\\bcheck\\s*:\\s*(#[0-9a-fA-F]{3,8})"));
    static const QRegularExpression icoArrow(
        QStringLiteral("@icons\\b[\\s\\S]*?\\barrow\\s*:\\s*(#[0-9a-fA-F]{3,8})"));
    auto mc = icoCheck.match(styleSheet);
    if (mc.hasMatch()) checkColor = mc.captured(1);
    auto ma = icoArrow.match(styleSheet);
    if (ma.hasMatch()) arrowColor = ma.captured(1);

    // Publish the arrow colour so C++-side icons (UiIcons) can match the arrows
    // the QSS subcontrols draw. Both applyTheme() paths route through here.
    qApp->setProperty("themeArrow", QColor(arrowColor));

    // Map each icon template to the colour it should be recoloured with.
    struct IconSpec { const char* name; const QString& color; };
    const QVector<IconSpec> icons = {
        { "check.svg",         checkColor },
        { "menu-check.svg",    arrowColor },  // menu sits on theme bg, use foreground
        { "radio.svg",         checkColor },
        { "arrow-down.svg",    arrowColor },
        { "arrow-up.svg",      arrowColor },
        { "arrow-right.svg",   arrowColor },
        { "arrow-left.svg",    arrowColor },
        { "branch-closed.svg", arrowColor },
        { "branch-open.svg",   arrowColor },
        { "play.svg",          arrowColor },
        { "swap.svg",          arrowColor },
        { "arrow-double-right.svg", arrowColor },
        { "arrow-double-left.svg",  arrowColor },
    };

    QString outDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                     + "/astra-icons/" + themeId;
    QDir().mkpath(outDir);

    for (const IconSpec& spec : icons) {
        QFile tmpl(QStringLiteral(":/icons/") + spec.name);
        if (!tmpl.open(QFile::ReadOnly | QFile::Text)) {
            qWarning() << "Cannot open icon template:" << spec.name;
            continue;
        }
        QString svg = QString::fromUtf8(tmpl.readAll());
        tmpl.close();

        svg.replace("CURRENTCOLOR", spec.color);

        QFile out(outDir + "/" + spec.name);
        if (out.open(QFile::WriteOnly | QFile::Truncate)) {
            out.write(svg.toUtf8());
            out.close();
        } else {
            qWarning() << "Cannot write recoloured icon:" << out.fileName();
        }
    }

    QString result = styleSheet;
    result.replace("__ICONDIR__", outDir);
    return result;
}