#ifndef SPECTRUMREPOSITORY_H
#define SPECTRUMREPOSITORY_H

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

  private:
    DBAccess &_db;
};

#endif // SPECTRUMREPOSITORY_H