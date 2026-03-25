#include "InstrumentConfigView.h"
#include "utils/DatabaseManager.h"
#include "utils/CoastlineData.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QToolTip>
#include <QUuid>
#include <QtMath>

// Small Helper

static bool naturalLessThan(const QString& a, const QString& b)
{
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        QChar ca = a[i], cb = b[j];
        if (ca.isDigit() && cb.isDigit()) {
            // Extract full number from each string
            int numStartA = i, numStartB = j;
            while (i < a.size() && a[i].isDigit()) ++i;
            while (j < b.size() && b[j].isDigit()) ++j;
            qulonglong na = QStringView(a).mid(numStartA, i - numStartA).toULongLong();
            qulonglong nb = QStringView(b).mid(numStartB, j - numStartB).toULongLong();
            if (na != nb) return na < nb;
        } else {
            if (ca.toLower() != cb.toLower())
                return ca.toLower() < cb.toLower();
            ++i;
            ++j;
        }
    }
    return a.size() < b.size();
}


// ═════════════════════════════════════════════════════════════════════════════
//  WorldMapWidget
// ═════════════════════════════════════════════════════════════════════════════

WorldMapWidget::WorldMapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(400, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void WorldMapWidget::setInstruments(
    const std::vector<std::shared_ptr<Instrument>>& instruments)
{
    _instruments = instruments;
    update();
}

void WorldMapWidget::setSelectedInstrumentId(const QString& id)
{
    if (_selectedId != id) {
        _selectedId = id;
        update();
    }
}

const QVector<QPolygonF>& WorldMapWidget::coastlines()
{
    static const QVector<QPolygonF> s_coastlines = loadCoastlinePolygons();
    return s_coastlines;
}

QRectF WorldMapWidget::mapRect() const
{
    const double pad = 10.0;
    double aw = width()  - 2 * pad;
    double ah = height() - 2 * pad;
    double mw, mh;
    if (aw / ah > 2.0) {
        mh = ah;
        mw = mh * 2.0;
    } else {
        mw = aw;
        mh = mw / 2.0;
    }
    return QRectF((width() - mw) / 2.0, (height() - mh) / 2.0, mw, mh);
}

QPointF WorldMapWidget::geoToWidget(double lat, double lon) const
{
    QRectF mr = mapRect();
    double x = mr.left() + (lon + 180.0) / 360.0 * mr.width();
    double y = mr.top()  + (90.0 - lat)  / 180.0 * mr.height();
    return {x, y};
}

void WorldMapWidget::drawGrid(QPainter& p)
{
    QPen gridPen(QColor(255, 255, 255, 25), 0.5);
    p.setPen(gridPen);

    for (int lat = -60; lat <= 60; lat += 30) {
        p.drawLine(geoToWidget(lat, -180), geoToWidget(lat, 180));
    }
    for (int lon = -150; lon <= 150; lon += 30) {
        p.drawLine(geoToWidget(90, lon), geoToWidget(-90, lon));
    }

    QPen eqPen(QColor(255, 255, 255, 50), 0.5, Qt::DashLine);
    p.setPen(eqPen);
    p.drawLine(geoToWidget(0, -180), geoToWidget(0, 180));
}

void WorldMapWidget::drawCoastlines(QPainter& p)
{
    p.setPen(QPen(QColor(80, 130, 80), 1.0));
    p.setBrush(QColor(55, 85, 55));

    for (const auto& geoPoly : coastlines()) {
        QPolygonF screenPoly;
        screenPoly.reserve(geoPoly.size());
        for (const auto& pt : geoPoly)
            screenPoly << geoToWidget(pt.y(), pt.x());
        p.drawPolygon(screenPoly);
    }
}

void WorldMapWidget::drawInstruments(QPainter& p)
{
    QFont labelFont = font();
    labelFont.setPointSize(8);
    p.setFont(labelFont);

    for (const auto& inst : _instruments) {
        if (inst->isSpaceBased()) continue;

        QPointF pos = geoToWidget(inst->getLatitude(), inst->getLongitude());
        bool selected = (inst->getId() == _selectedId);
        bool hovered  = (inst->getId() == _hoveredId);

        double radius = selected ? 6.0 : (hovered ? 5.0 : 3.5);
        QColor fill   = selected ? QColor(255, 200, 50)
                      : hovered  ? QColor(255, 160, 40)
                                 : QColor(220, 220, 230);

        p.setPen(QPen(Qt::white, selected ? 2.0 : 1.0));
        p.setBrush(fill);
        p.drawEllipse(pos, radius, radius);

        QColor textColor = selected ? QColor(255, 220, 80)
                                    : QColor(200, 200, 210);
        p.setPen(QColor(0, 0, 0, 140));
        p.drawText(pos + QPointF(radius + 5, 4) + QPointF(1, 1), inst->getName());
        p.setPen(textColor);
        p.drawText(pos + QPointF(radius + 5, 4), inst->getName());
    }
}

void WorldMapWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), QColor(18, 18, 24));

    QRectF mr = mapRect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(22, 38, 56));
    p.drawRoundedRect(mr, 4, 4);

    drawCoastlines(p);
    drawGrid(p);
    drawInstruments(p);

    p.setPen(QPen(QColor(60, 60, 70), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(mr, 4, 4);
}

void WorldMapWidget::mousePressEvent(QMouseEvent* e)
{
    const double hitRadius = 12.0;
    double bestDist = hitRadius;
    QString bestId;

    for (const auto& inst : _instruments) {
        if (inst->isSpaceBased()) continue;
        double d = QLineF(e->position(),
                          geoToWidget(inst->getLatitude(), inst->getLongitude()))
                       .length();
        if (d < bestDist) {
            bestDist = d;
            bestId = inst->getId();
        }
    }

    if (!bestId.isEmpty())
        emit instrumentClicked(bestId);
}

void WorldMapWidget::mouseMoveEvent(QMouseEvent* e)
{
    const double hitRadius = 12.0;
    double bestDist = hitRadius;
    QString bestId;

    for (const auto& inst : _instruments) {
        if (inst->isSpaceBased()) continue;
        double d = QLineF(e->position(),
                          geoToWidget(inst->getLatitude(), inst->getLongitude()))
                       .length();
        if (d < bestDist) {
            bestDist = d;
            bestId = inst->getId();
        }
    }

    if (bestId != _hoveredId) {
        _hoveredId = bestId;
        update();
    }

    if (!bestId.isEmpty()) {
        auto it = std::find_if(_instruments.begin(), _instruments.end(),
            [&](const auto& i){ return i->getId() == bestId; });
        if (it != _instruments.end()) {
            auto& inst = *it;
            QToolTip::showText(e->globalPosition().toPoint(),
                QString("%1\n%2\nLat: %3  Lon: %4  Alt: %5 m")
                    .arg(inst->getName(), inst->getFullName())
                    .arg(inst->getLatitude(), 0, 'f', 2)
                    .arg(inst->getLongitude(), 0, 'f', 2)
                    .arg(inst->getAltitude(), 0, 'f', 0),
                this);
        }
    } else {
        QToolTip::hideText();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  InstrumentConfigView
// ═════════════════════════════════════════════════════════════════════════════

InstrumentConfigView::InstrumentConfigView(DatabaseManager* dbManager,
                                           QWidget* parent)
    : QWidget(parent, Qt::Window)
    , _db(dbManager)
{
    setWindowTitle(tr("Instrument Configuration"));
    setMinimumSize(1050, 680);
    setupUi();
    refreshList();
    clearDetails();
}

void InstrumentConfigView::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    auto* toolbar = new QHBoxLayout;
    _addBtn     = new QPushButton(tr("+ Instrument"));
    _removeBtn  = new QPushButton(tr("- Instrument"));
    _restoreBtn = new QPushButton(tr("Restore Defaults"));
    _saveBtn    = new QPushButton(tr("Save"));
    _revertBtn  = new QPushButton(tr("Revert"));

    toolbar->addWidget(_addBtn);
    toolbar->addWidget(_removeBtn);
    toolbar->addSpacing(12);
    toolbar->addWidget(_restoreBtn);
    toolbar->addStretch();
    toolbar->addWidget(_saveBtn);
    toolbar->addWidget(_revertBtn);
    mainLayout->addLayout(toolbar);

    connect(_addBtn,     &QPushButton::clicked, this, &InstrumentConfigView::onAddInstrument);
    connect(_removeBtn,  &QPushButton::clicked, this, &InstrumentConfigView::onRemoveInstrument);
    connect(_restoreBtn, &QPushButton::clicked, this, &InstrumentConfigView::onRestoreDefaults);
    connect(_saveBtn,    &QPushButton::clicked, this, &InstrumentConfigView::onSave);
    connect(_revertBtn,  &QPushButton::clicked, this, &InstrumentConfigView::onRevert);

    auto* hSplitter = new QSplitter(Qt::Horizontal);
    mainLayout->addWidget(hSplitter, 1);

    _tree = new QTreeWidget;
    _tree->setHeaderLabel(tr("Instruments"));
    _tree->setMinimumWidth(180);
    _tree->setIndentation(16);
    hSplitter->addWidget(_tree);
    connect(_tree, &QTreeWidget::itemSelectionChanged,
            this,  &InstrumentConfigView::onTreeSelectionChanged);

    auto* vSplitter = new QSplitter(Qt::Vertical);
    hSplitter->addWidget(vSplitter);

    _map = new WorldMapWidget;
    vSplitter->addWidget(_map);
    connect(_map, &WorldMapWidget::instrumentClicked,
            this, &InstrumentConfigView::onMapClicked);

    auto* detailWidget = new QWidget;
    auto* detailLayout = new QHBoxLayout(detailWidget);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    vSplitter->addWidget(detailWidget);

    // Properties group
    auto* propsGroup = new QGroupBox(tr("Properties"));
    auto* propsForm  = new QFormLayout(propsGroup);

    _nameEdit     = new QLineEdit;
    _fullNameEdit = new QLineEdit;
    _latSpin      = new QDoubleSpinBox;
    _lonSpin      = new QDoubleSpinBox;
    _altSpin      = new QDoubleSpinBox;
    _spaceCheck   = new QCheckBox(tr("Space-based"));
    _builtinLabel = new QLabel;

    _latSpin->setRange(-90, 90);
    _latSpin->setDecimals(4);
    _latSpin->setSuffix(QStringLiteral("\u00B0"));
    _lonSpin->setRange(-180, 180);
    _lonSpin->setDecimals(4);
    _lonSpin->setSuffix(QStringLiteral("\u00B0"));
    _altSpin->setRange(-500, 100000);
    _altSpin->setDecimals(1);
    _altSpin->setSuffix(tr(" m"));

    propsForm->addRow(tr("Name:"),      _nameEdit);
    propsForm->addRow(tr("Full Name:"), _fullNameEdit);

    auto* coordRow = new QHBoxLayout;
    coordRow->addWidget(new QLabel(tr("Lat:")));
    coordRow->addWidget(_latSpin);
    coordRow->addWidget(new QLabel(tr("Lon:")));
    coordRow->addWidget(_lonSpin);
    propsForm->addRow(coordRow);

    propsForm->addRow(tr("Altitude:"), _altSpin);
    propsForm->addRow(_spaceCheck);
    propsForm->addRow(_builtinLabel);

    connect(_spaceCheck, &QCheckBox::toggled, this, [this](bool checked) {
        _latSpin->setEnabled(!checked);
        _lonSpin->setEnabled(!checked);
        _altSpin->setEnabled(!checked);
    });

    detailLayout->addWidget(propsGroup);

    // Modes group
    auto* modesGroup  = new QGroupBox(tr("Modes"));
    auto* modesLayout = new QVBoxLayout(modesGroup);

    _modeTable = new QTableWidget(0, 4);
    _modeTable->setHorizontalHeaderLabels(
        {tr("Key"), tr("Name"), tr("Type"), tr("Summary")});
    _modeTable->horizontalHeader()->setStretchLastSection(true);
    _modeTable->verticalHeader()->setVisible(false);
    _modeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _modeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    _modeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modesLayout->addWidget(_modeTable);

    auto* modeButtons = new QHBoxLayout;
    _addModeBtn    = new QPushButton(tr("Add"));
    _editModeBtn   = new QPushButton(tr("Edit"));
    _removeModeBtn = new QPushButton(tr("Remove"));
    modeButtons->addWidget(_addModeBtn);
    modeButtons->addWidget(_editModeBtn);
    modeButtons->addWidget(_removeModeBtn);
    modeButtons->addStretch();
    modesLayout->addLayout(modeButtons);

    connect(_addModeBtn,    &QPushButton::clicked, this, &InstrumentConfigView::onAddMode);
    connect(_editModeBtn,   &QPushButton::clicked, this, &InstrumentConfigView::onEditMode);
    connect(_removeModeBtn, &QPushButton::clicked, this, &InstrumentConfigView::onRemoveMode);
    connect(_modeTable, &QTableWidget::cellDoubleClicked,
            this, &InstrumentConfigView::onEditMode);

    detailLayout->addWidget(modesGroup);

    vSplitter->setStretchFactor(0, 3);
    vSplitter->setStretchFactor(1, 2);
    hSplitter->setStretchFactor(0, 0);
    hSplitter->setStretchFactor(1, 1);
    hSplitter->setSizes({200, 800});
}

// ── List management ─────────────────────────────────────────────────────────

void InstrumentConfigView::refreshList()
{
    _tree->blockSignals(true);
    _tree->clear();

    auto instruments = _db->getAllInstruments();
    std::sort(instruments.begin(), instruments.end(),
        [](const auto& a, const auto& b) { return a->getName() < b->getName(); });

    for (const auto& inst : instruments) {
        auto* item = new QTreeWidgetItem(_tree);
        QString label = inst->getName();
        if (inst->isSpaceBased())
            label += tr(" (space)");
        item->setText(0, label);
        item->setData(0, Qt::UserRole, inst->getId());

        auto modes = inst->modes();
        std::sort(modes.begin(), modes.end(),
            [](const InstrumentMode& a, const InstrumentMode& b) {
                return naturalLessThan(a.key(), b.key());
            });

        for (const auto& mode : modes) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, mode.displayName().isEmpty()
                              ? mode.key() : mode.displayName());
            child->setData(0, Qt::UserRole,     inst->getId());
            child->setData(0, Qt::UserRole + 1, mode.key());
        }
    }

    _tree->blockSignals(false);
    _map->setInstruments(instruments);
}

void InstrumentConfigView::selectInstrument(const QString& id)
{
    _selectedId = id;

    auto inst = _db->getInstrumentById(id);
    if (inst) {
        _editingInstrument = std::make_shared<Instrument>(*inst);
        populateDetails(_editingInstrument);
        populateModeTable(_editingInstrument);
        setDetailsEnabled(true);
    } else {
        _editingInstrument.reset();
        clearDetails();
    }

    _map->setSelectedInstrumentId(id);

    _tree->blockSignals(true);
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) {
        auto* item = _tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == id) {
            _tree->setCurrentItem(item);
            item->setExpanded(true);
            break;
        }
    }
    _tree->blockSignals(false);
}

// ── Detail population ───────────────────────────────────────────────────────

void InstrumentConfigView::populateDetails(std::shared_ptr<Instrument> inst)
{
    _nameEdit->setText(inst->getName());
    _fullNameEdit->setText(inst->getFullName());
    _latSpin->setValue(inst->getLatitude());
    _lonSpin->setValue(inst->getLongitude());
    _altSpin->setValue(inst->getAltitude());
    _spaceCheck->setChecked(inst->isSpaceBased());

    _builtinLabel->setText(inst->isBuiltin()
        ? tr("(Built-in instrument)")
        : tr("(User-defined instrument)"));
}

void InstrumentConfigView::populateModeTable(std::shared_ptr<Instrument> inst)
{
    _modeTable->setRowCount(0);

    auto modes = inst->modes();
    std::sort(modes.begin(), modes.end(),
        [](const InstrumentMode& a, const InstrumentMode& b) {
            return naturalLessThan(a.key(), b.key());
        });

    for (const auto& mode : modes) {
        int row = _modeTable->rowCount();
        _modeTable->insertRow(row);
        _modeTable->setItem(row, 0, new QTableWidgetItem(mode.key()));
        _modeTable->setItem(row, 1, new QTableWidgetItem(mode.displayName()));
        _modeTable->setItem(row, 2, new QTableWidgetItem(
            InstrumentMode::dataTypeToString(mode.dataType())));
        _modeTable->setItem(row, 3, new QTableWidgetItem(modeSummary(mode)));
    }

    _modeTable->resizeColumnsToContents();
}

void InstrumentConfigView::clearDetails()
{
    _nameEdit->clear();
    _fullNameEdit->clear();
    _latSpin->setValue(0);
    _lonSpin->setValue(0);
    _altSpin->setValue(0);
    _spaceCheck->setChecked(false);
    _builtinLabel->clear();
    _modeTable->setRowCount(0);
    _editingInstrument.reset();
    setDetailsEnabled(false);
}

void InstrumentConfigView::setDetailsEnabled(bool enabled)
{
    _nameEdit->setEnabled(enabled);
    _fullNameEdit->setEnabled(enabled);
    _latSpin->setEnabled(enabled && !_spaceCheck->isChecked());
    _lonSpin->setEnabled(enabled && !_spaceCheck->isChecked());
    _altSpin->setEnabled(enabled && !_spaceCheck->isChecked());
    _spaceCheck->setEnabled(enabled);
    _modeTable->setEnabled(enabled);
    _addModeBtn->setEnabled(enabled);
    _editModeBtn->setEnabled(enabled);
    _removeModeBtn->setEnabled(enabled);
    _saveBtn->setEnabled(enabled);
    _revertBtn->setEnabled(enabled);
    _removeBtn->setEnabled(enabled);
}

QString InstrumentConfigView::modeSummary(const InstrumentMode& mode)
{
    QStringList parts;
    if (mode.hasSpectralProperties()) {
        const auto& sp = mode.spectral();
        double midWl = (sp.wavelengthMin + sp.wavelengthMax) / 2.0;
        double midR = sp.resolution.at(midWl);
        if (midR > 0)
            parts << QString("R \u2248 %1").arg(midR, 0, 'f', 0);
        if (sp.wavelengthMin > 0 && sp.wavelengthMax > 0)
            parts << QString("%1\u2013%2 \u00C5")
                     .arg(sp.wavelengthMin, 0, 'f', 0)
                     .arg(sp.wavelengthMax, 0, 'f', 0);
    }
    if (mode.hasPhotometricProperties()) {
        const auto& pp = mode.photometric();
        if (pp.cadence > 0)
            parts << QString("%1 s").arg(pp.cadence, 0, 'f', 0);
        if (!pp.filters.isEmpty())
            parts << pp.filters.join(",");
    }
    return parts.join(", ");
}

// ── Slots ───────────────────────────────────────────────────────────────────

void InstrumentConfigView::onTreeSelectionChanged()
{
    auto* item = _tree->currentItem();
    if (!item) return;

    QString instId = item->data(0, Qt::UserRole).toString();
    if (instId != _selectedId)
        selectInstrument(instId);

    QString modeKey = item->data(0, Qt::UserRole + 1).toString();
    if (!modeKey.isEmpty()) {
        for (int row = 0; row < _modeTable->rowCount(); ++row) {
            if (_modeTable->item(row, 0)->text() == modeKey) {
                _modeTable->selectRow(row);
                break;
            }
        }
    }
}

void InstrumentConfigView::onMapClicked(const QString& instrumentId)
{
    selectInstrument(instrumentId);
}

void InstrumentConfigView::onAddInstrument()
{
    auto inst = std::make_shared<Instrument>(
        tr("New Instrument"), 0.0, 0.0, 0.0);
    inst->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

    if (!_db->saveInstrument(inst)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to create instrument."));
        return;
    }

    refreshList();
    selectInstrument(inst->getId());
    _nameEdit->setFocus();
    _nameEdit->selectAll();
}

void InstrumentConfigView::onRemoveInstrument()
{
    if (_selectedId.isEmpty()) return;

    auto inst = _db->getInstrumentById(_selectedId);
    if (!inst) return;

    if (inst->isBuiltin()) {
        auto answer = QMessageBox::question(this, tr("Remove Built-in"),
            tr("Remove built-in instrument \"%1\"?\n"
               "It will be restored on next fresh database creation.")
                .arg(inst->getName()),
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    } else {
        auto answer = QMessageBox::question(this, tr("Remove Instrument"),
            tr("Remove \"%1\" and all its modes?").arg(inst->getName()),
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    _db->deleteInstrument(_selectedId);
    _selectedId.clear();
    clearDetails();
    refreshList();
}

void InstrumentConfigView::onAddMode()
{
    if (!_editingInstrument) return;

    InstrumentMode mode;
    if (editModeDialog(mode, true)) {
        _editingInstrument->addMode(mode);
        populateModeTable(_editingInstrument);
    }
}

void InstrumentConfigView::onEditMode()
{
    if (!_editingInstrument) return;
    int row = _modeTable->currentRow();
    if (row < 0) return;

    QString key = _modeTable->item(row, 0)->text();
    const InstrumentMode* existing = _editingInstrument->mode(key);
    if (!existing) return;

    InstrumentMode mode = *existing;
    QString oldKey = key;

    if (editModeDialog(mode, false)) {
        if (mode.key() != oldKey)
            _editingInstrument->removeMode(oldKey);
        _editingInstrument->addMode(mode);
        populateModeTable(_editingInstrument);
    }
}

void InstrumentConfigView::onRemoveMode()
{
    if (!_editingInstrument) return;
    int row = _modeTable->currentRow();
    if (row < 0) return;

    QString key = _modeTable->item(row, 0)->text();

    auto answer = QMessageBox::question(this, tr("Remove Mode"),
        tr("Remove mode \"%1\"?").arg(key),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    _editingInstrument->removeMode(key);
    populateModeTable(_editingInstrument);
}

void InstrumentConfigView::onSave()
{
    if (!_editingInstrument) return;

    QString newName = _nameEdit->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this, tr("Validation"),
                             tr("Instrument name cannot be empty."));
        _nameEdit->setFocus();
        return;
    }

    _editingInstrument->setName(newName);
    _editingInstrument->setFullName(_fullNameEdit->text().trimmed());
    _editingInstrument->setLatitude(_latSpin->value());
    _editingInstrument->setLongitude(_lonSpin->value());
    _editingInstrument->setAltitude(_altSpin->value());
    _editingInstrument->setSpaceBased(_spaceCheck->isChecked());

    if (_db->saveInstrument(_editingInstrument)) {
        QString id = _editingInstrument->getId();
        refreshList();
        selectInstrument(id);
    } else {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save instrument."));
    }
}

void InstrumentConfigView::onRevert()
{
    if (!_selectedId.isEmpty())
        selectInstrument(_selectedId);
}

void InstrumentConfigView::onRestoreDefaults()
{
    auto answer = QMessageBox::warning(this, tr("Restore Defaults"),
        tr("This will remove all instruments (including user-defined ones) "
           "and restore the built-in set.\n\nContinue?"),
        QMessageBox::Yes | QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    _selectedId.clear();
    _editingInstrument.reset();

    _db->restoreDefaultInstruments();

    clearDetails();
    refreshList();
}

// ── Mode edit dialog ────────────────────────────────────────────────────────

bool InstrumentConfigView::editModeDialog(InstrumentMode& mode, bool isNew)
{
    QDialog dlg(this);
    dlg.setWindowTitle(isNew ? tr("Add Mode") : tr("Edit Mode"));
    dlg.setMinimumWidth(460);
    auto* layout = new QVBoxLayout(&dlg);

    // Basic fields
    auto* basicForm = new QFormLayout;
    auto* keyEdit   = new QLineEdit(mode.key());
    auto* nameEdit  = new QLineEdit(mode.displayName());
    auto* descEdit  = new QLineEdit(mode.description());
    auto* typeCombo = new QComboBox;
    typeCombo->addItems({
        "spectroscopy", "photometry", "imaging", "ifu", "other"});
    typeCombo->setCurrentText(
        InstrumentMode::dataTypeToString(mode.dataType()));

    basicForm->addRow(tr("Key:"),          keyEdit);
    basicForm->addRow(tr("Display Name:"), nameEdit);
    basicForm->addRow(tr("Data Type:"),    typeCombo);
    basicForm->addRow(tr("Description:"),  descEdit);
    layout->addLayout(basicForm);

    // Spectral properties
    auto* specGroup = new QGroupBox(tr("Spectral Properties"));
    specGroup->setCheckable(true);
    specGroup->setChecked(mode.hasSpectralProperties());
    auto* specForm = new QFormLayout(specGroup);

    auto* disperserEdit = new QLineEdit(
        mode.hasSpectralProperties() ? mode.spectral().disperser : "");

    auto* resEdit = new QLineEdit;
    if (mode.hasSpectralProperties()) {
        QStringList coeffs;
        for (double c : mode.spectral().resolution.coefficients)
            coeffs << QString::number(c, 'g', 10);
        resEdit->setText(coeffs.join(", "));
    }
    resEdit->setPlaceholderText(tr("e.g. 2100  or  100, 0.5"));
    resEdit->setToolTip(tr(
        "R(\u03BB) = c\u2080 + c\u2081\u03BB + c\u2082\u03BB\u00B2 + ...\n"
        "Enter comma-separated coefficients. Single value = constant R."));

    auto* wlMinSpin = new QDoubleSpinBox;
    wlMinSpin->setRange(0, 200000);
    wlMinSpin->setDecimals(1);
    wlMinSpin->setSuffix(QStringLiteral(" \u00C5"));
    wlMinSpin->setValue(
        mode.hasSpectralProperties() ? mode.spectral().wavelengthMin : 0);

    auto* wlMaxSpin = new QDoubleSpinBox;
    wlMaxSpin->setRange(0, 200000);
    wlMaxSpin->setDecimals(1);
    wlMaxSpin->setSuffix(QStringLiteral(" \u00C5"));
    wlMaxSpin->setValue(
        mode.hasSpectralProperties() ? mode.spectral().wavelengthMax : 0);

    specForm->addRow(tr("Disperser:"),               disperserEdit);
    specForm->addRow(tr("R(\u03BB) coefficients:"),  resEdit);
    specForm->addRow(tr("\u03BB min:"),               wlMinSpin);
    specForm->addRow(tr("\u03BB max:"),               wlMaxSpin);
    layout->addWidget(specGroup);

    // Photometric properties
    auto* photGroup = new QGroupBox(tr("Photometric Properties"));
    photGroup->setCheckable(true);
    photGroup->setChecked(mode.hasPhotometricProperties());
    auto* photForm = new QFormLayout(photGroup);

    auto* cadenceSpin = new QDoubleSpinBox;
    cadenceSpin->setRange(0, 1e7);
    cadenceSpin->setDecimals(1);
    cadenceSpin->setSuffix(tr(" s"));
    cadenceSpin->setValue(
        mode.hasPhotometricProperties() ? mode.photometric().cadence : 0);

    auto* fovSpin = new QDoubleSpinBox;
    fovSpin->setRange(0, 50000);
    fovSpin->setDecimals(2);
    fovSpin->setSuffix(QStringLiteral(" \u2032"));
    fovSpin->setValue(
        mode.hasPhotometricProperties() ? mode.photometric().fov : 0);

    auto* pxScaleSpin = new QDoubleSpinBox;
    pxScaleSpin->setRange(0, 200);
    pxScaleSpin->setDecimals(3);
    pxScaleSpin->setSuffix(QStringLiteral(" \u2033/px"));
    pxScaleSpin->setValue(
        mode.hasPhotometricProperties() ? mode.photometric().pixelScale : 0);

    auto* filtersEdit = new QLineEdit(
        mode.hasPhotometricProperties()
            ? mode.photometric().filters.join(", ") : "");
    filtersEdit->setPlaceholderText(tr("e.g. u, g, r, i, z"));

    photForm->addRow(tr("Cadence:"),     cadenceSpin);
    photForm->addRow(tr("FOV:"),         fovSpin);
    photForm->addRow(tr("Pixel scale:"), pxScaleSpin);
    photForm->addRow(tr("Filters:"),     filtersEdit);
    layout->addWidget(photGroup);

    // Link data type combo to group box defaults
    auto syncGroups = [&]() {
        QString dt = typeCombo->currentText();
        if (dt == "spectroscopy" || dt == "ifu") {
            specGroup->setChecked(true);
            if (!mode.hasPhotometricProperties())
                photGroup->setChecked(false);
        } else if (dt == "photometry" || dt == "imaging") {
            photGroup->setChecked(true);
            if (!mode.hasSpectralProperties())
                specGroup->setChecked(false);
        }
    };

    connect(typeCombo, &QComboBox::currentTextChanged, &dlg, syncGroups);

    if (isNew)
        syncGroups();

    // Buttons
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    // Validate
    QString key = keyEdit->text().trimmed();
    if (key.isEmpty()) {
        QMessageBox::warning(this, tr("Validation"),
                             tr("Mode key cannot be empty."));
        return false;
    }

    // Check for duplicate key on new modes
    if (isNew && _editingInstrument && _editingInstrument->hasMode(key)) {
        QMessageBox::warning(this, tr("Validation"),
            tr("A mode with key \"%1\" already exists.").arg(key));
        return false;
    }

    // Apply
    mode.setKey(key);
    mode.setDisplayName(nameEdit->text().trimmed());
    mode.setDescription(descEdit->text().trimmed());
    mode.setDataType(
        InstrumentMode::dataTypeFromString(typeCombo->currentText()));

    if (specGroup->isChecked()) {
        SpectralProperties sp;
        sp.disperser = disperserEdit->text().trimmed();
        for (const auto& s :
             resEdit->text().split(",", Qt::SkipEmptyParts)) {
            bool ok;
            double v = s.trimmed().toDouble(&ok);
            if (ok) sp.resolution.coefficients.append(v);
        }
        sp.wavelengthMin = wlMinSpin->value();
        sp.wavelengthMax = wlMaxSpin->value();
        if (mode.hasSpectralProperties())
            sp.commonSetups = mode.spectral().commonSetups;
        mode.setSpectralProperties(sp);
    } else {
        mode.clearSpectralProperties();
    }

    if (photGroup->isChecked()) {
        PhotometricProperties pp;
        pp.cadence    = cadenceSpin->value();
        pp.fov        = fovSpin->value();
        pp.pixelScale = pxScaleSpin->value();
        pp.filters    = filtersEdit->text().split(",", Qt::SkipEmptyParts);
        for (auto& f : pp.filters)
            f = f.trimmed();
        mode.setPhotometricProperties(pp);
    } else {
        mode.clearPhotometricProperties();
    }

    return true;
}