#ifndef COLUMNPRESET_H
#define COLUMNPRESET_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <map>

// ─────────────────────────────────────────────────────────────────────────────
// Single column definition: internal key → human-readable header
// ─────────────────────────────────────────────────────────────────────────────
struct ColumnDef
{
    QString key;          // internal name used in Star::getFieldMap()
    QString displayName;  // shown in table header & dialog
    QString category;     // for grouping in the config dialog
    bool    isBoolFlag;   // rendered as ✓ / ✗ instead of text

    ColumnDef() : isBoolFlag(false) {}
    ColumnDef(const QString& k, const QString& d, const QString& c, bool b = false)
        : key(k), displayName(d), category(c), isBoolFlag(b) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// A named, ordered set of column keys
// ─────────────────────────────────────────────────────────────────────────────
struct ColumnPreset
{
    QString id;                       // unique identifier
    QString name;                     // display name
    std::vector<QString> columnKeys;  // ordered list of column keys
    bool isBuiltIn = false;           // true for the four shipped presets

    QJsonObject toJson() const;
    static ColumnPreset fromJson(const QJsonObject& obj);
};

// ─────────────────────────────────────────────────────────────────────────────
// Central registry: knows every possible column and all presets
// ─────────────────────────────────────────────────────────────────────────────
class ColumnPresetManager
{
public:
    static ColumnPresetManager& instance();

    // ── All known columns ───────────────────────────────────────────────────
    const std::vector<ColumnDef>& allColumns() const { return _allColumns; }
    const ColumnDef* columnDef(const QString& key) const;
    QString displayName(const QString& key) const;
    bool    isBoolFlag(const QString& key) const;
    QStringList categories() const;
    std::vector<ColumnDef> columnsForCategory(const QString& cat) const;

    // ── Presets ─────────────────────────────────────────────────────────────
    std::vector<ColumnPreset> allPresets() const;      // built-in + custom
    std::vector<ColumnPreset> builtInPresets() const;
    std::vector<ColumnPreset> customPresets() const;
    const ColumnPreset* preset(const QString& id) const;

    void saveCustomPreset(const ColumnPreset& preset);
    void deleteCustomPreset(const QString& id);
    void renameCustomPreset(const QString& id, const QString& newName);

    // ── Default column set (used when project has none) ─────────────────────
    std::vector<QString> defaultColumns() const;

private:
    ColumnPresetManager();
    void buildColumnRegistry();
    void buildBuiltInPresets();
    void loadCustomPresets();
    void persistCustomPresets();

    std::vector<ColumnDef> _allColumns;
    std::map<QString, ColumnDef> _columnIndex;  // key → def (fast lookup)
    std::vector<ColumnPreset> _builtInPresets;
    std::vector<ColumnPreset> _customPresets;
};

#endif // COLUMNPRESET_H