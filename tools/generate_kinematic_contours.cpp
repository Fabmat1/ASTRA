// Generator for the kinematic reference contours bundled under
// resources/data/kinematics/ (see src/kinematics/KinematicContours.h).
//
// The contours derive from the three-component velocity distribution used
// by GalKin::PopulationClassifier: per-axis means and dispersions from
// Anguiano et al. (2020, AJ 160, 43) Table 1 as adopted by the classifier
// (kMean/kSigma), completed to full 3×3 covariance matrices with the
// vertex deviation α_Rφ and velocity-ellipsoid tilt α_Rz of their Table 2
// via
//
//     tan(2 α_ij) = 2 Σ_ij / (Σ_ii − Σ_jj)
//
// evaluated with the classifier dispersions. Anguiano et al. work in
// (v_R, v_φ, v_z) with v_R positive outward; the Star/classifier frame
// has U positive toward the GC (U = −v_R at the solar azimuth), so the
// cross terms map as Σ_UV = −Σ_Rφ and Σ_UW = −Σ_Rz; Σ_VW = 0 (the paper
// quotes no φz term, as expected from Galactic-plane symmetry).
//
// All contours enclose 86.47% (1 − e⁻²) of the probability mass in their
// plane - the mass of a 2σ Mahalanobis ellipse of a 2D Gaussian:
//   uv_{thin,thick,halo}.csv      V,U   2σ ellipse of the (U,V) marginal
//   wv_{thin,thick,halo}.csv      V,W   2σ ellipse of the (V,W) marginal
//   uw_{thin,thick,halo}.csv      U,W   2σ ellipse of the (U,W) marginal
//   toomre_{thin,thick,halo}.csv  V,√(U²+W²)  iso-density contour of the
//                                 projected density in the Toomre plane
//
// The Toomre curve is NOT the silhouette of the 3D ellipsoid: s=√(U²+W²)
// is a polar radius, the projected density p(V,s) = s ∫ N₃(s cosθ, V,
// s sinθ) dθ vanishes at s = 0, and the equal-probability contour stays
// clear of the V axis. The density is evaluated by quadrature, so the
// curve is the exact limit an MC-sampled contour would converge to.
//
// The J_z–e parallelogram (Pauli et al. 2006) is hand-measured and NOT
// regenerated here.
//
// Usage: generate_kinematic_contours <output-directory>
//        (CMake target `regenerate_kinematic_contours` runs it in place)

#include "kinematics/PopulationClassifier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using GalKin::PopulationClassifier;

constexpr double kDeg  = M_PI / 180.0;
constexpr double kNSig = 2.0;                    // ellipse level in σ
constexpr double kMass = 0.86466471676338730811; // 1 − e⁻², 2σ 2D mass

// Anguiano et al. (2020), Table 2: vertex deviation α_Rφ and tilt α_Rz
// [degrees] for the chemically selected thin/thick disk and the
// [Fe/H] < −1 halo (same population rows as kMean/kSigma).
constexpr double kAlphaRPhi[3] = {-4.01, -6.01, -2.30};
constexpr double kAlphaRz[3]   = {+1.41, +5.16, +11.43};

const char* kPopName[3] = {"thin", "thick", "halo"};

struct Cov3 {
    // symmetric covariance in (U, V, W)
    double UU, VV, WW, UV, UW, VW;
};

Cov3 covariance(int pop)
{
    const double sU = PopulationClassifier::kSigma[pop][0];
    const double sV = PopulationClassifier::kSigma[pop][1];
    const double sW = PopulationClassifier::kSigma[pop][2];
    // tan(2α) = 2Σ_ij/(Σ_ii−Σ_jj) in (v_R,v_φ,v_z); U = −v_R flips the
    // sign of both cross terms involving U.
    const double covRPhi =
        0.5 * std::tan(2.0 * kAlphaRPhi[pop] * kDeg) * (sU * sU - sV * sV);
    const double covRz =
        0.5 * std::tan(2.0 * kAlphaRz[pop] * kDeg) * (sU * sU - sW * sW);
    return {sU * sU, sV * sV, sW * sW, -covRPhi, -covRz, 0.0};
}

// 2×2 symmetric matrix helpers ------------------------------------------------

struct Mat2 {
    double a, b, c; // [[a, b], [b, c]]
};

// principal square root of a symmetric positive-definite 2×2 matrix
Mat2 sqrtSpd(const Mat2& m)
{
    const double tr  = m.a + m.c;
    const double det = m.a * m.c - m.b * m.b;
    const double s   = std::sqrt(std::max(0.0, det));
    const double t   = std::sqrt(std::max(1e-300, tr + 2.0 * s));
    return {(m.a + s) / t, m.b / t, (m.c + s) / t};
}

// CSV output ------------------------------------------------------------------

void writeCsv(const std::string& dir, const std::string& name,
              const std::string& header,
              const std::vector<std::array<double, 2>>& pts)
{
    const std::string path = dir + "/" + name;
    std::ofstream out(path);
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        std::exit(1);
    }
    out << header;
    char line[64];
    for (const auto& p : pts) {
        std::snprintf(line, sizeof line, "%.3f,%.3f\n", p[0], p[1]);
        out << line;
    }
    std::cout << "wrote " << path << " (" << pts.size() << " points)\n";
}

std::string headerFor(const std::string& diagram, int pop,
                      const std::string& what, const std::string& columns)
{
    return "# " + diagram + " contour, " + kPopName[pop] + " - " + what +
           "\n"
           "# of the Anguiano et al. (2020) velocity distribution "
           "(classifier\n"
           "# means/dispersions, Table 2 vertex deviation and ellipsoid "
           "tilt),\n"
           "# galactocentric frame.\n"
           "# Regenerate: cmake --build build --target "
           "regenerate_kinematic_contours\n"
           "# columns: " + columns + "\n";
}

// 2σ ellipse of a 2D marginal: μ + 2·√Σ·(cosθ, sinθ), closed polyline
std::vector<std::array<double, 2>> marginalEllipse(double mx, double my,
                                                   const Mat2& cov, int n)
{
    const Mat2 A = sqrtSpd(cov);
    std::vector<std::array<double, 2>> pts;
    pts.reserve(n + 1);
    for (int i = 0; i <= n; ++i) {
        const double th = 2.0 * M_PI * i / n;
        const double cx = std::cos(th), sy = std::sin(th);
        pts.push_back({mx + kNSig * (A.a * cx + A.b * sy),
                       my + kNSig * (A.b * cx + A.c * sy)});
    }
    return pts;
}

// Toomre iso-density contour ---------------------------------------------------

struct Gauss3 {
    double mu[3];
    double inv[3][3]; // Σ⁻¹
    double norm;      // (2π)^{-3/2} |Σ|^{-1/2}
};

Gauss3 gauss3(int pop)
{
    const auto S = covariance(pop);
    const double M[3][3] = {{S.UU, S.UV, S.UW},
                            {S.UV, S.VV, S.VW},
                            {S.UW, S.VW, S.WW}};
    const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                       M[0][1] * (M[0][1] * M[2][2] - M[1][2] * M[0][2]) +
                       M[0][2] * (M[0][1] * M[1][2] - M[1][1] * M[0][2]);
    Gauss3 g;
    for (int i = 0; i < 3; ++i)
        g.mu[i] = PopulationClassifier::kMean[pop][i];
    g.inv[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det;
    g.inv[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det;
    g.inv[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
    g.inv[0][1] = g.inv[1][0] =
        -(M[0][1] * M[2][2] - M[0][2] * M[2][1]) / det;
    g.inv[0][2] = g.inv[2][0] =
        (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
    g.inv[1][2] = g.inv[2][1] =
        -(M[0][0] * M[1][2] - M[0][2] * M[0][1]) / det;
    g.norm = std::pow(2.0 * M_PI, -1.5) / std::sqrt(det);
    return g;
}

// projected Toomre-plane density p(V=v, s) = s ∮ N₃(s cosθ, v, s sinθ) dθ
// (trapezoid over the periodic angle → spectrally accurate)
double toomreDensity(const Gauss3& g, double v, double s, int nTheta)
{
    if (s <= 0.0)
        return 0.0;
    const double dv = v - g.mu[1];
    double sum = 0.0;
    for (int j = 0; j < nTheta; ++j) {
        const double th = 2.0 * M_PI * j / nTheta;
        const double du = s * std::cos(th) - g.mu[0];
        const double dw = s * std::sin(th) - g.mu[2];
        const double q =
            du * (g.inv[0][0] * du + 2.0 * g.inv[0][1] * dv +
                  2.0 * g.inv[0][2] * dw) +
            dv * (g.inv[1][1] * dv + 2.0 * g.inv[1][2] * dw) +
            dw * g.inv[2][2] * dw;
        sum += std::exp(-0.5 * q);
    }
    return g.norm * s * sum * (2.0 * M_PI / nTheta);
}

// density level whose super-level set encloses `mass` of the total
// probability (grid histogram of p, cells sorted by density)
double isoDensityLevel(const Gauss3& g, double vLo, double vHi, double sMax,
                       double mass)
{
    const int NV = 900, NS = 900, NTH = 128;
    const double dV = (vHi - vLo) / NV, dS = sMax / NS;
    std::vector<double> p;
    p.reserve(size_t(NV) * NS);
    for (int iv = 0; iv < NV; ++iv) {
        const double v = vLo + (iv + 0.5) * dV;
        for (int is = 0; is < NS; ++is)
            p.push_back(toomreDensity(g, v, (is + 0.5) * dS, NTH));
    }
    std::sort(p.begin(), p.end(), std::greater<>());
    const double dA = dV * dS;
    double acc = 0.0;
    for (const double d : p) {
        acc += d * dA;
        if (acc >= mass)
            return d;
    }
    return p.back();
}

std::vector<std::array<double, 2>> toomreContour(int pop, int nV)
{
    const Gauss3 g = gauss3(pop);
    const double sU = PopulationClassifier::kSigma[pop][0];
    const double sV = PopulationClassifier::kSigma[pop][1];
    const double sW = PopulationClassifier::kSigma[pop][2];
    const double muV  = g.mu[1];
    const double vMin = muV - 5.0 * sV, vMax = muV + 5.0 * sV;
    const double sMax = std::hypot(g.mu[0], g.mu[2]) + 5.0 * (sU + sW);

    const double L = isoDensityLevel(g, vMin, vMax, sMax, kMass);

    const int NTH = 256, NS = 800;
    auto density = [&](double v, double s) {
        return toomreDensity(g, v, s, NTH);
    };
    // coarse sup_s p(v, s) - only used to bracket the contour's V extent
    auto maxOverS = [&](double v) {
        double best = 0.0;
        for (int j = 1; j <= 300; ++j)
            best = std::max(best, toomreDensity(g, v, sMax * j / 300.0, 128));
        return best;
    };
    auto vEdge = [&](double in, double out) {
        for (int it = 0; it < 40; ++it) {
            const double mid = 0.5 * (in + out);
            (maxOverS(mid) >= L ? in : out) = mid;
        }
        return 0.5 * (in + out);
    };
    const double vLo = vEdge(muV, vMin);
    const double vHi = vEdge(muV, vMax);

    // p(v,·) ≥ L on an s-interval (p vanishes at s = 0 and s → ∞);
    // refine the outermost scan crossings by bisection
    auto bisectCross = [&](double v, double sIn, double sOut) {
        for (int it = 0; it < 40; ++it) {
            const double mid = 0.5 * (sIn + sOut);
            (density(v, mid) >= L ? sIn : sOut) = mid;
        }
        return 0.5 * (sIn + sOut);
    };

    std::vector<double> vs(nV), sHi(nV), sLo(nV);
    const double ds = sMax / NS;
    for (int i = 0; i < nV; ++i) {
        const double v = vLo + (vHi - vLo) * i / (nV - 1);
        int first = -1, last = -1, peak = 1;
        double pPeak = 0.0;
        for (int j = 1; j <= NS; ++j) {
            const double p = density(v, j * ds);
            if (p > pPeak) {
                pPeak = p;
                peak  = j;
            }
            if (p >= L) {
                if (first < 0)
                    first = j;
                last = j;
            }
        }
        vs[i] = v;
        if (first < 0) {
            // contour endpoint: the level set degenerates to the peak
            sLo[i] = sHi[i] = peak * ds;
        } else {
            sLo[i] = bisectCross(v, first * ds, (first - 1) * ds);
            sHi[i] = bisectCross(v, last * ds, (last + 1) * ds);
        }
    }

    // closed outline: upper branch left→right, lower branch back
    std::vector<std::array<double, 2>> pts;
    pts.reserve(2 * nV + 1);
    for (int i = 0; i < nV; ++i)
        pts.push_back({vs[i], sHi[i]});
    for (int i = nV - 1; i >= 0; --i)
        pts.push_back({vs[i], sLo[i]});
    pts.push_back(pts.front());
    return pts;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const std::string dir = argv[1];

    constexpr int kEllipseN = 256;
    constexpr int kToomreNV = 241;

    const std::string ellipseWhat =
        "2 sigma Mahalanobis ellipse (86.5% mass) of the marginal";
    const std::string toomreWhat =
        "iso-density contour enclosing 86.5% of the projected mass";

    for (int pop = 0; pop < 3; ++pop) {
        const auto S = covariance(pop);
        const double muU = PopulationClassifier::kMean[pop][0];
        const double muV = PopulationClassifier::kMean[pop][1];
        const double muW = PopulationClassifier::kMean[pop][2];
        const std::string name(kPopName[pop]);

        // marginal 2σ ellipses; CSV column order matches the plots
        writeCsv(dir, "uv_" + name + ".csv",
                 headerFor("UV", pop, ellipseWhat, "V[km/s],U[km/s]"),
                 marginalEllipse(muV, muU, {S.VV, S.UV, S.UU}, kEllipseN));
        writeCsv(dir, "wv_" + name + ".csv",
                 headerFor("WV", pop, ellipseWhat, "V[km/s],W[km/s]"),
                 marginalEllipse(muV, muW, {S.VV, S.VW, S.WW}, kEllipseN));
        writeCsv(dir, "uw_" + name + ".csv",
                 headerFor("UW", pop, ellipseWhat, "U[km/s],W[km/s]"),
                 marginalEllipse(muU, muW, {S.UU, S.UW, S.WW}, kEllipseN));
        writeCsv(dir, "toomre_" + name + ".csv",
                 headerFor("Toomre", pop, toomreWhat,
                           "V[km/s],sqrt(U^2+W^2)[km/s]"),
                 toomreContour(pop, kToomreNV));
    }
    return 0;
}
