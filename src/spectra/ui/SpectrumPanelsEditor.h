#pragma once

#include "core/InstrumentMode.h"

#include <QWidget>

class QTableWidget;

/// Editor for an instrument mode's spectrum-panel set: one row per panel
/// with its title, wavelength window and how it is scaled (normalized, flux,
/// or following the panel's Normalized / Flux switch). Rows can be added,
/// removed, reordered, and reset to ASTRA's built-in set.
class SpectrumPanelsEditor : public QWidget
{
    Q_OBJECT
public:
    explicit SpectrumPanelsEditor(QWidget* parent = nullptr);

    void setPanels(const QVector<SpectrumViewPanel>& panels);
    /// The rows with a window (to > from), in table order.
    QVector<SpectrumViewPanel> panels() const;

private:
    void addRow(const SpectrumViewPanel& p, int at = -1);
    void moveCurrent(int delta);

    QTableWidget* _table = nullptr;
};
