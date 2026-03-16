// In src/utils/ThemeManager.h - create new file

#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QSettings>

struct ThemeInfo {
    QString id;          // Unique identifier (e.g., "catppuccin_latte")
    QString name;        // Display name (e.g., "Catppuccin Latte")
    QString filePath;    // Resource path (e.g., ":/themes/catppuccin_latte.qss")
    bool isDark;         // For system integration hints
    
    ThemeInfo() : isDark(false) {}
    ThemeInfo(const QString& id, const QString& name, const QString& filePath, bool isDark = false)
        : id(id), name(name), filePath(filePath), isDark(isDark) {}
};

class ThemeManager : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(QObject *parent = nullptr);
    ~ThemeManager();

    // Theme registration
    void registerTheme(const ThemeInfo& theme);
    void registerTheme(const QString& id, const QString& name, const QString& filePath, bool isDark = false);
    
    // Theme access
    QVector<ThemeInfo> getAvailableThemes() const;
    ThemeInfo getTheme(const QString& id) const;
    ThemeInfo getCurrentTheme() const;
    QString getCurrentThemeId() const;
    bool isCurrentThemeDark() const;
    
    // Theme application
    bool applyTheme(const QString& themeId);
    bool applyThemeFromFile(const QString& filePath);
    
    // Settings persistence
    void saveCurrentTheme();
    void loadSavedTheme();
    QString getSavedThemeId() const;

signals:
    void themeChanged(const QString& themeId);
    void themeApplied(const ThemeInfo& theme);

private:
    void registerDefaultThemes();
    QString loadStyleSheet(const QString& filePath) const;
    
    QVector<ThemeInfo> _themes;
    QString _currentThemeId;
    QSettings _settings;
};

#endif // THEMEMANAGER_H