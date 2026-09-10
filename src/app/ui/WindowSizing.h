#pragma once

class QObject;
class QWidget;

namespace WindowSizing {

// Keeps top-level windows inside the screen they live on.
//
// Qt's default size constraint copies a layout's minimum size onto the window,
// so a dialog whose content grows after construction (results filling in, a
// heavy tab being built) drags the window with it - on a small screen the
// window ends up taller than the desktop with its lower half unreachable.
// The guard caps that layout-driven minimum at the available screen area and
// pulls windows that already ran over the edge back into view.
//
// Install once on the QApplication before any window is shown.
void installScreenGuard(QObject* app);

// Applies the same clamp to a single window right now.
void fitToScreen(QWidget* window);

} // namespace WindowSizing
