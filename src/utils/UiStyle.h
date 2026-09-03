#pragma once

class QApplication;

namespace UiStyle {

// Corrects platform pixel metrics that do not survive Qt's device-independent
// coordinate system.
//
// So far that is the text cursor: QWindowsTheme returns SPI_GETCARETWIDTH
// unchanged, but Windows reports that in physical pixels and has already scaled
// it for the display's DPI. Qt then treats the number as device-independent and
// scales it a second time, so on a 200% display the system's one-pixel caret is
// painted four pixels wide - a solid block in a line edit. Nothing to correct on
// X11 or macOS, where the platform theme reports a plain 1.
//
// Install once on the QApplication before any window is shown.
void installMetricFixes(QApplication* app);

} // namespace UiStyle
