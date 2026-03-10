// In src/utils/ThemeManager.cpp - create new file

#include "ThemeManager.h"
#include <QApplication>
#include <QFile>
#include <QDebug>

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
    registerTheme("rose_pine_dawn", "Rosé Pine Dawn", ":/themes/rose_pine_dawn.qss", false);
    registerTheme("one_light", "One Light", ":/themes/one_light.qss", false);
    registerTheme("catppuccin_mocha", "Catppuccin Mocha", ":/themes/catppuccin_dark.qss", true);
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
    
    qApp->setStyleSheet(styleSheet);
    _currentThemeId = themeId;
    
    saveCurrentTheme();
    
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
    
    qApp->setStyleSheet(styleSheet);
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
    return _settings.value("appearance/theme", "catppuccin_latte").toString();
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