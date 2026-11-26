#include "evolution/TrackSetLibrary.h"

#include "evolution/EvolutionSettings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <map>
#include <stdexcept>

namespace astra::evolution {

namespace {

constexpr double kMassTol = 1e-6;
// sdfit (tracks/dorman/loader.py): Msh scatters by ~0.002 Msun within one
// composition, so a smaller core-mass span is not a usable interpolation axis.
constexpr double kMinDormanCoreSpan = 0.004;

const char* kindName(SubsetKind k)
{
    switch (k) {
    case SubsetKind::Cores: return "cores";
    case SubsetKind::Groups: return "groups";
    default: return "mass_range";
    }
}

QString fmtMass(double m)
{
    return QString::number(m, 'g', 4);
}

int familyOrder(const QString& key)
{
    static const QStringList order = {"han", "nicolas", "dorman", "driebe"};
    const int i = order.indexOf(key);
    return i < 0 ? order.size() : i;
}

QJsonArray toArray(const QVector<double>& v)
{
    QJsonArray a;
    for (double x : v) a.append(x);
    return a;
}

}  // namespace

// ── TrackFamilyInfo ──────────────────────────────────────────────────────────

QString TrackFamilyInfo::summary() const
{
    if (!usable()) return QStringLiteral("%1: %2").arg(displayName, error);
    QString s = QStringLiteral("%1: %2 tracks").arg(displayName).arg(nTracks);
    if (!coreMasses.isEmpty()) {
        const QString what = ndim == 1 ? QStringLiteral("total mass") : QStringLiteral("core mass");
        s += QStringLiteral(", %1 %2-%3 Msun").arg(what, fmtMass(coreMasses.first()), fmtMass(coreMasses.last()));
    }
    if (ndim == 2 && !envMasses.isEmpty())
        s += QStringLiteral(", envelope %1-%2 Msun").arg(fmtMass(envMasses.first()), fmtMass(envMasses.last()));
    if (!groups.isEmpty())
        s += QStringLiteral(", %1 compositions").arg(groups.size());
    s += QStringLiteral(", %1 points per track").arg(nEep);
    if (!triangulationPath.isEmpty()) s += QStringLiteral(", scipy triangulation");
    return s;
}

// ── TrackSubset ──────────────────────────────────────────────────────────────

QJsonObject TrackSubset::toJson(SubsetKind kind) const
{
    QJsonObject o;
    o["kind"] = QString::fromLatin1(kindName(kind));
    switch (kind) {
    case SubsetKind::Cores:
        o["cores"] = toArray(cores);
        break;
    case SubsetKind::MassRange:
        if (std::isfinite(massLo)) o["mass_lo"] = massLo;
        if (std::isfinite(massHi)) o["mass_hi"] = massHi;
        break;
    case SubsetKind::Groups:
        o["groups"] = QJsonArray::fromStringList(groups);
        o["envelopes"] = envelopes;
        break;
    }
    return o;
}

TrackSubset TrackSubset::fromJson(const QJsonObject& o)
{
    TrackSubset s;
    for (const auto& v : o.value("cores").toArray()) s.cores.append(v.toDouble());
    if (o.contains("mass_lo")) s.massLo = o.value("mass_lo").toDouble();
    if (o.contains("mass_hi")) s.massHi = o.value("mass_hi").toDouble();
    for (const auto& v : o.value("groups").toArray()) s.groups.append(v.toString());
    s.envelopes = o.value("envelopes").toInt(3);
    return s;
}

bool TrackSubset::operator==(const TrackSubset& o) const
{
    auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
    return cores == o.cores && same(massLo, o.massLo) && same(massHi, o.massHi) &&
           groups == o.groups && envelopes == o.envelopes;
}

// ── TrackModelSettings ───────────────────────────────────────────────────────

QString TrackModelSettings::eepPriorName(EepPriorChoice p)
{
    switch (p) {
    case EepPriorChoice::Age: return QStringLiteral("age");
    case EepPriorChoice::SdB: return QStringLiteral("sdb");
    case EepPriorChoice::SdO: return QStringLiteral("sdo");
    default: return QStringLiteral("none");
    }
}

QJsonObject TrackModelSettings::toJson() const
{
    QJsonObject o;
    o["eep_prior"] = eepPriorName(eepPrior);
    o["mass_prior"] = massPrior;
    o["walkers"] = walkers;
    o["burn"] = burn;
    o["steps"] = steps;
    o["seed"] = QString::number(seed);  // 64 bit: not exact as a JSON double
    o["han_interp"] = hanScipyInterpolation ? QStringLiteral("scipy") : QStringLiteral("bilinear");
    return o;
}

TrackModelSettings TrackModelSettings::fromJson(const QJsonObject& o)
{
    TrackModelSettings m;
    const QString p = o.value("eep_prior").toString();
    m.eepPrior = p == "age" ? EepPriorChoice::Age
               : p == "sdb" ? EepPriorChoice::SdB
               : p == "sdo" ? EepPriorChoice::SdO
                            : EepPriorChoice::None;
    m.massPrior = o.value("mass_prior").toBool(false);
    m.walkers = o.value("walkers").toInt(m.walkers);
    m.burn = o.value("burn").toInt(m.burn);
    m.steps = o.value("steps").toInt(m.steps);
    bool ok = false;
    const quint64 seed = o.value("seed").toString().toULongLong(&ok);
    if (ok) m.seed = seed;
    m.hanScipyInterpolation = o.value("han_interp").toString() == "scipy";
    return m;
}

sdage::FitConfig TrackModelSettings::toFitConfig() const
{
    sdage::FitConfig fc;
    switch (eepPrior) {
    case EepPriorChoice::Age:
        fc.model.eep_prior = sdage::EepPrior::Age;
        break;
    case EepPriorChoice::SdB:
        fc.model.eep_prior = sdage::EepPrior::Legacy;
        fc.model.eep_shape = sdage::EepPriorShape::Slow;
        break;
    case EepPriorChoice::SdO:
        fc.model.eep_prior = sdage::EepPrior::Legacy;
        fc.model.eep_shape = sdage::EepPriorShape::Fast;
        break;
    default:
        fc.model.eep_prior = sdage::EepPrior::None;
    }
    fc.model.mass_prior = massPrior;
    fc.sampler.n_walkers = walkers;
    fc.sampler.n_burn = burn;
    fc.sampler.n_steps = steps;
    return fc;
}

// ── TrackSetLibrary ──────────────────────────────────────────────────────────

TrackSetLibrary::TrackSetLibrary(QString folder) : _folder(std::move(folder)) {}

TrackSetLibrary& TrackSetLibrary::shared()
{
    static TrackSetLibrary lib;
    lib.setFolder(EvolutionSettings::tracksFolder());
    return lib;
}

void TrackSetLibrary::setFolder(const QString& folder)
{
    QMutexLocker lock(&_mutex);
    _folder = folder;
}

QString TrackSetLibrary::folder() const
{
    QMutexLocker lock(&_mutex);
    return _folder;
}

QString TrackSetLibrary::displayName(const QString& key)
{
    static const QMap<QString, QString> names = {
        {"han", "Han"}, {"nicolas", "Nicolas"}, {"dorman", "Dorman"}, {"driebe", "Driebe"}};
    if (names.contains(key)) return names.value(key);
    if (key.startsWith("han_")) return QStringLiteral("Han (%1)").arg(key.mid(4));
    return key.isEmpty() ? key : key.left(1).toUpper() + key.mid(1);
}

TrackSetLibrary::CacheEntry TrackSetLibrary::loadFile(const QString& path)
{
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();
    {
        QMutexLocker lock(&_mutex);
        const auto it = _cache.constFind(abs);
        if (it != _cache.constEnd() && it->size == fi.size() &&
            it->mtimeMs == fi.lastModified().toMSecsSinceEpoch())
            return *it;
    }
    CacheEntry e;
    e.size = fi.size();
    e.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) {
        e.error = QStringLiteral("cannot read the file (%1)").arg(f.errorString());
    } else {
        const QByteArray bytes = f.readAll();
        e.sha1 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        try {
            std::vector<unsigned char> buf(bytes.begin(), bytes.end());
            e.grid = std::make_shared<const sdage::TrackGrid>(
                sdage::TrackGrid::load_memory(std::move(buf), abs.toStdString()));
        } catch (const std::exception& ex) {
            e.error = QString::fromStdString(ex.what());
        }
    }
    QMutexLocker lock(&_mutex);
    _cache.insert(abs, e);
    return e;
}

TrackFamilyInfo TrackSetLibrary::describe(const QString& key, const QString& path, const QString& tri,
                                          const CacheEntry& e) const
{
    TrackFamilyInfo f;
    f.key = key;
    f.displayName = displayName(key);
    f.path = QFileInfo(path).absoluteFilePath();
    f.fileName = QFileInfo(path).fileName();
    f.triangulationPath = tri;
    f.sha1 = e.sha1;
    f.error = e.error;
    static const QMap<QString, QString> desc = {
        {"han", "Han et al. hot-subdwarf tracks: core x envelope mass grid, with central helium"},
        {"nicolas", "Nicolas' tracks: irregular core / envelope mass grid, no central helium"},
        {"dorman", "Dorman, Rood & O'Connell (1993) HB tracks: compositions x envelopes"},
        {"driebe", "Driebe et al. (1998) He white-dwarf cooling tracks: total mass only"}};
    f.description = desc.value(key.startsWith("han") ? QStringLiteral("han") : key);
    if (!e.grid) return f;

    const sdage::TrackGrid& g = *e.grid;
    f.nTracks = g.n_nodes();
    f.nEep = g.n_rows();
    f.ndim = g.ndim();
    f.hasHelium = g.has_helium();
    f.rectangular = g.geometry() == sdage::Geometry::Rect;
    for (double c : g.core_masses()) f.coreMasses.append(c);
    if (f.ndim == 2)
        for (double v : g.env_masses()) f.envMasses.append(v);
    if (g.has_groups()) {
        std::map<std::string, std::pair<double, int>> groups;  // label -> ([Fe/H], tracks)
        for (int n = 0; n < g.n_nodes(); ++n) {
            auto& entry = groups[g.node_group(n)];
            entry.first = g.node_feh(n);
            ++entry.second;
        }
        std::vector<std::pair<double, std::string>> byFeh;
        for (const auto& [label, v] : groups) {
            byFeh.emplace_back(std::isfinite(v.first) ? v.first : 1e9, label);
            f.maxEnvelopesPerGroup = std::max(f.maxEnvelopesPerGroup, v.second);
        }
        std::sort(byFeh.begin(), byFeh.end());
        for (const auto& [feh, label] : byFeh) {
            f.groups.append(QString::fromStdString(label));
            f.groupFeh.append(feh > 1e8 ? std::numeric_limits<double>::quiet_NaN() : feh);
        }
    }
    f.subsetKind = (f.rectangular || key.startsWith("han")) ? SubsetKind::Cores
                 : (!f.groups.isEmpty() && f.ndim == 2)    ? SubsetKind::Groups
                                                           : SubsetKind::MassRange;
    return f;
}

QVector<TrackFamilyInfo> TrackSetLibrary::families()
{
    const QString dirPath = folder();
    QVector<TrackFamilyInfo> out;
    if (dirPath.isEmpty()) return out;
    QDir dir(dirPath);
    if (!dir.exists()) return out;

    QSet<QString> keys;
    // Han: the original FITS file (rectangular grid, bilinear or scipy-triangulated)
    const QStringList hanFiles = dir.entryList({"han*_extended*.fits.gz", "han*_extended*.fits"},
                                               QDir::Files, QDir::Name);
    for (const QString& name : hanFiles) {
        QString stem = name;
        stem.chop(name.endsWith(".gz") ? 8 : 5);  // ".fits.gz" / ".fits"
        QString key = QStringLiteral("han");
        if (hanFiles.size() > 1 && !stem.endsWith("_extended_925")) {
            const int i = stem.indexOf("extended");
            key = QStringLiteral("han_") + (i >= 0 ? stem.mid(i + 9) : stem);
        }
        if (keys.contains(key)) continue;
        const QString tri = dir.filePath(stem + ".scipy_tri.txt");
        const CacheEntry e = loadFile(dir.filePath(name));
        out.append(describe(key, dir.filePath(name), QFileInfo::exists(tri) ? tri : QString(), e));
        keys.insert(key);
    }
    // exported track sets
    for (const QString& name : dir.entryList({"*.sdtracks.fits.gz", "*.sdtracks.fits"}, QDir::Files,
                                             QDir::Name)) {
        const CacheEntry e = loadFile(dir.filePath(name));
        QString key = e.grid ? QString::fromStdString(e.grid->family())
                             : name.left(name.indexOf(".sdtracks"));
        key = key.toLower();
        if (key.isEmpty() || keys.contains(key)) continue;  // the Han FITS file takes precedence
        out.append(describe(key, dir.filePath(name), QString(), e));
        keys.insert(key);
    }
    std::stable_sort(out.begin(), out.end(), [](const TrackFamilyInfo& a, const TrackFamilyInfo& b) {
        const int oa = familyOrder(a.key), ob = familyOrder(b.key);
        return oa != ob ? oa < ob : a.key < b.key;
    });
    return out;
}

TrackFamilyInfo TrackSetLibrary::family(const QString& key)
{
    for (const TrackFamilyInfo& f : families())
        if (f.key == key) return f;
    return {};
}

TrackSubset TrackSetLibrary::defaultSubset(const TrackFamilyInfo& f)
{
    TrackSubset s;
    s.envelopes = 3;
    if (f.subsetKind == SubsetKind::Cores) s.cores = f.coreMasses;
    if (f.subsetKind == SubsetKind::Groups) s.groups = f.groups;
    if (f.subsetKind == SubsetKind::MassRange && !f.coreMasses.isEmpty()) {
        s.massLo = f.coreMasses.first();
        s.massHi = f.coreMasses.last();
    }
    return s;
}

namespace {

// Is the subset `keep` a usable grid of geometry `g`?  Empty string if so.
QString checkKeep(const sdage::TrackGrid& g, const std::vector<bool>& keep)
{
    std::vector<double> core, env;
    for (int n = 0; n < g.n_nodes(); ++n)
        if (keep[n]) {
            core.push_back(g.node_core(n));
            env.push_back(g.node_env(n));
        }
    if (g.ndim() == 1)
        return core.size() >= 2 ? QString() : QStringLiteral("needs at least two tracks");
    if (core.size() < 3) return QStringLiteral("needs at least three tracks");
    if (sdage::delaunay(core, env).empty())
        return QStringLiteral("the tracks lie on one line (one core or envelope mass)");
    return QString();
}

}  // namespace

std::shared_ptr<const PreparedTrackSet> TrackSetLibrary::prepare(const QString& familyKey,
                                                                 const TrackSubset& subset,
                                                                 const TrackModelSettings& model,
                                                                 QString* whyNot)
{
    auto fail = [whyNot](const QString& msg) {
        if (whyNot) *whyNot = msg;
        return std::shared_ptr<const PreparedTrackSet>();
    };
    const TrackFamilyInfo f = family(familyKey);
    if (f.key.isEmpty())
        return fail(QStringLiteral("The %1 tracks are not in the evolutionary tracks folder.")
                        .arg(displayName(familyKey)));
    if (!f.usable()) return fail(QStringLiteral("%1 tracks: %2").arg(f.displayName, f.error));

    const CacheEntry e = loadFile(f.path);
    if (!e.grid) return fail(QStringLiteral("%1 tracks: %2").arg(f.displayName, e.error));
    auto p = std::make_shared<PreparedTrackSet>();
    p->family = f;
    p->full = e.grid;
    const sdage::TrackGrid& g = *e.grid;
    const int n = g.n_nodes();
    p->keep.assign(static_cast<std::size_t>(n), false);

    // the grid the subset is cut from (Han: optionally with scipy's triangles)
    std::shared_ptr<const sdage::TrackGrid> base = e.grid;
    p->interpolation = f.rectangular ? QStringLiteral("bilinear")
                     : f.ndim == 1   ? QStringLiteral("linear in mass")
                                     : QStringLiteral("Delaunay");
    if (f.rectangular && model.hanScipyInterpolation) {
        if (f.triangulationPath.isEmpty())
            return fail(QStringLiteral("scipy interpolation of the Han grid needs %1 next to the track file.")
                            .arg(QFileInfo(f.path).completeBaseName().section('.', 0, 0) + ".scipy_tri.txt"));
        try {
            auto tri = std::make_shared<sdage::TrackGrid>(g);
            tri->load_triangulation(f.triangulationPath.toStdString());
            base = tri;
            p->interpolation = QStringLiteral("scipy triangles");
        } catch (const std::exception& ex) {
            return fail(QStringLiteral("Han triangulation: %1").arg(QString::fromStdString(ex.what())));
        }
    }

    TrackSubset norm;
    switch (f.subsetKind) {
    case SubsetKind::Cores: {
        const QVector<double> wanted = subset.cores.isEmpty() ? f.coreMasses : subset.cores;
        QVector<double> matched;
        for (double w : wanted) {
            auto it = std::find_if(f.coreMasses.begin(), f.coreMasses.end(),
                                   [w](double c) { return std::fabs(c - w) < kMassTol; });
            if (it == f.coreMasses.end())
                return fail(QStringLiteral("%1 Msun is not a core mass of the %2 tracks.")
                                .arg(fmtMass(w), f.displayName));
            if (!matched.contains(*it)) matched.append(*it);
        }
        std::sort(matched.begin(), matched.end());
        if (matched.size() < 2) return fail(QStringLiteral("Select at least two core masses."));
        for (int i = 0; i < n; ++i)
            p->keep[i] = std::any_of(matched.begin(), matched.end(),
                                     [&](double c) { return std::fabs(g.node_core(i) - c) < kMassTol; });
        norm.cores = matched;
        break;
    }
    case SubsetKind::MassRange: {
        const double lo = std::isfinite(subset.massLo) ? subset.massLo : -1e300;
        const double hi = std::isfinite(subset.massHi) ? subset.massHi : 1e300;
        if (lo > hi) return fail(QStringLiteral("The lower mass limit is above the upper one."));
        auto keepRange = [&](double a, double b) {
            std::vector<bool> k(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) k[i] = g.node_core(i) >= a - kMassTol && g.node_core(i) <= b + kMassTol;
            return k;
        };
        p->keep = keepRange(lo, hi);
        const QString problem = checkKeep(g, p->keep);
        if (!problem.isEmpty()) {
            // suggest the narrowest widening that works: add the next track mass on
            // either side, nearest first, until the subset is usable
            const QVector<double>& m = f.coreMasses;
            int a = int(std::lower_bound(m.begin(), m.end(), lo - kMassTol) - m.begin());
            int b = int(std::upper_bound(m.begin(), m.end(), hi + kMassTol) - m.begin()) - 1;
            if (a > b) {  // no track inside: start from the nearest one
                a = std::min<int>(a, m.size() - 1);
                b = a;
            }
            QString hint;
            while (a > 0 || b < m.size() - 1) {
                const double down = a > 0 ? lo - m[a - 1] : 1e300;
                const double up = b < m.size() - 1 ? m[b + 1] - hi : 1e300;
                if (down <= up) --a;
                else ++b;
                const auto k = keepRange(m[a], m[b]);
                if (checkKeep(g, k).isEmpty()) {
                    hint = QStringLiteral(" Widen it to %1-%2 Msun (%3 tracks).")
                               .arg(fmtMass(m[a]), fmtMass(m[b]))
                               .arg(std::count(k.begin(), k.end(), true));
                    break;
                }
            }
            const long cnt = std::count(p->keep.begin(), p->keep.end(), true);
            return fail(QStringLiteral("%1 track%2 in this mass range: the %3 set %4.%5")
                            .arg(cnt).arg(cnt == 1 ? "" : "s").arg(f.displayName, problem, hint));
        }
        double kmin = 1e300, kmax = -1e300;
        for (int i = 0; i < n; ++i)
            if (p->keep[i]) {
                kmin = std::min(kmin, g.node_core(i));
                kmax = std::max(kmax, g.node_core(i));
            }
        norm.massLo = kmin;
        norm.massHi = kmax;
        break;
    }
    case SubsetKind::Groups: {
        const QStringList wanted = subset.groups.isEmpty() ? f.groups : subset.groups;
        QStringList kept;
        for (const QString& gname : f.groups)  // keep the [Fe/H] order
            if (wanted.contains(gname)) kept.append(gname);
        for (const QString& w : wanted)
            if (!f.groups.contains(w))
                return fail(QStringLiteral("Composition %1 is not in the %2 tracks.").arg(w, f.displayName));
        if (kept.size() < 2)
            return fail(QStringLiteral("Select at least two compositions: in the %1 tracks core mass and "
                                       "composition are degenerate, so one composition gives no core-mass axis.")
                            .arg(f.displayName));
        const int nEnv = subset.envelopes;
        if (nEnv < 0) return fail(QStringLiteral("Invalid number of envelopes."));
        std::map<std::string, std::vector<int>> byGroup;
        for (int i = 0; i < n; ++i) byGroup[g.node_group(i)].push_back(i);
        double cmin = 1e300, cmax = -1e300;
        for (const QString& gname : kept) {
            std::vector<int> nodes = byGroup[gname.toStdString()];
            std::stable_sort(nodes.begin(), nodes.end(),
                             [&](int a, int b) { return g.node_env(a) < g.node_env(b); });
            const std::size_t take = nEnv == 0 ? nodes.size() : std::min<std::size_t>(nodes.size(), nEnv);
            for (std::size_t j = 0; j < take; ++j) {
                p->keep[nodes[j]] = true;
                cmin = std::min(cmin, g.node_core(nodes[j]));
                cmax = std::max(cmax, g.node_core(nodes[j]));
            }
        }
        if (cmax - cmin < kMinDormanCoreSpan)
            return fail(QStringLiteral("These compositions span only %1 Msun in core mass, which is numerical "
                                       "scatter rather than an axis. Select compositions further apart.")
                            .arg(fmtMass(cmax - cmin)));
        const QString problem = checkKeep(g, p->keep);
        if (!problem.isEmpty())
            return fail(QStringLiteral("This selection of the %1 tracks %2.").arg(f.displayName, problem));
        norm.groups = kept;
        norm.envelopes = nEnv;
        break;
    }
    }
    p->subset = norm;

    const bool all = std::all_of(p->keep.begin(), p->keep.end(), [](bool k) { return k; });
    if (all) {
        p->grid = base;
    } else {
        try {
            if (f.rectangular) {
                std::vector<double> cores(norm.cores.begin(), norm.cores.end());
                p->grid = std::make_shared<const sdage::TrackGrid>(base->select_cores(cores));
                if (model.hanScipyInterpolation)
                    p->interpolation = QStringLiteral("bilinear (scipy's triangles cover the full grid only)");
            } else {
                p->grid = std::make_shared<const sdage::TrackGrid>(base->select_nodes(p->keep));
            }
        } catch (const std::exception& ex) {
            return fail(QStringLiteral("%1 tracks: %2").arg(f.displayName, QString::fromStdString(ex.what())));
        }
    }

    const long kept = std::count(p->keep.begin(), p->keep.end(), true);
    QString what;
    switch (f.subsetKind) {
    case SubsetKind::Cores: {
        QStringList c;
        for (double v : norm.cores) c << fmtMass(v);
        what = all ? QStringLiteral("all core masses") : QStringLiteral("core masses %1").arg(c.join(", "));
        break;
    }
    case SubsetKind::MassRange:
        what = all ? QStringLiteral("all tracks")
                   : QStringLiteral("%1 %2-%3 Msun").arg(f.ndim == 1 ? "mass" : "core mass",
                                                        fmtMass(norm.massLo), fmtMass(norm.massHi));
        break;
    case SubsetKind::Groups:
        what = QStringLiteral("%1, %2")
                   .arg(norm.groups.size() == f.groups.size() ? QStringLiteral("all compositions")
                                                              : norm.groups.join(", "),
                        norm.envelopes == 0 ? QStringLiteral("all envelopes")
                                            : QStringLiteral("%1 thinnest envelopes").arg(norm.envelopes));
        break;
    }
    p->summary = QStringLiteral("%1, %2 (%3 of %4 tracks)").arg(f.displayName, what).arg(kept).arg(n);
    return p;
}

TrackCurves TrackSetLibrary::curves(const PreparedTrackSet& p, CurvePlane plane)
{
    TrackCurves c;
    if (!p.full) return c;
    const sdage::TrackGrid& g = *p.full;
    const sdage::Quantity qy = plane == CurvePlane::HRD ? sdage::Q_LOGL : sdage::Q_LOGG;
    for (int n = 0; n < g.n_nodes(); ++n) {
        const double* x = g.column(n, sdage::Q_LOGTEFF);
        const double* y = g.column(n, qy);
        const double* age = g.column(n, sdage::Q_AGE);
        QVector<QPointF> line;
        QVector<double> ages;
        line.reserve(g.n_rows());
        ages.reserve(g.n_rows());
        for (int i = 0; i < g.n_rows(); ++i)
            if (std::isfinite(x[i]) && std::isfinite(y[i])) {
                line.append(QPointF(x[i], y[i]));
                ages.append(age[i]);
            }
        c.lines.append(line);
        c.ages.append(ages);
        c.inSubset.append(n < static_cast<int>(p.keep.size()) && p.keep[n]);
        c.core.append(g.node_core(n));
        c.env.append(g.node_env(n));
        c.group.append(QString::fromStdString(g.node_group(n)));
    }
    return c;
}

}  // namespace astra::evolution
