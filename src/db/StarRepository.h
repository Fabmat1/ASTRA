#ifndef STARREPOSITORY_H
#define STARREPOSITORY_H

#include <functional>
#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Project;
class Star;

class StarRepository {
public:
    /// Outcome of copyStarsToProject(). newStarIds is parallel to the ids
    /// that were asked for (empty where a star did not exist).
    struct CopyResult {
        bool                 ok = false;
        std::vector<QString> newStarIds;
        QString              error;
    };
    using CopyProgress = std::function<void(int percent, const QString& phase)>;

    explicit StarRepository(DBAccess& db);

    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    // Reassigns the given stars to another project. All attached data
    // (spectra, photometry, SEDs, RV, periodograms) is linked by star_id and
    // therefore moves with the star automatically. Runs in one transaction.
    bool moveStarsToProject(const std::vector<QString>& starIds, const QString& targetProjectId);
    // Duplicates the given stars into another project with fresh ids: every
    // row that hangs off them (spectra and fits, photometry with its points,
    // lightcurves, LC fits and SED models, RV curves with points and fits,
    // periodograms) and every data file those rows point at, so the copies
    // share nothing with the originals. Works row-to-row in SQL and file-to-
    // file on disk, never loading a star into memory. Runs in one transaction;
    // on failure nothing is left behind.
    CopyResult copyStarsToProject(const std::vector<QString>& starIds,
                                  const QString& targetProjectId,
                                  const CopyProgress& progress = {});
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
