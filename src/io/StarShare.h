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

} // namespace StarShare

#endif // STARSHARE_H