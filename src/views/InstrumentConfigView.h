#ifndef INSTRUMENTCONFIGVIEW_H
#define INSTRUMENTCONFIGVIEW_H

#include <QWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include "models/Instrument.h"
#include "models/InstrumentMode.h"

class DatabaseManager;

class WorldMapWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WorldMapWidget(QWidget* parent = nullptr);

    void setInstruments(const std::vector<std::shared_ptr<Instrument>>& instruments);
    void setSelectedInstrumentId(const QString& id);

signals:
    void instrumentClicked(const QString& instrumentId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QPointF geoToWidget(double lat, double lon) const;
    QRectF  mapRect() const;
    void    drawGrid(QPainter& p);
    void    drawCoastlines(QPainter& p);
    void    drawInstruments(QPainter& p);

    std::vector<std::shared_ptr<Instrument>> _instruments;
    QString _selectedId;
    QString _hoveredId;

    static const QVector<QPolygonF>& coastlines();
};

class InstrumentConfigView : public QWidget
{
    Q_OBJECT
public:
    explicit InstrumentConfigView(DatabaseManager* dbManager, QWidget* parent = nullptr);

private slots:
    void onTreeSelectionChanged();
    void onMapClicked(const QString& instrumentId);
    void onAddInstrument();
    void onRemoveInstrument();
    void onAddMode();
    void onEditMode();
    void onRemoveMode();
    void onSave();
    void onRevert();
    void onRestoreDefaults();

private:
    void setupUi();
    void refreshList();
    void selectInstrument(const QString& id);
    void populateDetails(std::shared_ptr<Instrument> inst);
    void populateModeTable(std::shared_ptr<Instrument> inst);
    void clearDetails();
    void setDetailsEnabled(bool enabled);
    bool editModeDialog(InstrumentMode& mode, bool isNew);
    static QString modeSummary(const InstrumentMode& mode);

    DatabaseManager* _db;

    QTreeWidget*    _tree;
    WorldMapWidget* _map;

    QLineEdit*      _nameEdit;
    QLineEdit*      _fullNameEdit;
    QDoubleSpinBox* _latSpin;
    QDoubleSpinBox* _lonSpin;
    QDoubleSpinBox* _altSpin;
    QCheckBox*      _spaceCheck;
    QLabel*         _builtinLabel;
    QTableWidget*   _modeTable;

    QPushButton* _addBtn;
    QPushButton* _removeBtn;
    QPushButton* _addModeBtn;
    QPushButton* _editModeBtn;
    QPushButton* _removeModeBtn;
    QPushButton* _saveBtn;
    QPushButton* _revertBtn;
    QPushButton* _restoreBtn;

    QString _selectedId;
    std::shared_ptr<Instrument> _editingInstrument;
};

#endif // INSTRUMENTCONFIGVIEW_H