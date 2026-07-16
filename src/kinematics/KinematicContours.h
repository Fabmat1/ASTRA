#pragma once

// Reference contours for the kinematic population diagrams (Toomre, U–V,
// W–V, U–W, J_z–e), bundled as two-column CSV polylines under
// :/data/kinematics/. The default files are the 2σ contours of the
// Anguiano et al. (2020) velocity distributions and the Pauli et al. (2006)
// thick-disk parallelogram of the J_z–e diagram; they can be regenerated or
// replaced without touching the code.
//
// IMPORTANT: the CSV velocity contours live in the *galactocentric* frame
// of the thesis (V ≈ 230 km/s for the thin disk). The Star model stores
// heliocentric UVW, so plots in the heliocentric frame must shift either
// the data or the contour by PopulationClassifier::kFrameShift.

#include <QString>
#include <QVector>

namespace GalKin {

struct ContourCurve {
    QVector<double> x, y;
    bool isEmpty() const { return x.isEmpty(); }
};

enum class Diagram { Toomre, UV, WV, UW, JzE };

// Load the contour polyline for one diagram and population
// (pop: 0 = thin disk, 1 = thick disk, 2 = halo). Results are cached.
// For Diagram::JzE only pop 1 (the thick-disk parallelogram) exists.
const ContourCurve& kinematicContour(Diagram d, int pop);

} // namespace GalKin
