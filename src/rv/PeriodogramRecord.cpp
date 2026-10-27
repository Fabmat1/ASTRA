#include "rv/PeriodogramRecord.h"
#include "app/AppSettings.h"
#include "app/ArrayCodec.h"
#include "app/DataStore.h"
#include "core/StreamBytes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QDataStream>
#include <QSet>
#include <QStringList>
#include <QIODevice>

namespace {

/*  Coded layouts (DataStore version 2):
 *      u8 kind, f64 f0, f64 df, u32 Nf, i32 nPoints, string label, then
 *      kind 2 (full):    power
 *      kind 1 (summary): u32 pool, pool maxima, pool means,
 *                        u32 windows, each u32 first index + exact power
 *  Arrays through ArrayCodec. The summary envelope has about kTargetPools
 *  pools; the windows cover the peaks PeriodogramPanel::detectPeaks would
 *  report (up to kWindows) down to half their height (at least kMinHalf bins
 *  either side, at most kMaxHalf), which is what estimatePeakAt reads for a
 *  peak's period and error. Windows are whole pools; the pool maxima and
 *  means describe the pools outside them.                                   */
constexpr quint8 kSummary = 1, kFull = 2;
constexpr int kTargetPools = 1 << 17;
constexpr int kWindows = 50;
constexpr int kMinHalf = 64, kMaxHalf = 1 << 16, kMargin = 8;

struct Summary {
    int pool = 0;
    std::vector<double> max, mean;
    std::vector<std::pair<int, std::vector<double>>> windows;
};

std::vector<double> toStd(const QVector<double>& v) { return {v.begin(), v.end()}; }

Summary summarize(const Periodogram::Result& r, int pool)
{
    const int N = r.grid.Nf;
    const int nPools = (N + pool - 1) / pool;
    Summary s;
    s.pool = pool;
    std::vector<char> exact(N, 0);

    if (r.isSummary() && r.pool == pool && int(r.poolMean.size()) == nPools) {
        // Re-saving a summary keeps exactly what it holds: the windows are its
        // exact ranges, the means its own.
        for (const auto& e : r.exact) {
            s.windows.push_back({e.first, std::vector<double>(r.power.begin() + e.first,
                                                              r.power.begin() + e.second)});
            std::fill(exact.begin() + e.first, exact.begin() + e.second, 1);
        }
        s.mean = toStd(r.poolMean);
    } else {
        // The peaks PeriodogramPanel::detectPeaks reports, at both separations
        // the dialogs use, each with its whole half-maximum walk
        // (estimatePeakAt's error window) and a margin.
        std::vector<int> candidates;
        for (int i = 1; i + 1 < N; ++i)
            if (r.power[i] > r.power[i - 1] && r.power[i] > r.power[i + 1]) candidates.push_back(i);
        std::sort(candidates.begin(), candidates.end(),
                  [&](int a, int b) { return r.power[a] > r.power[b]; });
        std::vector<int> peaks;
        for (double sep : {0.05, 0.01}) {
            std::vector<int> chosen;
            for (int i : candidates) {
                if (int(chosen.size()) >= kWindows) break;
                const double fi = r.frequency.value(i, r.grid.f0 + i * r.grid.df);
                const bool close = std::any_of(chosen.begin(), chosen.end(), [&](int j) {
                    const double fj = r.frequency.value(j, r.grid.f0 + j * r.grid.df);
                    return std::abs(fi - fj) / std::max(fi, 1e-30) < sep;
                });
                if (!close) chosen.push_back(i);
            }
            peaks.insert(peaks.end(), chosen.begin(), chosen.end());
        }
        std::vector<std::pair<int, int>> ranges;
        for (int i : peaks) {
            const double half = 0.5 * r.power[i];
            int lo = i, hi = i;
            while (lo > 0 && i - lo < kMaxHalf && r.power[lo] >= half) --lo;
            while (hi < N - 1 && hi - i < kMaxHalf && r.power[hi] >= half) ++hi;
            ranges.push_back({std::max(0, std::min(lo - kMargin, i - kMinHalf)),
                              std::min(N, std::max(hi + kMargin, i + kMinHalf) + 1)});
        }
        // Whole pools only: a pool is then either exact or envelope, and an
        // envelope run is never a single bin (a lone bin between lower
        // neighbours would read as a peak).
        for (auto& w : ranges) {
            w.first  = w.first / pool * pool;
            w.second = std::min(N, (w.second + pool - 1) / pool * pool);
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto& w : ranges) {
            if (!merged.empty() && w.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, w.second);
            else
                merged.push_back(w);
        }
        for (const auto& w : merged) {
            s.windows.push_back({w.first, std::vector<double>(r.power.begin() + w.first,
                                                              r.power.begin() + w.second)});
            std::fill(exact.begin() + w.first, exact.begin() + w.second, 1);
        }
    }

    // The envelope describes the bins outside the windows only: a pool's
    // maximum taken over a window would show the peak a second time beside
    // it. A pool lying wholly inside windows takes its plain maximum (never
    // shown) and mean.
    s.max.assign(nPools, -std::numeric_limits<double>::infinity());
    std::vector<double> sum(nPools, 0.0), allMax(nPools, -std::numeric_limits<double>::infinity()),
        allSum(nPools, 0.0);
    std::vector<int> count(nPools, 0);
    for (int i = 0; i < N; ++i) {
        const int j = i / pool;
        allMax[j] = std::max(allMax[j], r.power[i]);
        allSum[j] += r.power[i];
        if (exact[i]) continue;
        s.max[j] = std::max(s.max[j], r.power[i]);
        sum[j] += r.power[i];
        ++count[j];
    }
    const bool haveMean = !s.mean.empty();
    if (!haveMean) s.mean.assign(nPools, 0.0);
    for (int j = 0; j < nPools; ++j) {
        if (count[j] == 0) s.max[j] = allMax[j];
        if (!haveMean)
            s.mean[j] = count[j] ? sum[j] / count[j]
                                 : allSum[j] / double(std::min(pool, N - j * pool));
    }
    return s;
}

}   // namespace

bool PeriodogramRecord::saveDataToFile(const QString& filepath, StoreMode mode)
{
    if (mode == StoreMode::Classic) {
        QByteArray buf;
        {
            QDataStream s(&buf, QIODevice::WriteOnly);
            s.setVersion(QDataStream::Qt_6_0);
            s << static_cast<quint32>(1);                       // format version
            s << result.grid.f0 << result.grid.df;
            s << static_cast<quint32>(result.grid.Nf);
            s << static_cast<qint32>(result.nPoints);
            s << result.label;
            for (int i = 0; i < result.grid.Nf; ++i) s << result.power[i];
        }
        return DataStore::writeCompressed(filepath, DataStore::PeriodogramData, buf);
    }
    if (mode == StoreMode::Default)
        mode = AppSettings().storeFullPeriodograms() ? StoreMode::Full : StoreMode::Summary;

    const int pool = std::max(1, (result.grid.Nf + kTargetPools - 1) / kTargetPools);
    // A summary can't be widened back to full resolution.
    const bool summary = (mode == StoreMode::Summary || result.isSummary()) && pool > 1;

    QByteArray b;
    StreamBytes::putU8(b, summary ? kSummary : kFull);
    StreamBytes::putF64(b, result.grid.f0);
    StreamBytes::putF64(b, result.grid.df);
    StreamBytes::putU32(b, quint32(result.grid.Nf));
    StreamBytes::putI32(b, result.nPoints);
    StreamBytes::putString(b, result.label);
    if (!summary) {
        ArrayCodec::putF64s(b, toStd(result.power));
    } else {
        const Summary s = summarize(result, pool);
        StreamBytes::putU32(b, quint32(s.pool));
        ArrayCodec::putF64s(b, s.max);
        ArrayCodec::putF64s(b, s.mean);
        StreamBytes::putU32(b, quint32(s.windows.size()));
        for (const auto& w : s.windows) {
            StreamBytes::putU32(b, quint32(w.first));
            ArrayCodec::putF64s(b, w.second);
        }
    }
    return DataStore::writeCoded(filepath, DataStore::PeriodogramData, b);
}

bool PeriodogramRecord::loadDataFromFile(const QString& filepath)
{
    QByteArray buf;
    bool coded = false;
    if (!DataStore::readCompressed(filepath, DataStore::PeriodogramData, buf, &coded))
        return false;

    result = Periodogram::Result{};
    if (coded) {
        StreamBytes::Reader r(buf);
        const quint8 kind = r.u8();
        result.grid.f0 = r.f64();
        result.grid.df = r.f64();
        result.grid.Nf = int(r.u32());
        result.nPoints = r.i32();
        result.label   = r.string();
        const int N = result.grid.Nf;
        if (!r.ok || (kind != kSummary && kind != kFull) || N < 0) return false;
        std::vector<double> v;
        if (kind == kFull) {
            ArrayCodec::getF64s(r, v);
            if (!r.ok || int(v.size()) != N) return false;
            result.power = QVector<double>(v.begin(), v.end());
        } else {
            const int pool = int(r.u32());
            std::vector<double> max, mean;
            ArrayCodec::getF64s(r, max);
            ArrayCodec::getF64s(r, mean);
            const int nPools = pool > 0 ? (N + pool - 1) / pool : -1;
            if (!r.ok || int(max.size()) != nPools || int(mean.size()) != nPools) return false;
            result.pool = pool;
            result.poolMean = QVector<double>(mean.begin(), mean.end());
            result.power.resize(N);
            for (int i = 0; i < N; ++i) result.power[i] = max[i / pool];
            const quint32 nWin = r.u32();
            for (quint32 k = 0; k < nWin && r.ok; ++k) {
                const int first = int(r.u32());
                ArrayCodec::getF64s(r, v);
                if (!r.ok || first < 0 || first + int(v.size()) > N) return false;
                std::copy(v.begin(), v.end(), result.power.begin() + first);
                result.exact.append({first, first + int(v.size())});
            }
            if (!r.ok) return false;
        }
        result.frequency.resize(N);
        for (int i = 0; i < N; ++i) result.frequency[i] = result.grid.f0 + i * result.grid.df;
        return true;
    }

    QDataStream s(&buf, QIODevice::ReadOnly);
    s.setVersion(QDataStream::Qt_6_0);
    quint32 version; s >> version;
    if (version != 1) return false;

    quint32 nf;  qint32 npts;
    s >> result.grid.f0 >> result.grid.df >> nf >> npts >> result.label;
    result.grid.Nf = static_cast<int>(nf);
    result.nPoints = npts;
    result.power.resize(nf);
    result.frequency.resize(nf);
    for (quint32 i = 0; i < nf; ++i) {
        s >> result.power[i];
        result.frequency[i] = result.grid.f0 + i * result.grid.df;
    }
    return s.status() == QDataStream::Ok;
}

namespace PeriodogramUtils {

Periodogram::Result combineForSource(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
    const QString& source)
{
    QList<Periodogram::Result> parts;
    for (const auto& r : records)
        if (r && r->source == source && r->result.isValid())
            parts.append(r->result);
    return Periodogram::weightedSum(parts, source);
}

Periodogram::Result combineForStar(
    const std::vector<std::shared_ptr<PeriodogramRecord>>& records)
{
    QStringList sources;
    QSet<QString> seen;
    for (const auto& r : records) {
        if (!r || !r->result.isValid()) continue;
        if (!seen.contains(r->source)) { seen.insert(r->source); sources << r->source; }
    }
    QList<Periodogram::Result> srcResults;
    for (const QString& s : sources)
        srcResults.append(combineForSource(records, s));
    return Periodogram::multiplied(srcResults, "Combined");
}

}