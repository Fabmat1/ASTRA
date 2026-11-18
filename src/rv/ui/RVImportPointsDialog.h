#pragma once
#include <QDialog>
#include <QStringList>
#include <QHash>
#include <memory>
#include <vector>

#include "core/Time.h"

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
    bool loadFile();
    void fillColumnCombo(QComboBox* combo, int selected);
    void applyColumnGuess();
    /// Re-guesses the scale and epoch offset for the mapped time column.
    void applyScaleGuess();
    void refreshPreview();

    // Map the time-scale combo selection to a TimeScale enum.
    TimeScale selectedScale() const;
    bool isoDates() const;

    /// The number added to every timestamp before it is read on the selected
    /// scale, for tables that tabulate a reduced Julian date. Auto-filled from
    /// the file when it says so, and editable when it does not.
    double epochOffset() const;

    /// The RV error given to rows without one; NaN when the field is blank or
    /// not a positive number.
    double defaultError() const;
    /// The systematic error given to every row when no systematic column is
    /// mapped; 0 when the field is blank.
    double flatSystematic() const;

    // Resolve a free-text instrument token (from the instrument column) to a
    // known Instrument, falling back to the default-instrument selector.
    void buildInstrumentLookup();
    std::shared_ptr<Instrument> matchInstrument(const QString& token) const;
    std::shared_ptr<Instrument> defaultInstrument() const;

    /// One table row read with the current mapping.
    struct ParsedRow {
        double value = 0.0;          // timestamp on selectedScale(), offset applied
        double rv = 0.0;
        double errFormal = 0.0;
        double errSys = 0.0;
        bool   errDefaulted = false; // formal error taken from the default
        int    component = 1;
        std::shared_ptr<Instrument> inst;
    };
    struct ParseResult {
        std::vector<ParsedRow> rows;
        int     unparsed = 0;        // no readable time or RV
        int     implausible = 0;     // timestamp impossible on the scale
        double  firstImplausible = 0.0;
        int     missingFormal = 0;   // rows without a formal error
        int     unweighted = 0;      // rows left with no error at all
        int     withoutInstrument = 0;
        QStringList unknownInstruments;
    };
    ParseResult parseRows() const;
    /// The timestamp of a parsed row, with MJD/BJD resolved as far as the
    /// star's coordinates and the row's instrument allow.
    Time makeTime(const ParsedRow& r) const;

    std::shared_ptr<Star> _star;
    DatabaseManager*      _dbm = nullptr;
    std::vector<std::shared_ptr<RadialVelocityPoint>> _results;

    // Loaded table
    QStringList                _columns;
    std::vector<QStringList>   _rows;
    bool                       _hasHeader = true;
    /// Set once the user touches the header checkbox; until then each file
    /// gets its header detected.
    bool                       _headerChosen = false;
    /// What the time-scale guess had to say (e.g. a name the values overrule).
    QString                    _scaleNote;
    /// The file's '#' lines, kept because VizieR states a column's epoch
    /// offset there and nowhere else.
    QStringList                _metadataLines;

    // Instrument name/id → object lookup for per-row matching.
    QHash<QString, std::shared_ptr<Instrument>> _instByKey;

    // ── Widgets ──────────────────────────────────────────────────────────────
    QLineEdit*  _fileEdit   = nullptr;
    QComboBox*  _delimCombo  = nullptr;
    QCheckBox*  _headerCheck = nullptr;

    QComboBox*  _timeColCombo  = nullptr;
    QComboBox*  _timeTypeCombo = nullptr;
    QLineEdit*  _epochOffsetEdit = nullptr;
    QComboBox*  _rvColCombo     = nullptr;
    QComboBox*  _errColCombo     = nullptr;
    QComboBox*  _sysErrColCombo  = nullptr;
    QComboBox*  _instColCombo     = nullptr;   // per-row instrument column
    QComboBox*  _compColCombo     = nullptr;   // per-row stellar component (1/2)
    QComboBox*  _instCombo        = nullptr;   // default / fallback instrument
    QLineEdit*  _defaultErrEdit   = nullptr;   // RV error for rows without one
    QLineEdit*  _flatSysEdit      = nullptr;   // systematic error without a column

    QTableWidget* _preview    = nullptr;
    QLabel*       _status     = nullptr;
    QDialogButtonBox* _buttons = nullptr;
};
