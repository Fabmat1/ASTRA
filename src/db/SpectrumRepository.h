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
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);
    void loadSpectraBatch(std::vector<std::shared_ptr<Star>>& stars);

private:
    DBAccess& _db;
};

#endif // SPECTRUMREPOSITORY_H
