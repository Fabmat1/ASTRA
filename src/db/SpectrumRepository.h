#ifndef SPECTRUMREPOSITORY_H
#define SPECTRUMREPOSITORY_H

#include <QSet>
#include <QString>
#include <memory>
#include <vector>

class DBAccess;
class Star;
class SpectrumIndexRow;
class Spectrum;
class SpectralFit;

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

    // Archive-fetch support: all non-null origin_ids in a project (for
    // de-duplication), and removal of previously fetched rows before a
    // forced re-download.
    QSet<QString> originIdsForProject(const QString &projectId);
    bool deleteSpectraByOriginId(const QString &starId, const QString &originId);

  private:
    DBAccess &_db;
};

#endif // SPECTRUMREPOSITORY_H