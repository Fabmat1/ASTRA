#ifndef EXTRACTSED_H
#define EXTRACTSED_H

#include <QString>
#include <memory>
#include <vector>

#include "models/Photometry.h"

class SEDModel;
struct PhotometricPoint;

struct SEDExtractResult
{
    std::shared_ptr<SEDModel>       model;
    std::vector<PhotometricPoint>   photometricPoints;  // simplified for Photometry
    QString objectName;     // from tex header, e.g. "Gaia DR3 ..."
    QString folderName;     // immediate parent directory name
    bool    success = false;
    QString errorMessage;
};

namespace ExtractSED {

/// Returns true if \a dirPath looks like an ISIS SED fit directory
/// (contains at least photometry_fit.txt or photometry_fit_mag.txt).
bool isSEDFitDirectory(const QString& dirPath);

void mergePhotometryDat(const QString& filePath,
                        std::vector<SEDPhotometryPoint>& points);

/// Parse all available ISIS SED files in \a dirPath and build an SEDModel.
/// The model's compressed data (curve + observed photometry) is populated
/// in memory but NOT yet written to disk — the caller (DatabaseManager)
/// does that when saving.
SEDExtractResult extractFromDirectory(const QString& dirPath);

}  // namespace ExtractSED

#endif // EXTRACTSED_H