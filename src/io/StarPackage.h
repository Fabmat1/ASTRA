#ifndef STARPACKAGE_H
#define STARPACKAGE_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

class Star;
class Instrument;

// ─────────────────────────────────────────────────────────────────────────────
//  StarPackage - unified, version-tagged, compressed exchange format for ASTRA.
//
//  A single ".astra" file carries one or many stars together with every piece
//  of associated data (metadata, spectra + fits, photometry + lightcurves +
//  SED / LC fits, RV curve + points + fits) and the instruments they reference.
//
//  Cross-version policy:
//    • New optional FIELDS  → bump VERSION_MINOR. Old readers ignore unknown
//      fields; new readers default missing fields. Fully interoperable.
//    • BREAKING structural changes → bump VERSION_MAJOR. Readers refuse files
//      whose major version is newer than their own.
// ─────────────────────────────────────────────────────────────────────────────
class StarPackage {
  public:
    static constexpr quint16     VERSION_MAJOR  = 1;
    static constexpr quint16     VERSION_MINOR  = 0;
    static constexpr const char *FILE_EXTENSION = ".astra";

    struct ExportOptions {
        bool includeSpectra      = true;
        bool includeSpectralFits = true; // heavy model arrays inside each fit
        bool includePhotometry   = true;
        bool includeLightcurves  = true;
        bool includeSEDModels    = true;
        bool includeLCFits       = true;
        bool includeRV           = true;
        bool includeInstruments  = true;
        QString creatorNote; // free-form, stored in manifest
    };

    struct ImportResult {
        bool                                     success = false;
        QString                                  error;
        QStringList                              warnings;
        quint16                                  fileVersionMajor = 0;
        quint16                                  fileVersionMinor = 0;
        QString                                  creatorNote;
        QString                                  creatorApp;
        QString                                  createdAtIso;
        std::vector<std::shared_ptr<Star>>       stars;
        std::vector<std::shared_ptr<Instrument>> instruments;
    };

    // Supplies a full Instrument for a given instrument-id during export.
    // Return nullptr if unknown (it will simply be skipped). Optional.
    using InstrumentResolver =
        std::function<std::shared_ptr<Instrument>(const QString &instrumentId)>;

    // Progress reporting: (percent 0-100, human-readable phase label). Invoked
    // from the calling thread; keep the callback thread-safe (do not touch the
    // UI directly - marshal to the GUI thread). Optional.
    using ProgressFn = std::function<void(int percent, const QString &phase)>;

    // ── Write ────────────────────────────────────────────────────────────────
    static bool writeToFile(const QString                            &filepath,
                            const std::vector<std::shared_ptr<Star>> &stars,
                            const ExportOptions &opts, QString *error = nullptr,
                            const InstrumentResolver &resolver = {},
                            const ProgressFn         &progress = {});

    static QByteArray
    writeToBuffer(const std::vector<std::shared_ptr<Star>> &stars,
                  const ExportOptions &opts, QString *error = nullptr,
                  const InstrumentResolver &resolver = {},
                  const ProgressFn         &progress = {});

    // ── Read ─────────────────────────────────────────────────────────────────
    static ImportResult readFromFile(const QString    &filepath,
                                     const ProgressFn &progress = {});
    static ImportResult readFromBuffer(const QByteArray &bytes,
                                       const ProgressFn &progress = {});

    // ── Probing ────────────────────────────────────────────────────────────
    static bool isStarPackage(const QString &filepath);
    static bool peekVersion(const QString &filepath, quint16 &major,
                            quint16 &minor);
};

#endif // STARPACKAGE_H