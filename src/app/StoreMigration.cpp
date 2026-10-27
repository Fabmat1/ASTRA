#include "app/StoreMigration.h"

#include "app/DataStore.h"
#include "app/Logger.h"
#include "rv/PeriodogramRecord.h"
#include "spectra/Spectrum.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <limits>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Options {
    QString root;
    bool    dryRun = false;
    bool    live   = false;
    int     threads = 0;
    QString periodograms = QStringLiteral("summary");
    PeriodogramRecord::StoreMode pgMode = PeriodogramRecord::StoreMode::Summary;
};

/// What happened to the files of one kind.
struct Tally {
    qint64 files = 0;       ///< converted (or, in a dry run, would be)
    qint64 before = 0;      ///< their bytes before
    qint64 after = 0;       ///< and after, shared base files included
    qint64 skipped = 0;     ///< already version 2, or empty
    qint64 mismatched = 0;  ///< decoded differently: left as they were
    qint64 failed = 0;      ///< unreadable or unwritable: left as they were
    void add(const Tally& o)
    {
        files += o.files; before += o.before; after += o.after;
        skipped += o.skipped; mismatched += o.mismatched; failed += o.failed;
    }
};

using Tallies = QMap<QString, Tally>;

bool bitEqual(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * 8) == 0);
}

bool sameFit(const SpectralFit& a, const SpectralFit& b)
{
    return bitEqual(a.modelWavelengths, b.modelWavelengths) &&
           bitEqual(a.modelFluxes, b.modelFluxes) &&
           bitEqual(a.rebinnedFluxes, b.rebinnedFluxes) &&
           bitEqual(a.rebinnedSigmas, b.rebinnedSigmas) &&
           bitEqual(a.modelSplines, b.modelSplines) && a.modelIgnore == b.modelIgnore &&
           bitEqual(a.modelFluxesComp1, b.modelFluxesComp1) &&
           bitEqual(a.modelFluxesComp2, b.modelFluxesComp2) &&
           bitEqual(a.telluricTransmission, b.telluricTransmission) &&
           bitEqual(a.lightFractionComp1, b.lightFractionComp1);
}

enum class Outcome { Converted, Skipped, Mismatch, Failed };

/// A periodogram written as `mode` against the one it was read from: the grid
/// and every power value for full storage; for a summary the grid, the exact
/// windows, and an envelope that is each pool's maximum.
bool samePeriodogram(const Periodogram::Result& a, const Periodogram::Result& b,
                     PeriodogramRecord::StoreMode mode)
{
    if (a.grid.f0 != b.grid.f0 || a.grid.df != b.grid.df || a.grid.Nf != b.grid.Nf ||
        a.nPoints != b.nPoints || a.label != b.label || a.power.size() != b.power.size())
        return false;
    const int N = a.grid.Nf;
    if (!b.isSummary())
        return N == 0 || std::memcmp(a.power.constData(), b.power.constData(), size_t(N) * 8) == 0;
    if (mode != PeriodogramRecord::StoreMode::Summary) return false;
    for (const auto& e : b.exact)
        if (std::memcmp(a.power.constData() + e.first, b.power.constData() + e.first,
                        size_t(e.second - e.first) * 8) != 0)
            return false;
    // Outside the windows each bin shows its pool's maximum over the bins
    // outside the windows: never below the true value, never above that pool.
    std::vector<char> inWindow(N, 0);
    for (const auto& e : b.exact) std::fill(inWindow.begin() + e.first, inWindow.begin() + e.second, 1);
    for (int j = 0; j * b.pool < N; ++j) {
        const int lo = j * b.pool, hi = std::min(N, (j + 1) * b.pool);
        double mx = -std::numeric_limits<double>::infinity();
        for (int i = lo; i < hi; ++i)
            if (!inWindow[i]) mx = std::max(mx, a.power[i]);
        for (int i = lo; i < hi; ++i)
            if (!inWindow[i] && b.power[i] != mx) return false;
    }
    return !b.exact.isEmpty();
}

/// Encodes `src` in version 2 to `dst` and checks that it decodes to exactly
/// what `src` decodes to. `kind` names the file for the report.
Outcome convert(const QString& src, const QString& dst, PeriodogramRecord::StoreMode pgMode,
                QString& kind)
{
    quint16 version = 0, type = 0;
    const bool astr = DataStore::readHeader(src, version, type);
    const QString name = QFileInfo(src).fileName();
    const bool inSpectra = QFileInfo(src).dir().dirName() == QLatin1String("spectra");

    if (!astr) {
        // Uncompressed files from before the container existed; the two
        // loaders below still read them.
        if (inSpectra && name.startsWith(QLatin1String("fit_")))  type = DataStore::SpectralFitData;
        else if (inSpectra && !name.startsWith(QLatin1String("fitbase_"))) type = DataStore::SpectrumData;
        else { kind = QStringLiteral("unknown"); return Outcome::Failed; }
    }

    switch (type) {
    case DataStore::SpectralFitData: kind = QStringLiteral("fit"); break;
    case DataStore::SpectrumData:    kind = QStringLiteral("spectrum"); break;
    case DataStore::PeriodogramData: kind = QStringLiteral("periodogram"); break;
    case DataStore::FitBaseData:     kind = QStringLiteral("fit base"); break;
    default:                         kind = QStringLiteral("other"); break;
    }
    if (astr && version >= 2) return Outcome::Skipped;

    if (type == DataStore::SpectralFitData) {
        SpectralFit orig;
        if (!orig.loadDataFromFile(src)) return Outcome::Failed;
        if (!orig.hasData()) {               // a fit saved without model arrays
            kind = QStringLiteral("empty fit");
            return Outcome::Skipped;
        }
        if (!orig.saveDataToFile(dst)) return Outcome::Failed;
        SpectralFit back;
        if (!back.loadDataFromFile(dst)) return Outcome::Mismatch;
        return sameFit(orig, back) ? Outcome::Converted : Outcome::Mismatch;
    }
    if (type == DataStore::SpectrumData) {
        Spectrum orig;
        if (!orig.loadDataFromFile(src)) return Outcome::Failed;
        if (orig.getWavelengths().empty()) {
            kind = QStringLiteral("empty spectrum");
            return Outcome::Skipped;
        }
        if (!orig.saveDataToFile(dst)) return Outcome::Failed;
        Spectrum back;
        if (!back.loadDataFromFile(dst)) return Outcome::Mismatch;
        return bitEqual(orig.getWavelengths(), back.getWavelengths()) &&
                       bitEqual(orig.getFluxes(), back.getFluxes()) &&
                       bitEqual(orig.getFluxErrors(), back.getFluxErrors())
                   ? Outcome::Converted : Outcome::Mismatch;
    }

    if (type == DataStore::PeriodogramData) {
        PeriodogramRecord orig;
        if (!orig.loadDataFromFile(src)) return Outcome::Failed;
        if (!orig.saveDataToFile(dst, pgMode)) return Outcome::Failed;
        PeriodogramRecord back;
        if (!back.loadDataFromFile(dst)) return Outcome::Mismatch;
        return samePeriodogram(orig.result, back.result, pgMode) ? Outcome::Converted
                                                                  : Outcome::Mismatch;
    }

    // Every other type keeps its payload; only the container changes.
    const auto dt = static_cast<DataStore::DataType>(type);
    QByteArray payload, back;
    if (!DataStore::readCompressed(src, dt, payload)) return Outcome::Failed;
    if (!DataStore::writeCompressed(dst, dt, payload)) return Outcome::Failed;
    if (!DataStore::readCompressed(dst, dt, back)) return Outcome::Mismatch;
    return back == payload ? Outcome::Converted : Outcome::Mismatch;
}

bool replaceFile(const QString& from, const QString& to)
{
    // rename(2) replaces atomically; elsewhere fall back to remove + rename.
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    return QFile::remove(to) && QFile::rename(from, to);
}

qint64 basesSize(const QString& dir, const QSet<QString>& except = {})
{
    qint64 n = 0;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"fitbase_*.asd"}, QDir::Files))
        if (!except.contains(fi.fileName())) n += fi.size();
    return n;
}

/// One star directory. In a dry run the new files go to `scratch`, which is
/// emptied again before returning.
Tallies migrateStar(const QString& starDir, const Options& opt, const QString& scratch,
                    QStringList& problems)
{
    Tallies t;
    QDirIterator subdirs(starDir, QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList dirs{starDir};
    while (subdirs.hasNext()) dirs << subdirs.next();

    for (const QString& dir : dirs) {
        QDir d(dir);
        // Leftovers of an interrupted run.
        for (const QString& f : d.entryList({"*.v2tmp"}, QDir::Files)) d.remove(f);

        const QString outDir = opt.dryRun ? scratch + '/' + d.dirName() : dir;
        if (opt.dryRun) QDir().mkpath(outDir);
        QSet<QString> basesBefore;
        for (const QString& f : QDir(outDir).entryList({"fitbase_*.asd"}, QDir::Files))
            basesBefore.insert(f);

        // Fits last, so a directory's spectra are done before its fits start
        // sharing bases; the order does not matter for correctness.
        QStringList files = d.entryList({"*.asd"}, QDir::Files, QDir::Name);
        std::stable_partition(files.begin(), files.end(),
                              [](const QString& f) { return !f.startsWith(QLatin1String("fit_")); });

        for (const QString& f : files) {
            if (f.startsWith(QLatin1String("fitbase_"))) continue;
            const QString src = d.filePath(f);
            const QString dst = outDir + '/' + f + QStringLiteral(".v2tmp");
            QString kind;
            const qint64 before = QFileInfo(src).size();
            const Outcome o = convert(src, dst, opt.pgMode, kind);
            Tally& k = t[kind];
            switch (o) {
            case Outcome::Skipped:
                ++k.skipped;
                break;
            case Outcome::Mismatch:
                ++k.mismatched;
                problems << QStringLiteral("MISMATCH %1").arg(src);
                QFile::remove(dst);
                break;
            case Outcome::Failed:
                ++k.failed;
                problems << QStringLiteral("FAILED %1").arg(src);
                QFile::remove(dst);
                break;
            case Outcome::Converted: {
                ++k.files;
                k.before += before;
                k.after += QFileInfo(dst).size();
                if (opt.dryRun) {
                    QFile::remove(dst);
                } else if (!replaceFile(dst, src)) {
                    --k.files;
                    k.before -= before;
                    ++k.failed;
                    problems << QStringLiteral("FAILED to replace %1").arg(src);
                    QFile::remove(dst);
                }
                break;
            }
            }
        }

        // Base files written for this directory's fits count to the fits.
        if (!opt.dryRun) SpectralFit::pruneFitBases(dir, 0);
        t[QStringLiteral("fit")].after += basesSize(outDir, basesBefore);
        if (opt.dryRun) QDir(outDir).removeRecursively();
    }
    return t;
}

QString gb(qint64 bytes) { return QString::number(double(bytes) / 1e9, 'f', 2); }

int usage()
{
    std::fprintf(stderr,
                 "usage: --migrate-store <root> [--dry-run] [--threads N]\n"
                 "                       [--periodograms summary|full|keep]\n"
                 "                       [--i-know-this-is-live]\n");
    return 2;
}

}   // namespace

int runStoreMigration(int argc, char** argv)
{
    if (argc < 3) return usage();
    Options opt;
    opt.root = QString::fromLocal8Bit(argv[2]);
    for (int i = 3; i < argc; ++i) {
        const QByteArray a = argv[i];
        if (a == "--dry-run") opt.dryRun = true;
        else if (a == "--i-know-this-is-live") opt.live = true;
        else if (a == "--threads" && i + 1 < argc) opt.threads = std::atoi(argv[++i]);
        else if (a == "--periodograms" && i + 1 < argc) opt.periodograms = argv[++i];
        else return usage();
    }
    if (opt.periodograms == QLatin1String("summary")) opt.pgMode = PeriodogramRecord::StoreMode::Summary;
    else if (opt.periodograms == QLatin1String("full")) opt.pgMode = PeriodogramRecord::StoreMode::Full;
    else if (opt.periodograms == QLatin1String("keep")) opt.pgMode = PeriodogramRecord::StoreMode::Classic;
    else return usage();

    QCoreApplication app(argc, argv);
    // Loading a fit logs a line; a store holds hundreds of thousands.
    Logger::instance()->setMinLevel(LogLevel::Warning);
    const QDir root(opt.root);
    const QString stars = root.filePath(QStringLiteral("data/stars"));
    if (!QDir(stars).exists()) {
        std::fprintf(stderr, "%s has no data/stars\n", qPrintable(opt.root));
        return 1;
    }
    if (!opt.dryRun && !opt.live) {
        for (const char* f : {"astra.db-wal", "astra.db-shm"}) {
            if (root.exists(QString::fromLatin1(f))) {
                std::fprintf(stderr, "%s exists: ASTRA has this database open (or did not "
                                     "close cleanly). Close it, or open and close it once.\n",
                             qPrintable(root.filePath(QString::fromLatin1(f))));
                return 1;
            }
        }
        if (root.exists(QStringLiteral(".stfolder"))) {
            std::fprintf(stderr, "%s is shared by Syncthing; migrate a copy, or pass "
                                 "--i-know-this-is-live\n", qPrintable(opt.root));
            return 1;
        }
    }

    const QStringList starDirs =
        QDir(stars).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QTemporaryDir scratch;
    if (opt.dryRun && !scratch.isValid()) {
        std::fprintf(stderr, "no scratch directory for the dry run\n");
        return 1;
    }
    QThreadPool pool;
    if (opt.threads > 0) pool.setMaxThreadCount(opt.threads);
    std::printf("%s %d star directories in %s with %d threads, periodograms: %s\n",
                opt.dryRun ? "dry run over" : "migrating", int(starDirs.size()),
                qPrintable(stars), pool.maxThreadCount(), qPrintable(opt.periodograms));
    std::fflush(stdout);

    QMutex mutex;
    Tallies total;
    QStringList problems;
    std::atomic<int> done{0};
    QElapsedTimer clock;
    clock.start();
    qint64 lastReport = 0;

    std::vector<QString> work(starDirs.begin(), starDirs.end());
    QtConcurrent::blockingMap(&pool, work, [&](const QString& s) {
        QStringList mine;
        const QString starScratch = scratch.path() + '/' + s;
        const Tallies t = migrateStar(QDir(stars).filePath(s), opt, starScratch, mine);
        if (opt.dryRun) QDir(starScratch).removeRecursively();
        QMutexLocker lock(&mutex);
        for (auto it = t.begin(); it != t.end(); ++it) total[it.key()].add(it.value());
        problems << mine;
        const int n = ++done;
        if (clock.elapsed() - lastReport > 30000 || n == int(work.size())) {
            lastReport = clock.elapsed();
            qint64 b = 0, a = 0;
            for (const Tally& x : total) { b += x.before; a += x.after; }
            std::printf("  %d/%d stars, %s GB -> %s GB so far, %lld s\n", n, int(work.size()),
                        qPrintable(gb(b)), qPrintable(gb(a)), (long long)(clock.elapsed() / 1000));
            std::fflush(stdout);
        }
    });

    std::printf("\n%-12s %9s %10s %10s %6s %9s %9s %7s\n", "kind", "files", "before GB",
                "after GB", "ratio", "skipped", "mismatch", "failed");
    Tally all;
    for (auto it = total.begin(); it != total.end(); ++it) {
        const Tally& x = it.value();
        all.add(x);
        std::printf("%-12s %9lld %10s %10s %6.2f %9lld %9lld %7lld\n", qPrintable(it.key()),
                    (long long)x.files, qPrintable(gb(x.before)), qPrintable(gb(x.after)),
                    x.before ? double(x.after) / double(x.before) : 0.0, (long long)x.skipped,
                    (long long)x.mismatched, (long long)x.failed);
    }
    std::printf("%-12s %9lld %10s %10s %6.2f %9lld %9lld %7lld\n", "total", (long long)all.files,
                qPrintable(gb(all.before)), qPrintable(gb(all.after)),
                all.before ? double(all.after) / double(all.before) : 0.0, (long long)all.skipped,
                (long long)all.mismatched, (long long)all.failed);
    for (const QString& p : problems) std::printf("%s\n", qPrintable(p));
    std::printf("%s\n", opt.dryRun ? "dry run: nothing was written to the store"
                                    : "files that did not compare equal were left as they were");
    return all.mismatched || all.failed ? 1 : 0;
}
