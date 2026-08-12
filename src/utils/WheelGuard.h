#pragma once

class QWidget;

namespace astra {

/// Stop the mouse wheel from editing a spin box / combo box / slider, and hand
/// the scroll to the enclosing scroll area instead.  Long option pages get
/// scrolled far more often than their individual fields get edited, so a wheel
/// tick that lands on a field would otherwise silently change a parameter the
/// user only meant to scroll past.
void blockWheelScrolling(QWidget* widget);

/// Same, applied to `root` and every spin box / combo box / slider below it.
/// Scroll bars are left alone so the widget's own scrolling keeps working.
void blockWheelScrollingRecursive(QWidget* root);

} // namespace astra
