#ifndef STARSHARE_H
#define STARSHARE_H

#include <QString>
#include <memory>
#include <vector>

class QWidget;
class Star;
class Project;
class ApplicationController;

namespace StarShare {

// Prompts for a destination .astra file and exports the given stars
// (all metadata/spectra/photometry/fits/RV). Shows its own message boxes.
void exportStarsInteractive(QWidget *parent, ApplicationController *controller,
                            const std::vector<std::shared_ptr<Star>> &stars);

// Reads a .astra file (prompts if `path` is empty), persists its stars into
// `project` with fresh IDs, and adds them to the in-memory project.
// Returns number of stars imported, 0 on cancel/empty, -1 on error.
int importFileInteractive(QWidget *parent, ApplicationController *controller,
                          std::shared_ptr<Project> project,
                          const QString           &path = {});

// Deep-copies the given stars (with all attached spectra/fits/photometry/
// SEDs/lightcurves/RV) into `target`, assigning fresh IDs so the copies are
// fully independent of the originals. Shows a progress dialog and its own
// message boxes. Returns the number of stars copied, 0 on no-op, -1 on error.
int copyStarsToProject(QWidget *parent, ApplicationController *controller,
                       const std::vector<std::shared_ptr<Star>> &stars,
                       std::shared_ptr<Project>                  target);

// Moves the given stars (with all attached data) from `source` to `target`.
// The stars keep their IDs and data files; only their project assignment
// changes. `source` may be null. Shows its own message boxes. Returns the
// number of stars moved, 0 on no-op, -1 on error.
int moveStarsToProject(QWidget *parent, ApplicationController *controller,
                       const std::vector<std::shared_ptr<Star>> &stars,
                       std::shared_ptr<Project>                  source,
                       std::shared_ptr<Project>                  target);

} // namespace StarShare

#endif // STARSHARE_H