#ifndef ADDSTARDIALOG_H
#define ADDSTARDIALOG_H

#include <QDialog>
#include <memory>
#include <limits>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;
class QNetworkAccessManager;
class Star;

/**
 * Dialog to add a single star to a project. Fields can be filled manually
 * or auto-resolved from SIMBAD + Gaia DR3 (VizieR) starting from any of:
 * Gaia DR3 source_id, TIC, JName, or RA/Dec.
 *
 * Empty numeric fields are stored as NaN on the resulting Star.
 */
class AddStarDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddStarDialog(QWidget* parent = nullptr);
    ~AddStarDialog() override;

    /// Build a fresh Star instance from the current dialog fields.
    std::shared_ptr<Star> buildStar() const;

    /// Whether the user wants a SIMBAD bibliography query queued.
    bool shouldQueryBibliography() const;

private slots:
    void onResolve();

private:
    struct ResolvedIds {
        QString sourceId;
        QString tic;
        QString mainId;
        double  ra  = std::numeric_limits<double>::quiet_NaN();
        double  dec = std::numeric_limits<double>::quiet_NaN();
    };

    void setupUi();
    void setStatus(const QString& msg, bool isError = false);
    void setBusy(bool busy);

    // Synchronous query helpers (use QEventLoop with a timeout).
    bool resolveSimbad(const QString& queryStr, ResolvedIds& out, QString& err);
    bool fetchGaiaDR3(const QString& sourceId, QString& err);

    // Misc helpers
    static double  parseDoubleOrNaN(QLineEdit* le);
    static void    setDoubleIfEmpty(QLineEdit* le, double v);
    static void    setTextIfEmpty(QLineEdit* le, const QString& v);
    static QString jnameFromCoords(double raDeg, double decDeg);
    static bool    parseJnameToCoords(const QString& jname, double& raDeg, double& decDeg);
    static QString digitsOnly(const QString& s);

    // Identifier widgets
    QLineEdit *_aliasEdit;
    QLineEdit *_sourceIdEdit;
    QLineEdit *_ticEdit;
    QLineEdit *_jnameEdit;
    QLineEdit *_raEdit;
    QLineEdit *_decEdit;

    // Astrometry
    QLineEdit *_pmraEdit,  *_ePmraEdit;
    QLineEdit *_pmdecEdit, *_ePmdecEdit;
    QLineEdit *_plxEdit,   *_ePlxEdit;
    QLineEdit *_pmraPmdecCorrEdit;
    QLineEdit *_plxPmraCorrEdit;
    QLineEdit *_plxPmdecCorrEdit;

    // Gaia photometry
    QLineEdit *_gmagEdit, *_eGmagEdit;
    QLineEdit *_bpEdit,   *_eBpEdit;
    QLineEdit *_rpEdit,   *_eRpEdit;
    QLineEdit *_bpRpEdit;

    // Spectroscopy
    QLineEdit *_specClassEdit;
    QLineEdit *_teffEdit, *_eTeffEdit;
    QLineEdit *_loggEdit, *_eLoggEdit;
    QLineEdit *_heEdit,   *_eHeEdit;

    // Controls
    QPushButton  *_resolveBtn;
    QCheckBox    *_bibliographyCheck;
    QLabel       *_statusLabel;
    QProgressBar *_busyIndicator;

    QNetworkAccessManager *_network;
};

#endif // ADDSTARDIALOG_H