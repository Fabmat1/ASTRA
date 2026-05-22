#pragma once

#include <QDialog>
#include <QStringList>
#include <memory>
#include <vector>

#include "models/Photometry.h"
#include "models/Time.h"

class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QTableWidget;
class QPlainTextEdit;

class Star;
class DatabaseManager;
class Instrument;

class ImportLightcurveDialog : public QDialog
{
    Q_OBJECT
public:
    ImportLightcurveDialog(std::shared_ptr<Star>  star,
                           DatabaseManager*       dbm,
                           QWidget*               parent = nullptr);

    // Result after exec() returned Accepted.
    bool                                 wasImported()   const { return _imported; }
    QString                              sourceKey()     const { return _sourceKey; }
    TimeScale                            timeScale()     const { return _timeScale; }
    const std::vector<LightcurvePoint>&  importedPoints() const { return _points; }

private slots:
    void onBrowse();
    void onInstrumentChanged(int);
    void onAddCut();
    void onRemoveCut();
    void onImport();

private:
    void buildUi();
    void populateInstruments();
    void populateModes();
    void reloadFile();
    void rebuildColumnCombos();
    void updateCutColumnCombos();

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;

    // UI
    QLineEdit*    _filePath        = nullptr;
    QPushButton*  _browseBtn       = nullptr;
    QComboBox*    _instrumentCombo = nullptr;
    QComboBox*    _modeCombo       = nullptr;
    QComboBox*    _timeScaleCombo  = nullptr;
    QComboBox*    _timeColCombo    = nullptr;
    QComboBox*    _fluxColCombo    = nullptr;
    QComboBox*    _fluxErrColCombo = nullptr;
    QComboBox*    _filterColCombo  = nullptr;
    QLineEdit*    _defaultFilter   = nullptr;
    QTableWidget* _cutsTable       = nullptr;
    QPushButton*  _addCutBtn       = nullptr;
    QPushButton*  _removeCutBtn    = nullptr;
    QLabel*       _statusLabel     = nullptr;
    QPlainTextEdit* _previewView   = nullptr;
    QPushButton*  _importBtn       = nullptr;

    // File state
    QStringList         _headers;         // canonical, possibly synthetic
    QList<QStringList>  _dataRows;        // body rows (skip header if any)
    bool                _hasHeader = false;
    QChar               _delim     = QChar(',');

    // Results
    bool                          _imported = false;
    QString                       _sourceKey;
    TimeScale                     _timeScale = TimeScale::MJD;
    std::vector<LightcurvePoint>  _points;
};