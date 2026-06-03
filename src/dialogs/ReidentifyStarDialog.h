#pragma once

#include <QDialog>
#include <QString>
#include <limits>
#include <memory>
#include <vector>

class Star;
class DatabaseManager;

class QLineEdit;
class QDoubleSpinBox;
class QTableWidget;
class QPushButton;
class QLabel;
class QProgressBar;
class QCheckBox;
class QNetworkAccessManager;

// Small dialog to re-point a Star at a different nearby Gaia source
// (fixes confused / blended sources separated by < a couple arcsec) and
// re-query its identity + Gaia astrometry/photometry + bibliography.
class ReidentifyStarDialog : public QDialog {
    Q_OBJECT
  public:
    ReidentifyStarDialog(std::shared_ptr<Star> star, DatabaseManager *dbm,
                         QString projectId, QWidget *parent = nullptr);
    ~ReidentifyStarDialog() override;

  private slots:
    void onSearch();
    void onManualResolve();
    void onSelectionChanged();
    void onApply();

  private:
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    struct Candidate {
        QString sourceId;
        QString alias; // optional resolved main id
        double  ra        = kNaN;
        double  dec       = kNaN;
        double  gmag      = kNaN;
        double  sepArcsec = kNaN;
    };

    struct GaiaData {
        double ra = kNaN, dec = kNaN;
        double pmra = kNaN, pmdec = kNaN, e_pmra = kNaN, e_pmdec = kNaN;
        double plx = kNaN, e_plx = kNaN;
        double gmag = kNaN, e_gmag = kNaN;
        double bp = kNaN, e_bp = kNaN, rp = kNaN, e_rp = kNaN, bp_rp = kNaN;
        double pmra_pmdec_corr = kNaN, plx_pmra_corr = kNaN,
               plx_pmdec_corr = kNaN;
        bool   ok             = false;
    };

    struct ResolvedIds {
        QString mainId, sourceId, tic;
        double  ra = kNaN, dec = kNaN;
    };

    void setupUi();
    void setBusy(bool busy);
    void setStatus(const QString &msg, bool isError = false);
    void populateTable();

    // Network queries (synchronous, event-loop driven - matches AddStarDialog).
    bool coneSearchGaia(double ra, double dec, double radiusArcsec,
                        std::vector<Candidate> &out, QString &err);
    bool resolveSimbad(const QString &queryStr, ResolvedIds &out, QString &err);
    bool fetchGaiaFull(const QString &sourceId, GaiaData &out, QString &err);
    bool fetchBibcodes(const QString &sourceId, std::vector<QString> &out,
                       QString &err);

    std::shared_ptr<Star> _star;
    DatabaseManager      *_dbm = nullptr;
    QString               _projectId;

    QNetworkAccessManager *_network = nullptr;

    // Center of the search (degrees). Starts at the star's current position.
    double _centerRa  = kNaN;
    double _centerDec = kNaN;

    std::vector<Candidate> _candidates;

    QLabel         *_headerLabel       = nullptr;
    QDoubleSpinBox *_radiusSpin        = nullptr;
    QPushButton    *_searchBtn         = nullptr;
    QTableWidget   *_table             = nullptr;
    QLineEdit      *_manualEdit        = nullptr;
    QPushButton    *_manualBtn         = nullptr;
    QCheckBox      *_bibliographyCheck = nullptr;
    QProgressBar   *_busy              = nullptr;
    QLabel         *_status            = nullptr;
    QPushButton    *_applyBtn          = nullptr;
};