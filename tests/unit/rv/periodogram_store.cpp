// Periodogram records in data file format version 2 (PeriodogramRecord).
//
// A summary keeps the windows around the reported peaks exactly and a coarse
// envelope elsewhere. What must hold: the windows are bit-exact, the envelope
// never shows less than the true power nor a peak that is not there, a
// summary saved again is unchanged, small and full periodograms are lossless,
// and version 1 files still read.

#include <doctest.h>

#include "app/DataStore.h"
#include "rv/Periodogram.h"
#include "rv/PeriodogramRecord.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <random>

namespace {

using Mode = PeriodogramRecord::StoreMode;

/// Noise plus a few Lorentzian peaks of different widths on `n` bins.
Periodogram::Result synthetic(int n, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::exponential_distribution<double> noise(1.0);
    Periodogram::Result r;
    r.grid = {0.01, 1e-4, n};
    r.nPoints = 4321;
    r.label = QStringLiteral("TESS/T");
    r.frequency.resize(n);
    r.power.resize(n);
    const double centres[] = {0.37 * n, 0.61 * n, 0.12 * n, 0.9 * n};
    const double widths[]  = {3.0, 40.0, 900.0, 1.5};
    const double heights[] = {80.0, 45.0, 30.0, 25.0};
    for (int i = 0; i < n; ++i) {
        r.frequency[i] = r.grid.f0 + i * r.grid.df;
        double p = noise(rng);
        for (int k = 0; k < 4; ++k) {
            const double x = (i - centres[k]) / widths[k];
            p += heights[k] / (1.0 + x * x);
        }
        r.power[i] = p;
    }
    return r;
}

PeriodogramRecord saveLoad(const QString& path, const Periodogram::Result& r, Mode mode)
{
    PeriodogramRecord out;
    out.result = r;
    REQUIRE(out.saveDataToFile(path, mode));
    PeriodogramRecord back;
    REQUIRE(back.loadDataFromFile(path));
    return back;
}

bool samePower(const QVector<double>& a, const QVector<double>& b)
{
    return a.size() == b.size() &&
           std::memcmp(a.constData(), b.constData(), size_t(a.size()) * 8) == 0;
}

bool inExact(const Periodogram::Result& r, int i)
{
    for (const auto& e : r.exact)
        if (i >= e.first && i < e.second) return true;
    return false;
}

QByteArray fileBytes(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}   // namespace

TEST_SUITE("rv")
{

TEST_CASE("Periodogram records: full and classic storage are lossless")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto r = synthetic(300000);
    for (Mode m : {Mode::Full, Mode::Classic}) {
        const auto back = saveLoad(tmp.filePath("pg.asd"), r, m);
        CHECK_FALSE(back.result.isSummary());
        CHECK(samePower(back.result.power, r.power));
        CHECK(back.result.grid.Nf == r.grid.Nf);
        CHECK(back.result.label == r.label);
        CHECK(back.result.nPoints == r.nPoints);
        CHECK(back.result.frequency[1234] == r.frequency[1234]);
    }

    // Small enough for one bin per pool: kept in full even when asked for a summary.
    const auto small = synthetic(5000);
    const auto back = saveLoad(tmp.filePath("small.asd"), small, Mode::Summary);
    CHECK_FALSE(back.result.isSummary());
    CHECK(samePower(back.result.power, small.power));
}

TEST_CASE("Periodogram records: a summary keeps the peaks exactly and never invents one")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto r = synthetic(1200000, 7);
    const QString path = tmp.filePath("pg.asd");
    const auto back = saveLoad(path, r, Mode::Summary).result;
    REQUIRE(back.isSummary());
    CHECK(back.isValid());
    CHECK(QFile(path).size() < qint64(r.power.size()) * 8 / 4);

    // Every peak centre, and each one's half-maximum region, is exact.
    for (double c : {0.37, 0.61, 0.12, 0.9}) {
        const int i = int(c * r.grid.Nf);
        int peak = i;
        for (int j = i - 5; j <= i + 5; ++j)
            if (r.power[j] > r.power[peak]) peak = j;
        CHECK(inExact(back, peak));
        CHECK(back.power[peak] == r.power[peak]);
    }
    int lo = int(0.61 * r.grid.Nf), hi = lo;
    while (r.power[lo] >= 0.5 * r.power[int(0.61 * r.grid.Nf)]) --lo;
    while (r.power[hi] >= 0.5 * r.power[int(0.61 * r.grid.Nf)]) ++hi;
    CHECK(inExact(back, lo));
    CHECK(inExact(back, hi));

    // Outside the windows: never below the truth, and no strict local maxima,
    // so peak search finds only real peaks.
    int strictOutside = 0;
    for (int i = 1; i + 1 < back.grid.Nf; ++i) {
        if (inExact(back, i)) continue;
        CHECK(back.power[i] >= r.power[i]);
        if (back.power[i] > back.power[i - 1] && back.power[i] > back.power[i + 1]) ++strictOutside;
    }
    CHECK(strictOutside == 0);

    // The prior's level: pool means away from the peaks, exact inside them.
    const auto level = Periodogram::meanLevelPower(back);
    REQUIRE(level.size() == back.power.size());
    const int peak = int(0.37 * r.grid.Nf);
    CHECK(level[peak] == back.power[peak]);
    double outsideMax = 0, outsideLevel = 0;
    for (int i = 0; i < back.grid.Nf; ++i)
        if (!inExact(back, i)) { outsideMax += back.power[i]; outsideLevel += level[i]; }
    CHECK(outsideLevel < outsideMax);

    // Saving a summary again changes nothing.
    PeriodogramRecord again;
    again.result = back;
    REQUIRE(again.saveDataToFile(tmp.filePath("again.asd"), Mode::Full));
    CHECK(fileBytes(tmp.filePath("again.asd")) == fileBytes(path));
}

TEST_CASE("Periodogram records: sums of summaries are exact only where every part is")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto a = saveLoad(tmp.filePath("a.asd"), synthetic(800000, 2), Mode::Summary).result;
    const auto b = saveLoad(tmp.filePath("b.asd"), synthetic(800000, 3), Mode::Summary).result;
    const auto sum = Periodogram::weightedSum({a, b}, "TESS");
    REQUIRE(sum.isSummary());
    CHECK(sum.poolMean.size() == a.poolMean.size());
    for (const auto& e : sum.exact)
        for (int i : {e.first, e.second - 1}) {
            CHECK(inExact(a, i));
            CHECK(inExact(b, i));
        }
    const auto product = Periodogram::multiplied({a, b}, "Combined");
    CHECK(product.isSummary());
    CHECK(product.poolMean.isEmpty());

    const auto full = Periodogram::weightedSum({synthetic(800000, 2)}, "x");
    CHECK_FALSE(full.isSummary());
}

TEST_CASE("Periodogram records: version 1 files read")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto r = synthetic(20000);
    QByteArray payload;
    {
        QDataStream s(&payload, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << quint32(1) << r.grid.f0 << r.grid.df << quint32(r.grid.Nf) << qint32(r.nPoints)
          << r.label;
        for (double p : r.power) s << p;
    }
    const QString path = tmp.filePath("v1.asd");
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("ASTR", 4);
        QDataStream out(&f);
        out.setVersion(QDataStream::Qt_6_0);
        out << quint16(1) << quint16(DataStore::PeriodogramData) << qCompress(payload, 1);
    }
    PeriodogramRecord back;
    REQUIRE(back.loadDataFromFile(path));
    CHECK(samePower(back.result.power, r.power));
    CHECK(back.result.label == r.label);
}

}   // TEST_SUITE("rv")
