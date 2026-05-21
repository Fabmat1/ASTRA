#ifndef SPECTRUMREPOSITORY_H
#define SPECTRUMREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Star;
class Spectrum;
class SpectralFit;

class SpectrumRepository {
public:
    explicit SpectrumRepository(DBAccess& db);

    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString& starId);
    bool saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum);
    bool saveSpectralFit(const QString& starId, const QString& spectrumId, std::shared_ptr<SpectralFit> fit);
    bool deleteSpectrum(const QString& spectrumId);
    bool deleteSpectralFit(const QString& fitId);
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);
    void loadSpectraBatch(std::vector<std::shared_ptr<Star>>& stars);

    bool updateSpectrumFlag(const QString& spectrumId, bool flagged);
    bool updateSpectralFitFlag(const QString& fitId, bool flagged);
    bool updateBestFit(const QString& spectrumId, const QString& bestFitId);

private:
    DBAccess& _db;
};

#endif // SPECTRUMREPOSITORY_H
