#pragma once

#include "models/Time.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <vector>

class Instrument;
class Spectrum;
class QTableWidget;

/// Review step for "Add Spectra…": one row per picked file, where the user
/// confirms the instrument/mode and the observation time before anything is
/// written to the database.
///
/// Times are pre-filled from the FITS header when there is one, and otherwise
/// from a sidecar file next to the spectrum (the `<name>_mjd.txt` convention
/// the import wizard understands).
class AddSpectraDialog : public QDialog
{
    Q_OBJECT
public:
    struct Entry {
        QString                   path;
        std::shared_ptr<Spectrum> spectrum;   // already read from `path`
    };

    struct DetectedTime {
        double    value = 0.0;
        TimeScale scale = TimeScale::MJD;
        QString   sourceFile;                 // file name the value came from
        QString   sourcePath;                 // ... and its full path
    };

    /// True when `path` looks like a time sidecar rather than a spectrum, i.e.
    /// its name ends in `_mjd` / `_bjd` / `_hjd` / `_jd` or carries one of those
    /// as its extension. Case-insensitive.
    static bool isTimeSidecar(const QString& path);

    /// Look for a time sidecar belonging to `spectrumPath`: the same base name
    /// plus `_mjd` (or `_bjd`, `_hjd`, `_jd`) and a `.txt`/`.dat` extension, or
    /// the scale as the extension itself. Trailing numeric segments of the base
    /// name are peeled off as well, so `..._930_01.txt` also finds
    /// `..._930_mjd.txt`. `pool` holds sidecars the user picked explicitly; they
    /// are matched first and may live in any directory, otherwise only the
    /// spectrum's own directory is searched. Matching ignores case. Returns
    /// nothing if no such file exists or none of them contains a number.
    static std::optional<DetectedTime> detectSidecarTime(
        const QString& spectrumPath, const QStringList& pool = {});

    AddSpectraDialog(std::vector<Entry> entries,
                     std::vector<std::shared_ptr<Instrument>> instruments,
                     QStringList sidecarPool = {},
                     QWidget* parent = nullptr);

    /// The entries with the user's choices written into their spectra.
    /// Only meaningful once exec() has returned Accepted.
    const std::vector<Entry>& entries() const { return _entries; }

protected:
    void accept() override;

private:
    void setupUi();
    void fillRow(int row, const Entry& entry);
    void populateModes(int row, const QString& instrumentId,
                       const QString& preselectKey);
    void copyFirstRowToAll();
    bool applyChoices();

    std::vector<Entry>                       _entries;
    std::vector<std::shared_ptr<Instrument>> _instruments;
    QStringList                              _sidecarPool;
    QTableWidget*                            _table = nullptr;
};
