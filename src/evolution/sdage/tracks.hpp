// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.
//
// Changes made in ASTRA (keep this list current when porting fixes either way):
//   - include paths are root-anchored ("evolution/sdage/...").
//   - FITS files are read into memory and opened with fits_open_memfile (gzip is
//     inflated with zlib), so paths go through std::filesystem as UTF-8 and work on
//     Windows with non-ASCII user folders.  Without HAVE_CCFITS the readers throw.
//   - TrackGrid::select_nodes(): subsets of any geometry; a Scatter subset is
//     re-triangulated with delaunay() (filtering the stored triangles left holes of
//     up to 50 % of the support for Dorman composition subsets).
//   - optional per-track group labels and [Fe/H] (TRACKMETA extension written by
//     scripts/export_tracks.py, e.g. the Dorman composition of every track).
//   - FitConfig::chain_every thins the samples returned with keep_chain.

// A set of evolutionary tracks and the interpolation between them.
//
// Every track is sampled at the same number of "equivalent evolutionary points"
// (EEPs), so a model is addressed by (core mass, envelope mass, EEP) and all stellar
// quantities are obtained by interpolating between neighbouring tracks at fixed EEP
// and then along the EEP axis.
//
// Three grid geometries are supported:
//   Rect     complete rectangular (core mass x envelope mass) grid   - Han
//   Scatter  tracks at arbitrary (core mass, envelope mass) points,
//            interpolated on a given triangulation                   - Nicolas, Dorman
//   Line     tracks that differ in one mass only                     - Driebe
#pragma once

#include <string>
#include <vector>

namespace sdage {

/// Quantities stored for every track point.
enum Quantity : int {
    Q_LOGTEFF = 0,  ///< log10(Teff / K)
    Q_LOGG,         ///< log10(g / cm s^-2)
    Q_AGE,          ///< age [Myr] (since the zero-age EHB for the sdB families)
    Q_LOGL,         ///< log10(L / Lsun)
    Q_RADIUS,       ///< radius [Rsun]
    Q_HE,           ///< central helium mass fraction (NaN if the family has none)
    Q_COUNT
};

enum class Geometry { Rect, Scatter, Line };

struct FitsHandle;  // an open FITS file (tracks.cpp)

/// How to interpolate between tracks of a Rect grid.
enum class InterpMode {
    Bilinear,      ///< bilinear inside each grid cell (smooth, no arbitrary choices)
    Triangulated,  ///< linear on triangles; reproduces scipy's LinearNDInterpolator
                   ///< when given scipy's triangulation.  Always used for Scatter.
};

/// Interpolation weights of the (up to) four tracks surrounding a point.
struct CellWeights {
    int node[4];
    double w[4];
};

/// Delaunay triangulation of points (x[i], y[i]) in raw units, as scipy.spatial /
/// Qhull computes it for sdfit's LinearNDInterpolator: node-index triples, counter-
/// clockwise.  Empty if the points are collinear or fewer than three.  Only exactly
/// co-circular points (a rectangular grid) can be split differently from Qhull.
std::vector<int> delaunay(const std::vector<double>& x, const std::vector<double>& y);

class TrackGrid {
public:
    /// Load a track set: either a file written by scripts/export_tracks.py
    /// (*.sdtracks.fits.gz, any family) or the Han FITS file itself (see load_fits).
    /// Throws std::runtime_error on any problem.
    static TrackGrid load(const std::string& path);

    /// Read the multi-extension FITS file of the Han tracks: one binary table per
    /// track, extension name ending in the core mass ("..._Z_0.45"), column "mass"
    /// holding the envelope mass.  Gives a Rect grid with bilinear interpolation.
    static TrackGrid load_fits(const std::string& path);

    /// Read a file written by scripts/export_tracks.py.
    static TrackGrid load_trackset(const std::string& path);

    /// Rect grid from memory.  `columns[node][quantity]` must all have the same
    /// length; node = icore*n_env + ienv.
    static TrackGrid from_arrays(std::vector<double> core_masses, std::vector<double> env_masses,
                                 const std::vector<std::vector<std::vector<double>>>& columns);

    /// Scatter (ndim = 2, with triangles as node-index triples) or Line (ndim = 1)
    /// grid from memory.  `columns[node][quantity]`.
    static TrackGrid from_nodes(std::vector<double> node_core, std::vector<double> node_env,
                                const std::vector<std::vector<std::vector<double>>>& columns,
                                const std::vector<int>& triangles, int ndim);

    /// Same as load(), for a file already in memory (plain or gzip-compressed FITS).
    /// `source` is only used in messages and as source().
    static TrackGrid load_memory(std::vector<unsigned char> bytes, const std::string& source);

    /// Rect only: keep the tracks with the listed core masses (e.g. {0.45, 0.50}).
    TrackGrid select_cores(const std::vector<double>& cores) const;

    /// Keep the tracks with keep[node] == true (keep.size() == n_nodes()).
    ///   Rect     the kept tracks must form a complete (core x envelope) sub-grid;
    ///            the result interpolates bilinearly (as select_cores).
    ///   Scatter  a fresh Delaunay triangulation of the kept tracks (as sdfit, which
    ///            triangulates whatever it loads); see delaunay().
    ///   Line     the kept tracks (at least two).
    /// Throws std::runtime_error if the subset gives no usable grid.
    TrackGrid select_nodes(const std::vector<bool>& keep) const;

    /// Scatter: the triangulation in use (node-index triples); empty otherwise.
    std::vector<int> triangles() const;

    /// Optional per-track metadata (e.g. the Dorman composition "z22" and its [Fe/H]).
    bool has_groups() const { return !node_group_.empty(); }
    const std::string& node_group(int node) const;
    double node_feh(int node) const;
    void set_node_groups(std::vector<std::string> groups, std::vector<double> feh);

    /// Rect only: use the triangulation stored in a text file (one triangle per line:
    /// "core0 env0 core1 env1 core2 env2") and switch to InterpMode::Triangulated.
    void load_triangulation(const std::string& path);
    void set_cell_diagonal(int icore, int ienv, bool anti);
    void set_mode(InterpMode mode);
    InterpMode mode() const { return mode_; }

    Geometry geometry() const { return geometry_; }
    /// Number of mass axes: 2 (core + envelope mass) or 1 (Line).
    int ndim() const { return geometry_ == Geometry::Line ? 1 : 2; }
    bool has_helium() const { return has_helium_; }
    const std::string& family() const { return family_; }
    const std::string& source() const { return source_; }
    /// One-line description for log output.
    std::string describe() const;

    int n_nodes() const { return static_cast<int>(node_core_.size()); }
    int n_rows() const { return n_rows_; }
    double node_core(int node) const { return node_core_[node]; }
    double node_env(int node) const { return node_env_[node]; }
    /// Sorted distinct core / envelope masses of the tracks.
    const std::vector<double>& core_masses() const { return cores_; }
    const std::vector<double>& env_masses() const { return envs_; }
    int n_core() const { return static_cast<int>(cores_.size()); }
    int n_env() const { return static_cast<int>(envs_.size()); }
    double core_min() const { return cores_.front(); }
    double core_max() const { return cores_.back(); }
    double env_min() const { return envs_.front(); }
    double env_max() const { return envs_.back(); }
    double eep_max() const { return static_cast<double>(n_rows_ - 1); }

    /// Number of EEPs on either side over which dAge/dEEP has to be taken (0 for
    /// finely tabulated ages; > 0 if the ages are rounded so that many steps are
    /// zero, as in the Nicolas tracks).  Same rule as sdfit.
    int age_window() const { return age_window_; }

    /// Interpolation weights at (core, env).  Returns false where the tracks give no
    /// model (outside the grid / the triangulation).  `env` is ignored for Line.
    bool locate(double core, double env, CellWeights& out) const;

    /// Pointer to the n_rows() values of one quantity of one track.
    const double* column(int node, Quantity q) const {
        return data_.data() + (static_cast<std::size_t>(node) * Q_COUNT + q) * n_rows_;
    }

    /// Quantity at a (fractional) EEP; linear along the EEP axis.  EEPs outside
    /// [0, n_rows-1] are clipped to the ends of the track.
    double value(const CellWeights& cw, Quantity q, double eep) const;
    double value_at_row(const CellWeights& cw, Quantity q, int row) const;
    /// Whole interpolated track of one quantity; `out` must hold n_rows() values.
    void interpolate_column(const CellWeights& cw, Quantity q, double* out) const;

private:
    static TrackGrid load_trackset_file(FitsHandle& file);
    static TrackGrid load_fits_file(FitsHandle& file);
    void copy_meta_from(const TrackGrid& other, const std::vector<int>& kept_nodes);
    void set_data(const std::vector<std::vector<std::vector<double>>>& columns);
    void finish();  // derived members: axes, helium flag, age window
    void build_triangle_index();
    int rect_node(int icore, int ienv) const { return icore * n_env() + ienv; }

    Geometry geometry_ = Geometry::Rect;
    std::vector<double> node_core_, node_env_;  // per track
    std::vector<double> cores_, envs_;          // sorted distinct values (Rect: the axes)
    int n_rows_ = 0;
    std::vector<double> data_;  // [node][quantity][row]
    bool has_helium_ = true;
    int age_window_ = 0;
    std::string family_ = "han";
    std::string source_;
    std::vector<std::string> node_group_;  // per track, empty if the file has none
    std::vector<double> node_feh_;         // per track, NaN where unknown

    // Rect
    InterpMode mode_ = InterpMode::Bilinear;
    std::vector<signed char> diag_;  // per cell: -1 unknown, 0 main, 1 anti diagonal

    // Scatter
    struct Triangle {
        int n[3];
        double x0, y0;           // first vertex
        double a, b, c, d;       // inverse of the edge matrix
    };
    std::vector<Triangle> triangles_;
    int buckets_ = 0;                          // bucket grid resolution
    std::vector<std::vector<int>> bucket_;     // triangles overlapping each bucket

    // Line
    std::vector<int> line_order_;  // nodes sorted by mass
};

}  // namespace sdage
