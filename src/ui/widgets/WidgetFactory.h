#pragma once

// Small, domain-free widget helpers.
//
// These existed as private copies in four different dialogs, which is how the
// spin boxes ended up with two different maximum widths and inconsistent
// keyboard tracking. Anything here must stay free of domain types so every
// dialog can reach for it.

#include <QString>

class QDoubleSpinBox;
class QLayout;
class QSpinBox;

namespace astra {

/// Deletes every widget and nested layout inside `layout`, leaving it empty and
/// reusable. Safe on nullptr.
void clearLayout(QLayout* layout);

/// A QDoubleSpinBox set up the way ASTRA's parameter forms want one: fixed
/// decimals, no keyboard tracking (so a half-typed number never fires a
/// valueChanged), wheel scrolling blocked, and a narrow fixed width.
QDoubleSpinBox* makeDoubleSpin(double min, double max, int decimals, double val,
                               double step = 1.0, const QString& suffix = {},
                               int maxWidth = 110);

/// The integer counterpart. Same conventions, no suffix or decimals.
QSpinBox* makeIntSpin(int min, int max, int val, int step = 1);

}   // namespace astra
