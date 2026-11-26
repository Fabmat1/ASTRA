// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

#include "evolution/sdage/tracks.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef HAVE_CCFITS
#include <fitsio.h>
#include <zlib.h>
#endif

namespace sdage {

namespace {

constexpr double kNodeTol = 1e-9;

int find_index(const std::vector<double>& axis, double value) {
    for (std::size_t i = 0; i < axis.size(); ++i)
        if (std::fabs(axis[i] - value) < kNodeTol) return static_cast<int>(i);
    return -1;
}

std::vector<double> sorted_unique(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(),
                        [](double a, double b) { return std::fabs(a - b) < kNodeTol; }),
            v.end());
    return v;
}

// Paths are UTF-8 everywhere in ASTRA; going through std::filesystem::path keeps
// non-ASCII folders working on Windows, where a narrow std::string path would be
// read in the ANSI code page.
std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream in(fs_path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open track file '" + path + "'");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (in.bad()) throw std::runtime_error("cannot read track file '" + path + "'");
    return bytes;
}

std::vector<std::vector<double>> node_columns(const TrackGrid& g, int node) {
    std::vector<std::vector<double>> cols(Q_COUNT);
    for (int q = 0; q < Q_COUNT; ++q) {
        const double* p = g.column(node, static_cast<Quantity>(q));
        cols[q].assign(p, p + g.n_rows());
    }
    return cols;
}

struct RawTrack {
    double core = 0.0;
    double env = 0.0;
    std::vector<double> col[Q_COUNT];
};

#ifdef HAVE_CCFITS

[[noreturn]] void fits_fail(const std::string& what, int status) {
    char msg[FLEN_STATUS] = {0};
    fits_get_errstatus(status, msg);
    throw std::runtime_error(what + ": " + msg);
}

// gzip (possibly several concatenated members) -> plain bytes
std::vector<unsigned char> gunzip(const std::vector<unsigned char>& in, const std::string& source) {
    if (in.size() > static_cast<std::size_t>(UINT_MAX))
        throw std::runtime_error("track file '" + source + "' is too large");
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
        throw std::runtime_error("zlib initialisation failed");
    std::vector<unsigned char> out(std::max<std::size_t>(in.size() * 4, 1 << 16));
    std::size_t produced = 0;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    for (;;) {
        if (produced == out.size()) out.resize(out.size() * 2);
        const std::size_t room = std::min<std::size_t>(out.size() - produced, UINT_MAX / 2);
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(room);
        const int ret = inflate(&zs, Z_NO_FLUSH);
        produced += room - zs.avail_out;
        if (ret == Z_STREAM_END) {
            if (zs.avail_in == 0) break;
            inflateReset(&zs);  // another gzip member follows
            continue;
        }
        if (ret == Z_BUF_ERROR && zs.avail_out == 0) continue;  // output full: grow
        if (ret != Z_OK) {
            inflateEnd(&zs);
            throw std::runtime_error("track file '" + source + "' is not valid gzip data" +
                                     (ret == Z_BUF_ERROR ? " (truncated)" : ""));
        }
    }
    inflateEnd(&zs);
    out.resize(produced);
    return out;
}

bool read_column(fitsfile* f, const std::vector<const char*>& names, long nrows,
                 std::vector<double>& out) {
    for (const char* name : names) {
        int status = 0;
        int colnum = 0;
        fits_get_colnum(f, CASEINSEN, const_cast<char*>(name), &colnum, &status);
        if (status != 0) {
            fits_clear_errmsg();
            continue;
        }
        out.assign(static_cast<std::size_t>(nrows), 0.0);
        int anynul = 0;
        double nulval = std::nan("");
        fits_read_col(f, TDOUBLE, colnum, 1, 1, nrows, &nulval, out.data(), &anynul, &status);
        if (status != 0) fits_fail(std::string("reading column ") + name, status);
        return true;
    }
    return false;
}

bool read_string_column(fitsfile* f, const char* name, long nrows, std::vector<std::string>& out) {
    int status = 0;
    int colnum = 0;
    fits_get_colnum(f, CASEINSEN, const_cast<char*>(name), &colnum, &status);
    if (status != 0) {
        fits_clear_errmsg();
        return false;
    }
    int typecode = 0;
    long repeat = 0, width = 0;
    fits_get_coltype(f, colnum, &typecode, &repeat, &width, &status);
    if (status != 0 || typecode != TSTRING) {
        fits_clear_errmsg();
        return false;
    }
    const long w = std::max(repeat, 1L) + 1;
    std::vector<char> storage(static_cast<std::size_t>(w * nrows), '\0');
    std::vector<char*> ptrs(static_cast<std::size_t>(nrows));
    for (long i = 0; i < nrows; ++i) ptrs[static_cast<std::size_t>(i)] = storage.data() + i * w;
    int anynul = 0;
    char nulstr[] = "";
    fits_read_col(f, TSTRING, colnum, 1, 1, nrows, nulstr, ptrs.data(), &anynul, &status);
    if (status != 0) fits_fail(std::string("reading column ") + name, status);
    out.assign(ptrs.begin(), ptrs.end());
    for (auto& s : out) {  // FITS pads strings with blanks
        const std::size_t end = s.find_last_not_of(' ');
        s.erase(end == std::string::npos ? 0 : end + 1);
    }
    return true;
}

// Read a whole image extension as doubles / ints; returns the axis lengths (FITS order).
template <class T>
std::vector<long> read_image(fitsfile* f, const char* extname, int datatype, std::vector<T>& out,
                             const std::string& path) {
    int status = 0;
    fits_movnam_hdu(f, IMAGE_HDU, const_cast<char*>(extname), 0, &status);
    if (status != 0) fits_fail(std::string("extension ") + extname + " missing in " + path, status);
    int naxis = 0;
    fits_get_img_dim(f, &naxis, &status);
    std::vector<long> naxes(static_cast<std::size_t>(std::max(naxis, 1)), 0);
    fits_get_img_size(f, naxis, naxes.data(), &status);
    if (status != 0) fits_fail("reading " + path, status);
    long total = 1;
    for (long n : naxes) total *= n;
    out.assign(static_cast<std::size_t>(total), T());
    int anynul = 0;
    fits_read_img(f, datatype, 1, total, nullptr, out.data(), &anynul, &status);
    if (status != 0) fits_fail(std::string("reading extension ") + extname + " of " + path, status);
    return naxes;
}

#endif  // HAVE_CCFITS

}  // namespace

// A FITS file opened from memory, closed on every exit path.
#ifdef HAVE_CCFITS
struct FitsHandle {
    fitsfile* f = nullptr;
    std::string source;
    std::vector<unsigned char> bytes;
    void* mem = nullptr;
    std::size_t memsize = 0;

    FitsHandle(std::vector<unsigned char> raw, std::string src) : source(std::move(src)) {
        const bool gz = raw.size() >= 2 && raw[0] == 0x1f && raw[1] == 0x8b;
        bytes = gz ? gunzip(raw, source) : std::move(raw);
        if (bytes.empty()) throw std::runtime_error("track file '" + source + "' is empty");
        mem = bytes.data();
        memsize = bytes.size();
        int status = 0;
        fits_open_memfile(&f, "sdtracks", READONLY, &mem, &memsize, 0, nullptr, &status);
        if (status != 0) {
            f = nullptr;
            fits_fail("cannot open track file '" + source + "'", status);
        }
    }
    ~FitsHandle() {
        int status = 0;
        if (f) fits_close_file(f, &status);
    }
    FitsHandle(const FitsHandle&) = delete;
    FitsHandle& operator=(const FitsHandle&) = delete;
};
#else
struct FitsHandle {
    FitsHandle(std::vector<unsigned char>, const std::string& source) {
        throw std::runtime_error("cannot read track file '" + source +
                                 "': ASTRA was built without FITS support (cfitsio)");
    }
};
#endif

// --- loading --------------------------------------------------------------------------

TrackGrid TrackGrid::load(const std::string& path) {
    return load_memory(read_file(path), path);
}

TrackGrid TrackGrid::load_memory(std::vector<unsigned char> bytes, const std::string& source) {
    FitsHandle file(std::move(bytes), source);
#ifdef HAVE_CCFITS
    int status = 0;
    int version = 0;
    fits_read_key(file.f, TINT, "SDTRACKS", &version, nullptr, &status);
    const bool exported = status == 0;
    fits_clear_errmsg();
    return exported ? load_trackset_file(file) : load_fits_file(file);
#else
    return TrackGrid();
#endif
}

TrackGrid TrackGrid::load_trackset(const std::string& path) {
    FitsHandle file(read_file(path), path);
    return load_trackset_file(file);
}

TrackGrid TrackGrid::load_fits(const std::string& path) {
    FitsHandle file(read_file(path), path);
    return load_fits_file(file);
}

#ifndef HAVE_CCFITS
TrackGrid TrackGrid::load_trackset_file(FitsHandle&) { return TrackGrid(); }
TrackGrid TrackGrid::load_fits_file(FitsHandle&) { return TrackGrid(); }
#else

TrackGrid TrackGrid::load_trackset_file(FitsHandle& file) {
    fitsfile* f = file.f;
    const std::string& path = file.source;
    int status = 0;
    int hdutype = 0;
    fits_movabs_hdu(f, 1, &hdutype, &status);
    int ndim = 0;
    char family[FLEN_VALUE] = {0};
    fits_read_key(f, TINT, "NDIM", &ndim, nullptr, &status);
    fits_read_key(f, TSTRING, "FAMILY", family, nullptr, &status);
    if (status != 0) fits_fail("'" + path + "' is not an exported track set", status);
    if (ndim != 1 && ndim != 2) throw std::runtime_error(path + ": NDIM must be 1 or 2");

    std::vector<double> nodes, values;
    const std::vector<long> nshape = read_image(f, "NODES", TDOUBLE, nodes, path);
    const std::vector<long> vshape = read_image(f, "VALUES", TDOUBLE, values, path);
    if (nshape.size() != 2 || nshape[0] != 2 || vshape.size() != 3 || vshape[1] != Q_COUNT ||
        vshape[2] != nshape[1])
        throw std::runtime_error(path + ": unexpected array shapes");
    const std::size_t ntrack = static_cast<std::size_t>(nshape[1]);
    const std::size_t neep = static_cast<std::size_t>(vshape[0]);

    std::vector<int> triangles;
    if (ndim == 2) {
        const std::vector<long> tshape = read_image(f, "TRIANGLES", TINT, triangles, path);
        if (tshape.size() != 2 || tshape[0] != 3)
            throw std::runtime_error(path + ": unexpected TRIANGLES shape");
    }

    // Optional per-track metadata (ASTRA's export_tracks.py): GROUP label, FEH.
    std::vector<std::string> groups;
    std::vector<double> feh;
    {
        int st = 0;
        fits_movnam_hdu(f, BINARY_TBL, const_cast<char*>("TRACKMETA"), 0, &st);
        if (st == 0) {
            long nrows = 0;
            fits_get_num_rows(f, &nrows, &st);
            if (st != 0) fits_fail("reading TRACKMETA of " + path, st);
            if (static_cast<std::size_t>(nrows) != ntrack)
                throw std::runtime_error(path + ": TRACKMETA has a different number of tracks");
            read_string_column(f, "GROUP", nrows, groups);
            if (!read_column(f, {"FEH"}, nrows, feh)) feh.clear();
        } else {
            fits_clear_errmsg();
        }
    }

    std::vector<double> core(ntrack), env(ntrack);
    std::vector<std::vector<std::vector<double>>> columns(ntrack);
    for (std::size_t t = 0; t < ntrack; ++t) {
        core[t] = nodes[2 * t];
        env[t] = nodes[2 * t + 1];
        columns[t].resize(Q_COUNT);
        for (int q = 0; q < Q_COUNT; ++q) {
            const double* p = values.data() + (t * Q_COUNT + q) * neep;
            columns[t][q].assign(p, p + neep);
        }
    }
    TrackGrid g = from_nodes(std::move(core), std::move(env), columns, triangles, ndim);
    g.family_ = family;
    g.source_ = path;
    if (!groups.empty() || !feh.empty()) g.set_node_groups(std::move(groups), std::move(feh));
    return g;
}

TrackGrid TrackGrid::load_fits_file(FitsHandle& file) {
    fitsfile* f = file.f;
    const std::string& path = file.source;
    int status = 0;

    std::vector<RawTrack> tracks;
    int nhdu = 0;
    fits_get_num_hdus(f, &nhdu, &status);
    if (status != 0) fits_fail("reading " + path, status);
    for (int hdu = 2; hdu <= nhdu; ++hdu) {
        int type = 0;
        fits_movabs_hdu(f, hdu, &type, &status);
        if (status != 0) fits_fail("reading " + path, status);
        if (type != BINARY_TBL && type != ASCII_TBL) continue;

        char extname[FLEN_VALUE] = {0};
        fits_read_key(f, TSTRING, "EXTNAME", extname, nullptr, &status);
        if (status != 0) fits_fail("track extension without EXTNAME in " + path, status);
        const std::string name(extname);
        const std::size_t us = name.find_last_of('_');
        const std::string tail = name.substr(us == std::string::npos ? 0 : us + 1);
        char* end = nullptr;
        RawTrack t;
        t.core = std::strtod(tail.c_str(), &end);
        if (end == tail.c_str())
            throw std::runtime_error("cannot parse the core mass from extension name '" + name + "'");

        long nrows = 0;
        fits_get_num_rows(f, &nrows, &status);
        if (status != 0) fits_fail("reading " + path, status);
        if (nrows < 5) throw std::runtime_error("track '" + name + "' has fewer than 5 points");

        std::vector<double> env;
        if (!read_column(f, {"H-envelope", "mass"}, nrows, env))
            throw std::runtime_error("track '" + name + "': no envelope mass column ('mass')");
        t.env = env.front();
        struct {
            Quantity q;
            std::vector<const char*> names;
        } wanted[] = {
            {Q_LOGTEFF, {"log_teff"}}, {Q_LOGG, {"logg"}},     {Q_AGE, {"age"}},
            {Q_LOGL, {"log_l"}},       {Q_RADIUS, {"radius"}}, {Q_HE, {"hec", "He-abundance"}},
        };
        for (auto& w : wanted)
            if (!read_column(f, w.names, nrows, t.col[w.q]))
                throw std::runtime_error("track '" + name + "': missing column '" +
                                         w.names.front() + "'");
        tracks.push_back(std::move(t));
    }
    if (tracks.empty()) throw std::runtime_error("no track tables found in " + path);

    std::vector<double> cores, envs;
    for (const auto& t : tracks) {
        cores.push_back(t.core);
        envs.push_back(t.env);
    }
    cores = sorted_unique(cores);
    envs = sorted_unique(envs);

    const std::size_t nrows = tracks.front().col[0].size();
    std::vector<std::vector<std::vector<double>>> columns(cores.size() * envs.size());
    for (auto& t : tracks) {
        if (t.col[0].size() != nrows)
            throw std::runtime_error("tracks in " + path + " have different lengths");
        const int ic = find_index(cores, t.core);
        const int ie = find_index(envs, t.env);
        auto& slot = columns[static_cast<std::size_t>(ic) * envs.size() + ie];
        if (!slot.empty()) throw std::runtime_error("duplicate track (core, env) in " + path);
        slot.assign(t.col, t.col + Q_COUNT);
    }
    for (const auto& slot : columns)
        if (slot.empty())
            throw std::runtime_error("track grid in " + path +
                                     " is incomplete (not every core/envelope combination "
                                     "is present)");

    TrackGrid g = from_arrays(std::move(cores), std::move(envs), columns);
    g.source_ = path;
    return g;
}

#endif  // HAVE_CCFITS

// --- per-track metadata ---------------------------------------------------------------

const std::string& TrackGrid::node_group(int node) const {
    static const std::string empty;
    return node_group_.empty() ? empty : node_group_.at(static_cast<std::size_t>(node));
}

double TrackGrid::node_feh(int node) const {
    return node_feh_.empty() ? std::numeric_limits<double>::quiet_NaN()
                             : node_feh_.at(static_cast<std::size_t>(node));
}

void TrackGrid::set_node_groups(std::vector<std::string> groups, std::vector<double> feh) {
    const std::size_t n = node_core_.size();
    if (groups.empty() && !feh.empty()) groups.assign(n, std::string());
    if (feh.empty() && !groups.empty()) feh.assign(n, std::numeric_limits<double>::quiet_NaN());
    if (groups.size() != n || feh.size() != n)
        throw std::runtime_error("track set: group labels do not match the number of tracks");
    node_group_ = std::move(groups);
    node_feh_ = std::move(feh);
}

void TrackGrid::copy_meta_from(const TrackGrid& other, const std::vector<int>& kept_nodes) {
    family_ = other.family_;
    source_ = other.source_;
    if (other.node_group_.empty()) return;
    std::vector<std::string> groups;
    std::vector<double> feh;
    for (int node : kept_nodes) {
        groups.push_back(other.node_group_.at(static_cast<std::size_t>(node)));
        feh.push_back(other.node_feh_.at(static_cast<std::size_t>(node)));
    }
    set_node_groups(std::move(groups), std::move(feh));
}

// --- construction ---------------------------------------------------------------------

void TrackGrid::set_data(const std::vector<std::vector<std::vector<double>>>& columns) {
    const std::size_t nnode = node_core_.size();
    if (columns.size() != nnode) throw std::runtime_error("track set: wrong number of tracks");
    n_rows_ = static_cast<int>(columns.front().at(0).size());
    if (n_rows_ < 5) throw std::runtime_error("track set: tracks need at least 5 points");
    data_.assign(nnode * Q_COUNT * n_rows_, 0.0);
    for (std::size_t node = 0; node < nnode; ++node) {
        if (columns[node].size() != Q_COUNT)
            throw std::runtime_error("track set: wrong number of quantities");
        for (int q = 0; q < Q_COUNT; ++q) {
            const auto& c = columns[node][q];
            if (static_cast<int>(c.size()) != n_rows_)
                throw std::runtime_error("track set: tracks have different lengths");
            std::copy(c.begin(), c.end(), data_.begin() + (node * Q_COUNT + q) * n_rows_);
        }
    }
}

void TrackGrid::finish() {
    cores_ = sorted_unique(node_core_);
    envs_ = sorted_unique(node_env_);

    has_helium_ = false;
    for (int node = 0; node < n_nodes() && !has_helium_; ++node) {
        const double* he = column(node, Q_HE);
        for (int i = 0; i < n_rows_; ++i)
            if (std::isfinite(he[i])) {
                has_helium_ = true;
                break;
            }
    }

    // Ages that are tabulated coarser than the EEP spacing (many zero steps) have to be
    // differentiated over a window; same rule as sdfit.priors.age_derivative_window().
    long n_steps = 0, n_zero = 0;
    for (int node = 0; node < n_nodes(); ++node) {
        const double* age = column(node, Q_AGE);
        for (int i = 0; i + 1 < n_rows_; ++i) {
            const double step = age[i + 1] - age[i];
            if (!std::isfinite(step)) continue;
            ++n_steps;
            if (step <= 0) ++n_zero;
        }
    }
    age_window_ = 0;
    if (n_steps > 0) {
        const double zero = static_cast<double>(n_zero) / static_cast<double>(n_steps);
        if (zero >= 0.2) age_window_ = static_cast<int>(std::ceil(10.0 / std::max(1.0 - zero, 0.01)));
    }
}

TrackGrid TrackGrid::from_arrays(std::vector<double> core_masses, std::vector<double> env_masses,
                                 const std::vector<std::vector<std::vector<double>>>& columns) {
    if (core_masses.size() < 2 || env_masses.size() < 2)
        throw std::runtime_error("track grid needs at least 2 core and 2 envelope masses");
    if (!std::is_sorted(core_masses.begin(), core_masses.end()) ||
        !std::is_sorted(env_masses.begin(), env_masses.end()))
        throw std::runtime_error("track grid axes must be sorted");
    TrackGrid g;
    g.geometry_ = Geometry::Rect;
    for (double c : core_masses)
        for (double e : env_masses) {
            g.node_core_.push_back(c);
            g.node_env_.push_back(e);
        }
    g.set_data(columns);
    g.finish();
    g.diag_.assign((g.cores_.size() - 1) * (g.envs_.size() - 1), -1);
    return g;
}

TrackGrid TrackGrid::from_nodes(std::vector<double> node_core, std::vector<double> node_env,
                                const std::vector<std::vector<std::vector<double>>>& columns,
                                const std::vector<int>& triangles, int ndim) {
    TrackGrid g;
    g.node_core_ = std::move(node_core);
    g.node_env_ = std::move(node_env);
    if (g.node_core_.size() != g.node_env_.size() || g.node_core_.size() < 2)
        throw std::runtime_error("track set needs at least two tracks");
    g.set_data(columns);
    g.finish();
    g.mode_ = InterpMode::Triangulated;

    if (ndim == 1) {
        g.geometry_ = Geometry::Line;
        g.line_order_.resize(g.node_core_.size());
        for (std::size_t i = 0; i < g.line_order_.size(); ++i) g.line_order_[i] = static_cast<int>(i);
        std::sort(g.line_order_.begin(), g.line_order_.end(),
                  [&](int a, int b) { return g.node_core_[a] < g.node_core_[b]; });
        return g;
    }

    g.geometry_ = Geometry::Scatter;
    if (triangles.empty() || triangles.size() % 3 != 0)
        throw std::runtime_error("a scattered track set needs a triangulation");
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        Triangle tri;
        for (int k = 0; k < 3; ++k) {
            tri.n[k] = triangles[t + k];
            if (tri.n[k] < 0 || tri.n[k] >= g.n_nodes())
                throw std::runtime_error("triangulation refers to a track that does not exist");
        }
        const double x0 = g.node_core_[tri.n[0]], y0 = g.node_env_[tri.n[0]];
        const double e1x = g.node_core_[tri.n[1]] - x0, e1y = g.node_env_[tri.n[1]] - y0;
        const double e2x = g.node_core_[tri.n[2]] - x0, e2y = g.node_env_[tri.n[2]] - y0;
        const double det = e1x * e2y - e2x * e1y;
        if (det == 0.0 || !std::isfinite(det)) continue;  // degenerate, covers no area
        tri.x0 = x0;
        tri.y0 = y0;
        tri.a = e2y / det;
        tri.b = -e2x / det;
        tri.c = -e1y / det;
        tri.d = e1x / det;
        g.triangles_.push_back(tri);
    }
    if (g.triangles_.empty()) throw std::runtime_error("triangulation has no usable triangles");
    g.build_triangle_index();
    return g;
}

void TrackGrid::build_triangle_index() {
    buckets_ = 32;
    bucket_.assign(static_cast<std::size_t>(buckets_) * buckets_, {});
    const double x0 = core_min(), xs = core_max() - core_min();
    const double y0 = env_min(), ys = env_max() - env_min();
    auto cell = [&](double v, double lo, double span) {
        const int i = static_cast<int>(std::floor((v - lo) / span * buckets_));
        return std::min(std::max(i, 0), buckets_ - 1);
    };
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        for (int n : triangles_[t].n) {
            xmin = std::min(xmin, node_core_[n]);
            xmax = std::max(xmax, node_core_[n]);
            ymin = std::min(ymin, node_env_[n]);
            ymax = std::max(ymax, node_env_[n]);
        }
        for (int ix = cell(xmin, x0, xs); ix <= cell(xmax, x0, xs); ++ix)
            for (int iy = cell(ymin, y0, ys); iy <= cell(ymax, y0, ys); ++iy)
                bucket_[static_cast<std::size_t>(ix) * buckets_ + iy].push_back(static_cast<int>(t));
    }
}

// --- Rect-only operations -------------------------------------------------------------

TrackGrid TrackGrid::select_cores(const std::vector<double>& cores) const {
    if (geometry_ != Geometry::Rect)
        throw std::runtime_error("selecting core masses is only possible on a rectangular grid");
    std::vector<double> keep = sorted_unique(cores);
    std::vector<std::vector<std::vector<double>>> columns;
    std::vector<int> kept_nodes;
    for (double c : keep) {
        const int ic = find_index(cores_, c);
        if (ic < 0) {
            std::ostringstream os;
            os << "core mass " << c << " is not a node of the track grid";
            throw std::runtime_error(os.str());
        }
        for (int ie = 0; ie < n_env(); ++ie) {
            std::vector<std::vector<double>> cols(Q_COUNT);
            for (int q = 0; q < Q_COUNT; ++q) {
                const double* p = column(rect_node(ic, ie), static_cast<Quantity>(q));
                cols[q].assign(p, p + n_rows_);
            }
            columns.push_back(std::move(cols));
            kept_nodes.push_back(rect_node(ic, ie));
        }
    }
    TrackGrid g = from_arrays(keep, envs_, columns);
    g.copy_meta_from(*this, kept_nodes);
    // A triangulation of the full grid says nothing about how scipy would triangulate
    // the subset, so the subset always starts out bilinear.
    return g;
}

// --- Delaunay triangulation -----------------------------------------------------------
//
// sdfit interpolates every 2-D track set with scipy's LinearNDInterpolator, i.e. on
// the Qhull Delaunay triangulation of the (core mass, envelope mass) nodes in raw
// units.  A subset therefore needs its own Delaunay triangulation; cutting triangles
// out of the full set's one leaves holes.  Small node sets (a few hundred tracks), so
// the simple robust route: a triangulation from a lexicographic sweep (orientation
// tests only), then Lawson edge flips until every edge is locally Delaunay.  Only
// exactly co-circular nodes (e.g. a rectangular grid) can be split differently from
// Qhull; both splits are Delaunay.

namespace {

struct Pt {
    double x, y;
};

// > 0 if c lies left of the directed line a -> b
long double orient(const Pt& a, const Pt& b, const Pt& c) {
    return (static_cast<long double>(b.x) - a.x) * (static_cast<long double>(c.y) - a.y) -
           (static_cast<long double>(b.y) - a.y) * (static_cast<long double>(c.x) - a.x);
}

// orient() with points that are collinear up to rounding counted as collinear (0), so
// that no zero-area sliver is created (e.g. Dorman's thickest envelopes, whose nodes lie
// on the line core + envelope = 0.9 Msun)
int orient_sign(const Pt& a, const Pt& b, const Pt& c) {
    const long double o = orient(a, b, c);
    const long double scale = (std::fabs(static_cast<long double>(b.x) - a.x) + std::fabs(static_cast<long double>(b.y) - a.y)) *
                              (std::fabs(static_cast<long double>(c.x) - a.x) + std::fabs(static_cast<long double>(c.y) - a.y));
    if (std::fabs(o) <= 1e-12L * scale) return 0;
    return o > 0 ? 1 : -1;
}

// > 0 if d lies inside the circumcircle of the counter-clockwise triangle abc; `perm`
// receives the matching sum of absolute terms, a scale for the rounding error
long double incircle(const Pt& a, const Pt& b, const Pt& c, const Pt& d, long double& perm) {
    const long double adx = static_cast<long double>(a.x) - d.x, ady = static_cast<long double>(a.y) - d.y;
    const long double bdx = static_cast<long double>(b.x) - d.x, bdy = static_cast<long double>(b.y) - d.y;
    const long double cdx = static_cast<long double>(c.x) - d.x, cdy = static_cast<long double>(c.y) - d.y;
    const long double alift = adx * adx + ady * ady;
    const long double blift = bdx * bdx + bdy * bdy;
    const long double clift = cdx * cdx + cdy * cdy;
    perm = alift * (std::fabs(bdx * cdy) + std::fabs(cdx * bdy)) +
           blift * (std::fabs(cdx * ady) + std::fabs(adx * cdy)) +
           clift * (std::fabs(adx * bdy) + std::fabs(bdx * ady));
    return alift * (bdx * cdy - cdx * bdy) + blift * (cdx * ady - adx * cdy) +
           clift * (adx * bdy - bdx * ady);
}

}  // namespace

std::vector<int> delaunay(const std::vector<double>& xs, const std::vector<double>& ys) {
    const int n = static_cast<int>(std::min(xs.size(), ys.size()));
    if (n < 3) return {};

    // Translation and uniform scaling leave a Delaunay triangulation unchanged; they
    // bring the predicates to numbers of order one.
    double xmin = xs[0], xmax = xs[0], ymin = ys[0], ymax = ys[0];
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) return {};
        xmin = std::min(xmin, xs[i]);
        xmax = std::max(xmax, xs[i]);
        ymin = std::min(ymin, ys[i]);
        ymax = std::max(ymax, ys[i]);
    }
    const double span = std::max(xmax - xmin, ymax - ymin);
    if (!(span > 0.0)) return {};
    std::vector<Pt> p(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) p[i] = {(xs[i] - xmin) / span, (ys[i] - ymin) / span};

    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return p[a].x < p[b].x || (p[a].x == p[b].x && p[a].y < p[b].y);
    });
    // exact duplicates get no triangles
    order.erase(std::unique(order.begin(), order.end(),
                            [&](int a, int b) { return p[a].x == p[b].x && p[a].y == p[b].y; }),
                order.end());
    const int m = static_cast<int>(order.size());
    if (m < 3) return {};

    std::vector<std::array<int, 3>> tri;
    std::map<long long, int> edge;  // directed edge (a, b) -> triangle holding it (CCW)
    auto key = [n](int a, int b) { return static_cast<long long>(a) * n + b; };
    auto set_edges = [&](int t) {
        const auto& v = tri[static_cast<std::size_t>(t)];
        for (int k = 0; k < 3; ++k) edge[key(v[k], v[(k + 1) % 3])] = t;
    };
    auto add = [&](int a, int b, int c) {  // counter-clockwise a, b, c
        tri.push_back({a, b, c});
        set_edges(static_cast<int>(tri.size()) - 1);
    };

    // 1. sweep: a chain of collinear starting points, then the first point off it
    std::vector<int> chain = {order[0], order[1]};
    int k = 2;
    while (k < m && orient_sign(p[chain.front()], p[chain.back()], p[order[k]]) == 0) chain.push_back(order[k++]);
    if (k == m) return {};  // all collinear
    const int q0 = order[k++];
    const bool left = orient_sign(p[chain.front()], p[chain.back()], p[q0]) > 0;
    std::vector<int> hull;  // counter-clockwise
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        if (left) add(chain[i], chain[i + 1], q0);
        else add(chain[i + 1], chain[i], q0);
    }
    if (left) {
        hull = chain;
        hull.push_back(q0);
    } else {
        hull = {chain.front(), q0};
        for (std::size_t i = chain.size() - 1; i >= 1; --i) hull.push_back(chain[i]);
    }

    // every further point is lexicographically largest so far, hence outside the hull:
    // connect it to every hull edge it sees
    for (; k < m; ++k) {
        const int v = order[k];
        const int h = static_cast<int>(hull.size());
        std::vector<char> vis(static_cast<std::size_t>(h));
        int nvis = 0;
        for (int i = 0; i < h; ++i) {
            vis[i] = orient_sign(p[hull[i]], p[hull[(i + 1) % h]], p[v]) < 0;
            nvis += vis[i];
        }
        if (nvis == 0 || nvis == h) continue;  // cannot happen for distinct points; be safe
        // the visible edges form one cyclic run; find its first edge
        int first = 0;
        while (!(vis[first] && !vis[(first + h - 1) % h])) ++first;
        for (int j = 0; j < nvis; ++j) {
            const int i = (first + j) % h;
            add(hull[i], v, hull[(i + 1) % h]);
        }
        // hull: keep hull[first], drop the nvis-1 vertices inside the run, insert v
        std::vector<int> next;
        next.reserve(static_cast<std::size_t>(h - nvis + 2));
        for (int j = 0; j <= h - nvis; ++j) next.push_back(hull[(first + nvis + j) % h]);
        next.push_back(v);
        hull = std::move(next);
    }

    // 2. Lawson flips
    std::vector<std::pair<int, int>> stack;
    for (const auto& t : tri)
        for (int j = 0; j < 3; ++j) stack.emplace_back(t[j], t[(j + 1) % 3]);
    auto third = [&](int t, int a, int b) {
        for (int v : tri[static_cast<std::size_t>(t)])
            if (v != a && v != b) return v;
        return -1;
    };
    long budget = 50L * n * n + 1000;  // termination guard against rounding ping-pong
    while (!stack.empty() && budget-- > 0) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const auto i1 = edge.find(key(a, b));
        const auto i2 = edge.find(key(b, a));
        if (i1 == edge.end() || i2 == edge.end()) continue;  // hull edge or flipped away
        const int t1 = i1->second, t2 = i2->second;
        const int c = third(t1, a, b), d = third(t2, a, b);
        long double perm = 0;
        const long double det = incircle(p[a], p[b], p[c], p[d], perm);
        if (!(det > 1e-12L * perm)) continue;
        if (!(orient_sign(p[a], p[d], p[c]) > 0 && orient_sign(p[d], p[b], p[c]) > 0)) continue;
        for (int t : {t1, t2}) {
            const auto& vv = tri[static_cast<std::size_t>(t)];
            for (int j = 0; j < 3; ++j) edge.erase(key(vv[j], vv[(j + 1) % 3]));
        }
        tri[static_cast<std::size_t>(t1)] = {a, d, c};
        tri[static_cast<std::size_t>(t2)] = {d, b, c};
        set_edges(t1);
        set_edges(t2);
        stack.emplace_back(a, d);
        stack.emplace_back(d, b);
        stack.emplace_back(b, c);
        stack.emplace_back(c, a);
    }

    std::vector<int> out;
    out.reserve(tri.size() * 3);
    for (const auto& t : tri)
        if (orient_sign(p[t[0]], p[t[1]], p[t[2]]) > 0) out.insert(out.end(), t.begin(), t.end());
    return out;
}

std::vector<int> TrackGrid::triangles() const {
    std::vector<int> out;
    out.reserve(triangles_.size() * 3);
    for (const Triangle& tri : triangles_) out.insert(out.end(), tri.n, tri.n + 3);
    return out;
}

// --- subsets of any geometry -----------------------------------------------------------

TrackGrid TrackGrid::select_nodes(const std::vector<bool>& keep) const {
    if (keep.size() != node_core_.size())
        throw std::runtime_error("track subset: one flag per track expected");
    std::vector<int> kept;
    for (int node = 0; node < n_nodes(); ++node)
        if (keep[static_cast<std::size_t>(node)]) kept.push_back(node);
    if (static_cast<int>(kept.size()) == n_nodes()) return *this;
    if (kept.size() < 2) throw std::runtime_error("a track subset needs at least two tracks");

    TrackGrid g;
    if (geometry_ == Geometry::Rect) {
        std::vector<double> kc, ke;
        for (int node : kept) {
            kc.push_back(node_core_[node]);
            ke.push_back(node_env_[node]);
        }
        kc = sorted_unique(kc);
        ke = sorted_unique(ke);
        // every (core, env) pair occurs once in a Rect grid, so equal counts mean the
        // kept tracks are the full product of their core and envelope masses
        if (kc.size() * ke.size() != kept.size())
            throw std::runtime_error(
                "a subset of a rectangular track grid must contain every envelope mass of "
                "each selected core mass (a complete sub-grid)");
        if (kc.size() < 2 || ke.size() < 2)
            throw std::runtime_error(
                "a subset of a rectangular track grid needs at least two core and two "
                "envelope masses");
        std::vector<std::vector<std::vector<double>>> columns;
        std::vector<int> order;
        for (double c : kc)
            for (double e : ke) {
                const int node = rect_node(find_index(cores_, c), find_index(envs_, e));
                columns.push_back(node_columns(*this, node));
                order.push_back(node);
            }
        g = from_arrays(kc, ke, columns);
        g.copy_meta_from(*this, order);
        return g;
    }

    std::vector<double> core, env;
    std::vector<std::vector<std::vector<double>>> columns;
    for (int node : kept) {
        core.push_back(node_core_[node]);
        env.push_back(node_env_[node]);
        columns.push_back(node_columns(*this, node));
    }
    if (geometry_ == Geometry::Line) {
        g = from_nodes(std::move(core), std::move(env), columns, {}, 1);
    } else {
        // sdfit triangulates whatever tracks it loads, so a subset gets a fresh
        // Delaunay triangulation, not the stored one with triangles cut out
        if (sorted_unique(core).size() < 2 || sorted_unique(env).size() < 2)
            throw std::runtime_error(
                "the selected tracks span a single core or envelope mass; a 2-D track set "
                "needs at least two of each");
        const std::vector<int> tris = delaunay(core, env);
        if (tris.empty())
            throw std::runtime_error("the selected tracks cannot be triangulated (collinear)");
        g = from_nodes(std::move(core), std::move(env), columns, tris, 2);
    }
    g.copy_meta_from(*this, kept);
    return g;
}

void TrackGrid::set_mode(InterpMode mode) {
    if (geometry_ != Geometry::Rect) {
        if (mode == InterpMode::Bilinear)
            throw std::runtime_error("bilinear interpolation needs a rectangular track grid; the " +
                                     family_ + " tracks are not on one");
        return;
    }
    if (mode == InterpMode::Triangulated)
        for (signed char d : diag_)
            if (d < 0)
                throw std::runtime_error(
                    "triangulated interpolation requested but no triangulation is loaded");
    mode_ = mode;
}

void TrackGrid::set_cell_diagonal(int icore, int ienv, bool anti) {
    if (geometry_ != Geometry::Rect) throw std::runtime_error("not a rectangular grid");
    diag_.at(static_cast<std::size_t>(icore) * (n_env() - 1) + ienv) = anti ? 1 : 0;
}

void TrackGrid::load_triangulation(const std::string& path) {
    if (geometry_ != Geometry::Rect)
        throw std::runtime_error("this track set already carries its triangulation");
    std::ifstream in(fs_path(path));
    if (!in) throw std::runtime_error("cannot open triangulation file '" + path + "'");
    std::vector<signed char> diag(diag_.size(), -1);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream is(line);
        double v[6];
        int n = 0;
        while (n < 6 && (is >> v[n])) ++n;
        if (n == 0) continue;
        std::ostringstream where;
        where << path << ":" << lineno;
        if (n != 6) throw std::runtime_error(where.str() + ": expected 6 numbers");
        int ic[3], ie[3];
        for (int k = 0; k < 3; ++k) {
            ic[k] = find_index(cores_, v[2 * k]);
            ie[k] = find_index(envs_, v[2 * k + 1]);
            if (ic[k] < 0 || ie[k] < 0)
                throw std::runtime_error(where.str() + ": vertex is not a grid node");
        }
        const int c0 = std::min({ic[0], ic[1], ic[2]}), c1 = std::max({ic[0], ic[1], ic[2]});
        const int e0 = std::min({ie[0], ie[1], ie[2]}), e1 = std::max({ie[0], ie[1], ie[2]});
        if (c1 - c0 != 1 || e1 - e0 != 1)
            throw std::runtime_error(where.str() + ": triangle is not half of a grid cell");
        // Which corner of the cell is missing?  If it is (c0,e0) or (c1,e1) the triangle's
        // hypotenuse is the anti-diagonal, otherwise the main diagonal.
        bool has[2][2] = {{false, false}, {false, false}};
        for (int k = 0; k < 3; ++k) has[ic[k] - c0][ie[k] - e0] = true;
        const bool anti = !has[0][0] || !has[1][1];
        signed char& d = diag[static_cast<std::size_t>(c0) * (n_env() - 1) + e0];
        if (d >= 0 && d != (anti ? 1 : 0))
            throw std::runtime_error(where.str() + ": inconsistent triangles for one cell");
        d = anti ? 1 : 0;
    }
    for (signed char d : diag)
        if (d < 0)
            throw std::runtime_error("triangulation file '" + path +
                                     "' does not cover every grid cell");
    diag_ = std::move(diag);
    mode_ = InterpMode::Triangulated;
}

// --- interpolation --------------------------------------------------------------------

bool TrackGrid::locate(double core, double env, CellWeights& out) const {
    if (geometry_ == Geometry::Line) {
        const int n = static_cast<int>(line_order_.size());
        const double lo_m = node_core_[line_order_.front()], hi_m = node_core_[line_order_.back()];
        if (!(core >= lo_m && core <= hi_m)) return false;
        int hi = 1;
        while (hi < n - 1 && node_core_[line_order_[hi]] <= core) ++hi;
        const int lo = hi - 1;
        const double span = node_core_[line_order_[hi]] - node_core_[line_order_[lo]];
        const double frac = span > 0 ? (core - node_core_[line_order_[lo]]) / span : 0.0;
        out.node[0] = out.node[2] = out.node[3] = line_order_[lo];
        out.node[1] = line_order_[hi];
        out.w[0] = 1.0 - frac;
        out.w[1] = frac;
        out.w[2] = out.w[3] = 0.0;
        return true;
    }

    if (!(core >= cores_.front() && core <= cores_.back() && env >= envs_.front() &&
          env <= envs_.back()))
        return false;  // also catches NaN

    if (geometry_ == Geometry::Scatter) {
        const double xs = core_max() - core_min(), ys = env_max() - env_min();
        int ix = static_cast<int>(std::floor((core - core_min()) / xs * buckets_));
        int iy = static_cast<int>(std::floor((env - env_min()) / ys * buckets_));
        ix = std::min(std::max(ix, 0), buckets_ - 1);
        iy = std::min(std::max(iy, 0), buckets_ - 1);
        // Tolerance on the barycentric coordinates, so that points on shared edges and
        // on the hull are found whatever the rounding (scipy uses a similar epsilon).
        const double eps = 1e-10;
        for (int t : bucket_[static_cast<std::size_t>(ix) * buckets_ + iy]) {
            const Triangle& tri = triangles_[t];
            const double dx = core - tri.x0, dy = env - tri.y0;
            const double l1 = tri.a * dx + tri.b * dy;
            const double l2 = tri.c * dx + tri.d * dy;
            const double l0 = 1.0 - l1 - l2;
            if (l0 < -eps || l1 < -eps || l2 < -eps) continue;
            out.node[0] = tri.n[0];
            out.node[1] = tri.n[1];
            out.node[2] = tri.n[2];
            out.node[3] = tri.n[0];
            out.w[0] = l0;
            out.w[1] = l1;
            out.w[2] = l2;
            out.w[3] = 0.0;
            return true;
        }
        return false;  // inside the bounding box but not covered by tracks
    }

    int ic = static_cast<int>(std::upper_bound(cores_.begin(), cores_.end(), core) -
                              cores_.begin()) - 1;
    ic = std::min(std::max(ic, 0), n_core() - 2);
    int ie = static_cast<int>(std::upper_bound(envs_.begin(), envs_.end(), env) -
                              envs_.begin()) - 1;
    ie = std::min(std::max(ie, 0), n_env() - 2);

    const double u = (core - cores_[ic]) / (cores_[ic + 1] - cores_[ic]);
    const double v = (env - envs_[ie]) / (envs_[ie + 1] - envs_[ie]);

    out.node[0] = rect_node(ic, ie);
    out.node[1] = rect_node(ic + 1, ie);
    out.node[2] = rect_node(ic, ie + 1);
    out.node[3] = rect_node(ic + 1, ie + 1);

    if (mode_ == InterpMode::Bilinear) {
        out.w[0] = (1.0 - u) * (1.0 - v);
        out.w[1] = u * (1.0 - v);
        out.w[2] = (1.0 - u) * v;
        out.w[3] = u * v;
        return true;
    }

    const bool anti = diag_[static_cast<std::size_t>(ic) * (n_env() - 1) + ie] == 1;
    if (!anti) {  // diagonal (0,0)-(1,1)
        if (u >= v) {
            out.w[0] = 1.0 - u;
            out.w[1] = u - v;
            out.w[2] = 0.0;
            out.w[3] = v;
        } else {
            out.w[0] = 1.0 - v;
            out.w[1] = 0.0;
            out.w[2] = v - u;
            out.w[3] = u;
        }
    } else {  // diagonal (1,0)-(0,1)
        if (u + v <= 1.0) {
            out.w[0] = 1.0 - u - v;
            out.w[1] = u;
            out.w[2] = v;
            out.w[3] = 0.0;
        } else {
            out.w[0] = 0.0;
            out.w[1] = 1.0 - v;
            out.w[2] = 1.0 - u;
            out.w[3] = u + v - 1.0;
        }
    }
    return true;
}

double TrackGrid::value_at_row(const CellWeights& cw, Quantity q, int row) const {
    double s = 0.0;
    for (int k = 0; k < 4; ++k) s += cw.w[k] * column(cw.node[k], q)[row];
    return s;
}

double TrackGrid::value(const CellWeights& cw, Quantity q, double eep) const {
    const double last = static_cast<double>(n_rows_ - 1);
    const double pos = eep < 0.0 ? 0.0 : (eep > last ? last : eep);
    int i = static_cast<int>(pos);
    if (i > n_rows_ - 2) i = n_rows_ - 2;
    const double f = pos - i;
    double a = 0.0, b = 0.0;
    for (int k = 0; k < 4; ++k) {
        const double* c = column(cw.node[k], q);
        a += cw.w[k] * c[i];
        b += cw.w[k] * c[i + 1];
    }
    return a * (1.0 - f) + b * f;
}

void TrackGrid::interpolate_column(const CellWeights& cw, Quantity q, double* out) const {
    const double* c0 = column(cw.node[0], q);
    const double* c1 = column(cw.node[1], q);
    const double* c2 = column(cw.node[2], q);
    const double* c3 = column(cw.node[3], q);
    const double w0 = cw.w[0], w1 = cw.w[1], w2 = cw.w[2], w3 = cw.w[3];
    for (int i = 0; i < n_rows_; ++i) out[i] = w0 * c0[i] + w1 * c1[i] + w2 * c2[i] + w3 * c3[i];
}

std::string TrackGrid::describe() const {
    char buf[320];
    if (geometry_ == Geometry::Line) {
        std::snprintf(buf, sizeof buf, "%s: %d tracks, total mass %g..%g Msun, %d points per track",
                      family_.c_str(), n_nodes(), core_min(), core_max(), n_rows_);
    } else if (geometry_ == Geometry::Rect) {
        std::snprintf(buf, sizeof buf,
                      "%s: %d core masses [%g..%g] x %d envelope masses [%g..%g], %d points per "
                      "track, %s interpolation",
                      family_.c_str(), n_core(), core_min(), core_max(), n_env(), env_min(),
                      env_max(), n_rows_,
                      mode_ == InterpMode::Bilinear ? "bilinear" : "scipy-triangulated");
    } else {
        std::snprintf(buf, sizeof buf,
                      "%s: %d tracks, core mass %g..%g, envelope mass %g..%g Msun, %d points per "
                      "track, %zu triangles",
                      family_.c_str(), n_nodes(), core_min(), core_max(), env_min(), env_max(),
                      n_rows_, triangles_.size());
    }
    return buf;
}

}  // namespace sdage
