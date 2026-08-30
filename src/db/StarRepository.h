#ifndef STARREPOSITORY_H
#define STARREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Project;
class Star;

class StarRepository {
public:
    explicit StarRepository(DBAccess& db);

    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    // Reassigns the given stars to another project. All attached data
    // (spectra, photometry, SEDs, RV, periodograms) is linked by star_id and
    // therefore moves with the star automatically. Runs in one transaction.
    bool moveStarsToProject(const std::vector<QString>& starIds, const QString& targetProjectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& starId);
    size_t getStarCountForProject(const QString& projectId);
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);
    /// Writes only n_spectra / n_fit_spectra. Deliberately narrow: updateStarRow
    /// rewrites teff, logg, he and every fitted quantity from the in-memory
    /// star, which is not something an import of a few spectra should be
    /// deciding.
    bool updateSpectraCounts(const QString& starId, int nSpectra,
                             int nFitSpectra);

    bool updateStarRow(const QString& projectId, std::shared_ptr<Star> star);
    QString findMatchingStarId(const QString& projectId, const QString& sourceId, const QString& alias, const QString& tic, const QString& jname, double ra, double dec, double toleranceArcsec = 2.0);

private:
    DBAccess& _db;
};

#endif // STARREPOSITORY_H
