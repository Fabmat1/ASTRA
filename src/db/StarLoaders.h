#pragma once

/*  Wires freshly loaded stars to the database: lazy loaders for photometry,
 *  spectra and the RV curve, the RV curve's point persistence and BJD
 *  resolution, and the summary-row persistence. Every place that loads a
 *  project's stars for real use (the window, the headless modes, the AI
 *  tools) needs exactly this, so it lives in one place.                     */

#include <QString>
#include <memory>
#include <vector>

class DatabaseManager;
class Star;

void attachStarLoaders(DatabaseManager* dbm, const std::vector<std::shared_ptr<Star>>& stars,
                       const QString& projectId);
