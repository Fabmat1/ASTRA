#ifndef SPECTRUMREPOSITORY_H
#define SPECTRUMREPOSITORY_H

#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

class DBAccess;
class Star;
class SpectrumIndexRow;
class Spectrum;
class SpectralFit;

/// One (instrument, mode) bucket of a star sample's spectra.
///
/// `instrumentId` empty means the "unlinked" bucket: rows written before the
/// explicit instrument link existed carry a null instrument_id and mode_key,
/// and they are counted and shown but cannot be configured, because there is
/// no mode to hang a region configuration off.
struct ModeSpectrumStat {
    QString instrumentId;
    QString modeKey;
    QString instrumentName;    ///< the free-text `instrument` column, for display
    int     count     = 0;     ///< spectra in this bucket
    int     starCount = 0;     ///< distinct stars contributing to it
};

class SpectrumRepository {
  public:
    explicit SpectrumRepository(DBAccess &db);

    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString &starId);

    // NEW: one query, project-wide, no blobs / no per-spectrum subqueries
    std::vector<SpectrumIndexRow> loadSpectraIndex(const QString &projectId);

    bool saveSpectrum(const QString &starId, std::shared_ptr<Spectrum> spectrum,
                      bool cascadeFits = true);
    bool saveSpectralFit(const QString &starId, const QString &spectrumId,
                         std::shared_ptr<SpectralFit> fit);
    bool deleteSpectrum(const QString &spectrumId);
    bool deleteSpectralFit(const QString &fitId);
    std::vector<std::shared_ptr<SpectralFit>>
         loadSpectralFits(const QString &spectrumId);
    void loadSpectraBatch(std::vector<std::shared_ptr<Star>> &stars);

    bool updateSpectrumFlag(const QString &spectrumId, bool flagged);
    bool updateSpectrumInstrument(const QString &spectrumId,
                                  const QString &instrument,
                                  const QString &instrumentId,
                                  const QString &modeKey);
    bool updateSpectralFitFlag(const QString &fitId, bool flagged);
    bool updateBestFit(const QString &spectrumId, const QString &bestFitId);

    /// How many spectra a star has, and how many of them carry a best fit.
    /// Two indexed COUNTs, so this is the cheap way to bring stars.n_spectra
    /// back in line after rows are added without the Star object in hand -
    /// loading the spectra themselves just to size the vector would pull
    /// every wavelength array with them.
    bool spectraCounts(const QString &starId, int *nSpectra,
                       int *nFitSpectra) const;

    /// Spectrum counts per (instrument_id, mode_key) over @p starIds, as ONE
    /// grouped query per chunk of star ids - the scope can be a whole
    /// catalogue, so loading the spectra themselves is not an option. Rows
    /// with no instrument link all merge into a single bucket whose
    /// instrumentId and modeKey are empty.
    std::vector<ModeSpectrumStat> spectraModeStats(const QStringList& starIds);

    /// Up to @p limit spectrum ids from one (instrument, mode) bucket of
    /// @p starIds, for picking a representative spectrum to plot. Pass empty
    /// strings for both keys to address the unlinked bucket.
    QStringList spectrumIdsForMode(const QStringList& starIds,
                                   const QString& instrumentId,
                                   const QString& modeKey, int limit);

    /// Spectra by id, WITHOUT their spectral fits. The preview plot only needs
    /// the wavelength and flux arrays, and pulling every fit blob for a handful
    /// of candidate spectra would dwarf that.
    std::vector<std::shared_ptr<Spectrum>>
    loadSpectraByIds(const QStringList& spectrumIds);

    // Archive-fetch support: all non-null origin_ids in a project (for
    // de-duplication), and removal of previously fetched rows before a
    // forced re-download.
    QSet<QString> originIdsForProject(const QString &projectId);
    bool deleteSpectraByOriginId(const QString &starId, const QString &originId);

  private:
    DBAccess &_db;
};

#endif // SPECTRUMREPOSITORY_H