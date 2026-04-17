#pragma once

#include <QDialog>
#include <QVector>
#include <QColor>
#include <memory>
#include <vector>

class Star;
class QCustomPlot;
class QListWidget;
class QListWidgetItem;
class QRadioButton;
class QCheckBox;
class QPushButton;
class ApplicationController;

struct CMDTrack {
    QString name;
    QString filename;          // relative filename inside the tracks dir
    bool    plotByDefault = false;
    bool    enabled       = false;
    QColor  color;
    QVector<double> colors; // BP-RP
    QVector<double> mags;   // M_G
    bool    dataLoaded = false;
};

class CMDDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CMDDialog(std::shared_ptr<Star> star,
                       std::vector<std::shared_ptr<Star>> projectStars,
                       const QString& projectId = {},
                       QWidget* parent = nullptr);
    ~CMDDialog() override;

private slots:
    void onBackgroundModeChanged();
    void onAddTrack();
    void onRemoveTrack();
    void onRenameTrack();
    void onTrackItemChanged(QListWidgetItem* item);
    void onLabelTracksToggled(bool);

private:
    void setupUi();
    QWidget* buildControlPanel();

    void loadTrackConfig();
    void saveTrackConfig();
    bool loadTrackData(CMDTrack& track);
    QString tracksDir() const;
    QString tracksConfigPath() const;

    void loadProjectReferenceStars();
    void loadGaiaReferenceStars();
    void computeStarPoint();

    void updatePlot();
    void plotBackgroundAsDensity(const QVector<double>& xs,
                             const QVector<double>& ys);
    void applyPlotTheme();
    bool isDarkTheme() const;

    std::shared_ptr<Star> _star;
    std::vector<std::shared_ptr<Star>> _projectStars;
    QString               _projectId;

    QCustomPlot*  _plot            = nullptr;
    QRadioButton* _bgProjectRadio  = nullptr;
    QRadioButton* _bgGaiaRadio     = nullptr;
    QListWidget*  _tracksList      = nullptr;
    QPushButton*  _addTrackBtn     = nullptr;
    QPushButton*  _removeTrackBtn  = nullptr;
    QPushButton*  _renameTrackBtn  = nullptr;
    QCheckBox*    _labelTracksCb   = nullptr;

    QVector<double> _projBpRp, _projGabs;
    QVector<double> _gaiaBpRp, _gaiaGabs;
    bool _gaiaLoaded = false;
    bool _projLoaded = false;

    std::vector<CMDTrack> _tracks;

    enum class StarMode { Skip, Normal, UpperLimit };
    StarMode _starMode = StarMode::Skip;
    double _starColor = 0.0, _starColorErr = 0.0;
    double _starMag   = 0.0, _starMagErr   = 0.0;
};