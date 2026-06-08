#pragma once
#include <QDialog>
#include <QStringList>
#include <QHash>
#include <memory>
#include <vector>

#include "models/Time.h"

class Star;
class DatabaseManager;
class Instrument;
class RadialVelocityPoint;

class QLineEdit;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QLabel;
class QPushButton;
class QDialogButtonBox;

// ─────────────────────────────────────────────────────────────────────────────
//   Bulk-import RV points for a single star from a CSV/ASCII table.
//
//   Mirrors the "From a single RV table" flow of the Star Import Wizard
//   (RadialVelocityImportPage) but scoped to one star: the star identifier
//   column is replaced by an instrument selector that lets each imported
//   timestamp be converted to BJD via the proper barycentric correction.
// ─────────────────────────────────────────────────────────────────────────────
class RVImportPointsDialog : public QDialog
{
    Q_OBJECT
public:
    RVImportPointsDialog(std::shared_ptr<Star> star,
                         DatabaseManager* dbm,
                         QWidget* parent = nullptr);

    std::vector<std::shared_ptr<RadialVelocityPoint>> results() const
    { return _results; }

private slots:
    void onBrowse();
    void onReloadFile();
    void onAccept();

private:
    void setupUi();

    // CSV helpers (self-contained, modelled on RadialVelocityImportPage)
    QChar       delimiter() const;
    static QChar detectDelimiter(const QString& line);
    static QStringList parseLine(const QString& line, QChar delim);
    bool loadFile();
    void populateColumnCombo(QComboBox* combo, const QStringList& patterns);
    void refreshPreview();

    // Map the time-scale combo selection to a TimeScale enum.
    TimeScale selectedScale() const;

    // Resolve a free-text instrument token (from the instrument column) to a
    // known Instrument, falling back to the default-instrument selector.
    void buildInstrumentLookup();
    std::shared_ptr<Instrument> resolveRowInstrument(const QStringList& row,
                                                     int instCol) const;

    std::shared_ptr<Star> _star;
    DatabaseManager*      _dbm = nullptr;
    std::vector<std::shared_ptr<RadialVelocityPoint>> _results;

    // Loaded table
    QStringList                _columns;
    std::vector<QStringList>   _rows;

    // Instrument name/id → object lookup for per-row matching.
    QHash<QString, std::shared_ptr<Instrument>> _instByKey;

    // ── Widgets ──────────────────────────────────────────────────────────────
    QLineEdit*  _fileEdit   = nullptr;
    QComboBox*  _delimCombo  = nullptr;
    QCheckBox*  _headerCheck = nullptr;

    QComboBox*  _timeColCombo  = nullptr;
    QComboBox*  _timeTypeCombo = nullptr;
    QComboBox*  _rvColCombo     = nullptr;
    QComboBox*  _errColCombo     = nullptr;
    QComboBox*  _sysErrColCombo  = nullptr;
    QComboBox*  _instColCombo     = nullptr;   // per-row instrument column
    QComboBox*  _instCombo        = nullptr;   // default / fallback instrument

    QTableWidget* _preview    = nullptr;
    QLabel*       _status     = nullptr;
    QDialogButtonBox* _buttons = nullptr;
};
