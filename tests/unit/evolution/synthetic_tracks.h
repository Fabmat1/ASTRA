#pragma once

// Synthetic track sets for the evolution tests: tracks that are linear in core mass,
// envelope mass and EEP (so every interpolation scheme is exact), and writers for the
// two file formats ASTRA reads (exported *.sdtracks.fits[.gz] and the Han FITS file).
// The real tracks are unpublished, so tests never depend on them.

#include "evolution/sdage/fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef HAVE_CCFITS
#include <fitsio.h>
#endif

namespace synth {

constexpr int kRows = 60;
inline const std::vector<double> kCores = {0.40, 0.45, 0.50, 0.55};
inline const std::vector<double> kEnvs = {0.0, 0.002, 0.004};

// Defining functions.  x = EEP / (rows - 1).
inline double fn(sdage::Quantity q, double core, double env, double x)
{
    switch (q) {
    case sdage::Q_LOGTEFF: return 4.30 + 0.5 * (core - 0.40) + 20.0 * env + 0.20 * x;
    case sdage::Q_LOGG:    return 5.60 - 1.0 * (core - 0.40) - 10.0 * env + 0.30 * x;
    case sdage::Q_AGE:     return (100.0 + 400.0 * (0.55 - core) + 1000.0 * env) * x;
    case sdage::Q_LOGL:    return 1.00 + 2.0 * (core - 0.40) - 30.0 * env + 0.50 * x;
    case sdage::Q_RADIUS:  return 0.15 + 0.2 * (core - 0.40) + 0.10 * x;
    case sdage::Q_HE:      return std::max(0.0, 0.98 - 1.2 * x);
    default:               return 0.0;
    }
}

inline std::vector<std::vector<double>> trackColumns(double core, double env, int rows = kRows)
{
    std::vector<std::vector<double>> cols(sdage::Q_COUNT, std::vector<double>(rows));
    for (int q = 0; q < sdage::Q_COUNT; ++q)
        for (int i = 0; i < rows; ++i)
            cols[q][i] = fn(static_cast<sdage::Quantity>(q), core, env, i / double(rows - 1));
    return cols;
}

inline sdage::TrackGrid rectGrid()
{
    std::vector<std::vector<std::vector<double>>> columns;
    for (double c : kCores)
        for (double e : kEnvs) columns.push_back(trackColumns(c, e));
    return sdage::TrackGrid::from_arrays(kCores, kEnvs, columns);
}

// The same tracks as a scattered set, every grid cell split along its main diagonal.
inline sdage::TrackGrid scatterGrid()
{
    std::vector<double> core, env;
    std::vector<std::vector<std::vector<double>>> columns;
    for (double c : kCores)
        for (double e : kEnvs) {
            core.push_back(c);
            env.push_back(e);
            columns.push_back(trackColumns(c, e));
        }
    const int ne = static_cast<int>(kEnvs.size());
    std::vector<int> tris;
    for (int ic = 0; ic + 1 < static_cast<int>(kCores.size()); ++ic)
        for (int ie = 0; ie + 1 < ne; ++ie) {
            const int n00 = ic * ne + ie, n10 = (ic + 1) * ne + ie;
            const int n01 = ic * ne + ie + 1, n11 = (ic + 1) * ne + ie + 1;
            tris.insert(tris.end(), {n00, n10, n11, n00, n11, n01});
        }
    return sdage::TrackGrid::from_nodes(core, env, columns, tris, 2);
}

// Scattered tracks at arbitrary nodes, triangulated like scipy.
inline sdage::TrackGrid scatterAt(const std::vector<double>& core, const std::vector<double>& env)
{
    std::vector<std::vector<std::vector<double>>> columns;
    for (std::size_t i = 0; i < core.size(); ++i) columns.push_back(trackColumns(core[i], env[i]));
    return sdage::TrackGrid::from_nodes(core, env, columns, sdage::delaunay(core, env), 2);
}

inline sdage::TrackGrid lineGrid()
{
    const std::vector<double> masses = {0.20, 0.25, 0.30, 0.35};
    std::vector<std::vector<std::vector<double>>> columns;
    for (double m : masses) columns.push_back(trackColumns(m + 0.2, 0.0));
    return sdage::TrackGrid::from_nodes(masses, std::vector<double>(masses.size(), 0.0), columns,
                                        {}, 1);
}

#ifdef HAVE_CCFITS

inline void fitsCheck(int status)
{
    if (status != 0) {
        char msg[FLEN_STATUS] = {0};
        fits_get_errstatus(status, msg);
        throw std::runtime_error(std::string("cfitsio: ") + msg);
    }
}

// An exported track set (the format of scripts/export_tracks.py).  A Scatter grid is
// written with its triangulation, a Line grid as NDIM = 1; group labels go to TRACKMETA.
inline void writeTrackSet(const std::string& path, const std::string& family, const sdage::TrackGrid& g,
                          bool withMeta = true)
{
    fitsfile* f = nullptr;
    int status = 0;
    const std::string bang = "!" + path;  // overwrite
    fits_create_file(&f, bang.c_str(), &status);
    fitsCheck(status);
    fits_create_img(f, BYTE_IMG, 0, nullptr, &status);
    int one = 1, ndim = g.ndim();
    fits_write_key(f, TINT, "SDTRACKS", &one, nullptr, &status);
    fits_write_key(f, TSTRING, "FAMILY", const_cast<char*>(family.c_str()), nullptr, &status);
    fits_write_key(f, TINT, "NDIM", &ndim, nullptr, &status);
    fitsCheck(status);

    const long nt = g.n_nodes(), neep = g.n_rows();
    std::vector<double> nodes;
    for (int n = 0; n < nt; ++n) nodes.insert(nodes.end(), {g.node_core(n), g.node_env(n)});
    long nshape[2] = {2, nt};
    fits_create_img(f, DOUBLE_IMG, 2, nshape, &status);
    fits_write_key(f, TSTRING, "EXTNAME", const_cast<char*>("NODES"), nullptr, &status);
    fits_write_img(f, TDOUBLE, 1, nodes.size(), nodes.data(), &status);

    std::vector<double> values;
    for (int n = 0; n < nt; ++n)
        for (int q = 0; q < sdage::Q_COUNT; ++q) {
            const double* c = g.column(n, static_cast<sdage::Quantity>(q));
            values.insert(values.end(), c, c + neep);
        }
    long vshape[3] = {neep, sdage::Q_COUNT, nt};
    fits_create_img(f, DOUBLE_IMG, 3, vshape, &status);
    fits_write_key(f, TSTRING, "EXTNAME", const_cast<char*>("VALUES"), nullptr, &status);
    fits_write_img(f, TDOUBLE, 1, values.size(), values.data(), &status);

    if (ndim == 2) {
        std::vector<int> tris = g.triangles();
        long tshape[2] = {3, static_cast<long>(tris.size() / 3)};
        fits_create_img(f, LONG_IMG, 2, tshape, &status);
        fits_write_key(f, TSTRING, "EXTNAME", const_cast<char*>("TRIANGLES"), nullptr, &status);
        fits_write_img(f, TINT, 1, tris.size(), tris.data(), &status);
    }
    fitsCheck(status);

    if (withMeta && g.has_groups()) {
        char* ttype[] = {const_cast<char*>("GROUP"), const_cast<char*>("FEH")};
        char* tform[] = {const_cast<char*>("8A"), const_cast<char*>("D")};
        fits_create_tbl(f, BINARY_TBL, nt, 2, ttype, tform, nullptr, "TRACKMETA", &status);
        std::vector<std::string> labels;
        std::vector<double> feh;
        for (int n = 0; n < nt; ++n) {
            labels.push_back(g.node_group(n));
            feh.push_back(g.node_feh(n));
        }
        std::vector<char*> ptrs;
        for (auto& s : labels) ptrs.push_back(s.data());
        fits_write_col(f, TSTRING, 1, 1, 1, nt, ptrs.data(), &status);
        fits_write_col(f, TDOUBLE, 2, 1, 1, nt, feh.data(), &status);
    }
    fits_close_file(f, &status);
    fitsCheck(status);
}

// The Han layout: one binary table per track, EXTNAME ending in the core mass.
inline void writeHanStyle(const std::string& path, const sdage::TrackGrid& g)
{
    fitsfile* f = nullptr;
    int status = 0;
    const std::string bang = "!" + path;
    fits_create_file(&f, bang.c_str(), &status);
    fits_create_img(f, BYTE_IMG, 0, nullptr, &status);
    fitsCheck(status);
    const char* names[] = {"mass", "log_teff", "logg", "age", "log_l", "radius", "hec"};
    for (int n = 0; n < g.n_nodes(); ++n) {
        char* ttype[7];
        char* tform[7];
        for (int k = 0; k < 7; ++k) {
            ttype[k] = const_cast<char*>(names[k]);
            tform[k] = const_cast<char*>("D");
        }
        char ext[64];
        std::snprintf(ext, sizeof ext, "track_%d_Z_%.2f", n, g.node_core(n));
        fits_create_tbl(f, BINARY_TBL, g.n_rows(), 7, ttype, tform, nullptr, ext, &status);
        std::vector<double> env(g.n_rows(), g.node_env(n));
        fits_write_col(f, TDOUBLE, 1, 1, 1, g.n_rows(), env.data(), &status);
        for (int q = 0; q < sdage::Q_COUNT; ++q)
            fits_write_col(f, TDOUBLE, 2 + q, 1, 1, g.n_rows(),
                           const_cast<double*>(g.column(n, static_cast<sdage::Quantity>(q))), &status);
        fitsCheck(status);
    }
    fits_close_file(f, &status);
    fitsCheck(status);
}

// The text triangulation that goes with a Han FITS file (main diagonals).
inline void writeHanTriangulation(const std::string& path)
{
    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) throw std::runtime_error("cannot write " + path);
    for (std::size_t ic = 0; ic + 1 < kCores.size(); ++ic)
        for (std::size_t ie = 0; ie + 1 < kEnvs.size(); ++ie) {
            const double c0 = kCores[ic], c1 = kCores[ic + 1], e0 = kEnvs[ie], e1 = kEnvs[ie + 1];
            std::fprintf(fp, "%g %g %g %g %g %g\n", c0, e0, c1, e0, c1, e1);
            std::fprintf(fp, "%g %g %g %g %g %g\n", c0, e0, c1, e1, c0, e1);
        }
    std::fclose(fp);
}

#endif  // HAVE_CCFITS

}  // namespace synth
