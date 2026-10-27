#include "app/ArrayCodec.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace ArrayCodec {
namespace {

// zstd-3 is within a few percent of zstd-19 on these arrays at a tenth of the time.
constexpr int kLevel = 3;

// Tag: the low two bits name the transform, bit 2 says the lanes are float32.
enum : quint8 { Plain = 0, Xor = 1, Delta1 = 2, Delta2 = 3, Float32Lanes = 4 };

// Above this many values the transform is chosen on a prefix, not by
// compressing the whole array four times.
constexpr size_t kTrialLimit = 1u << 18;
constexpr size_t kTrialSample = 1u << 16;

template <typename U>
void forward(std::vector<U>& x, int t)
{
    const size_t n = x.size();
    if (t == Xor) {
        for (size_t i = n; i-- > 1;) x[i] ^= x[i - 1];
    } else if (t == Delta1 || t == Delta2) {
        for (int pass = 0; pass < (t == Delta2 ? 2 : 1); ++pass)
            for (size_t i = n; i-- > 1;) x[i] -= x[i - 1];
    }
}

template <typename U>
void inverse(std::vector<U>& x, int t)
{
    const size_t n = x.size();
    if (t == Xor) {
        for (size_t i = 1; i < n; ++i) x[i] ^= x[i - 1];
    } else if (t == Delta1 || t == Delta2) {
        for (int pass = 0; pass < (t == Delta2 ? 2 : 1); ++pass)
            for (size_t i = 1; i < n; ++i) x[i] += x[i - 1];
    }
}

// Byte k (by significance, so independent of the host's byte order) of every
// value, then byte k+1, ...
template <typename U>
QByteArray shuffled(const std::vector<U>& x)
{
    constexpr int w = sizeof(U);
    const size_t n = x.size();
    QByteArray b(qsizetype(n * w), Qt::Uninitialized);
    char* p = b.data();
    for (int k = 0; k < w; ++k)
        for (size_t i = 0; i < n; ++i) *p++ = char((x[i] >> (8 * k)) & 0xff);
    return b;
}

template <typename U>
void unshuffled(const QByteArray& b, std::vector<U>& x)
{
    constexpr int w = sizeof(U);
    const size_t n = size_t(b.size()) / w;
    x.assign(n, 0);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(b.constData());
    for (int k = 0; k < w; ++k)
        for (size_t i = 0; i < n; ++i) x[i] |= U(*p++) << (8 * k);
}

template <typename U>
QByteArray encoded(std::vector<U> x, int t)
{
    forward(x, t);
    return compress(shuffled(x));
}

template <typename U>
int bestTransform(const std::vector<U>& lanes)
{
    std::vector<U> trial = lanes.size() > kTrialLimit
        ? std::vector<U>(lanes.begin(), lanes.begin() + kTrialSample) : lanes;
    int best = Plain;
    qsizetype bestSize = -1;
    for (int t : {Plain, Xor, Delta1, Delta2}) {
        const qsizetype s = encoded(trial, t).size();
        if (bestSize < 0 || s < bestSize) { best = t; bestSize = s; }
    }
    return best;
}

void putRecord(QByteArray& out, quint8 tag, quint32 count, const QByteArray& bytes)
{
    StreamBytes::putU8(out, tag);
    StreamBytes::putU32(out, count);
    StreamBytes::putU32(out, quint32(bytes.size()));
    out.append(bytes);
}

ZSTD_CCtx* cctx()
{
    thread_local struct Holder {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        ~Holder() { ZSTD_freeCCtx(c); }
    } h;
    return h.c;
}

ZSTD_DCtx* dctx()
{
    thread_local struct Holder {
        ZSTD_DCtx* d = ZSTD_createDCtx();
        ~Holder() { ZSTD_freeDCtx(d); }
    } h;
    return h.d;
}

}   // namespace

QByteArray compress(const char* data, qsizetype size)
{
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(size))), Qt::Uninitialized);
    const size_t n = ZSTD_compressCCtx(cctx(), out.data(), size_t(out.size()), data,
                                       size_t(size), kLevel);
    if (ZSTD_isError(n)) return {};
    out.resize(qsizetype(n));
    return out;
}

bool decompress(const char* data, qsizetype size, QByteArray& out, qint64 expected)
{
    out.clear();
    const unsigned long long stated = ZSTD_getFrameContentSize(data, size_t(size));
    if (stated == ZSTD_CONTENTSIZE_ERROR || stated == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    if (expected >= 0 && stated != (unsigned long long)expected) return false;
    if (stated > (unsigned long long)std::numeric_limits<qsizetype>::max() / 2) return false;
    out.resize(qsizetype(stated));
    const size_t n = ZSTD_decompressDCtx(dctx(), out.data(), size_t(out.size()), data,
                                         size_t(size));
    if (ZSTD_isError(n) || n != stated) {
        out.clear();
        return false;
    }
    return true;
}

quint64 hash64(const char* data, qsizetype size, quint64 seed)
{
    quint64 h = seed;
    for (qsizetype i = 0; i < size; ++i) {
        h ^= quint64(static_cast<unsigned char>(data[i]));
        h *= 0x100000001b3ull;
    }
    return h;
}

void putF64s(QByteArray& out, const std::vector<double>& v)
{
    if (v.empty()) {
        putRecord(out, Plain, 0, {});
        return;
    }
    const size_t n = v.size();
    std::vector<quint64> bits(n);
    std::memcpy(bits.data(), v.data(), n * 8);

    // Exactly float32 when every value survives the round trip bit for bit.
    bool f32 = true;
    std::vector<quint32> narrow(n);
    for (size_t i = 0; i < n && f32; ++i) {
        const float f = float(v[i]);
        const double back = double(f);
        quint64 b;
        std::memcpy(&b, &back, 8);
        std::memcpy(&narrow[i], &f, 4);
        f32 = (b == bits[i]);
    }

    if (f32) {
        const int t = bestTransform(narrow);
        putRecord(out, quint8(t | Float32Lanes), quint32(n), encoded(std::move(narrow), t));
    } else {
        const int t = bestTransform(bits);
        putRecord(out, quint8(t), quint32(n), encoded(std::move(bits), t));
    }
}

void getF64s(StreamBytes::Reader& r, std::vector<double>& v)
{
    v.clear();
    const quint8 tag = r.u8();
    const quint32 count = r.u32();
    const quint32 bytes = r.u32();
    if (!r.ok || tag > (Delta2 | Float32Lanes) || !r.take(bytes)) {
        r.ok = false;
        return;
    }
    const char* p = r.p;
    r.p += bytes;
    if (count == 0) return;

    const bool f32 = tag & Float32Lanes;
    const int t = tag & 3;
    QByteArray raw;
    if (!decompress(p, bytes, raw, qint64(count) * (f32 ? 4 : 8))) {
        r.ok = false;
        return;
    }
    v.resize(count);
    if (f32) {
        std::vector<quint32> lanes;
        unshuffled(raw, lanes);
        inverse(lanes, t);
        for (quint32 i = 0; i < count; ++i) {
            float f;
            std::memcpy(&f, &lanes[i], 4);
            v[i] = double(f);
        }
    } else {
        std::vector<quint64> lanes;
        unshuffled(raw, lanes);
        inverse(lanes, t);
        std::memcpy(v.data(), lanes.data(), size_t(count) * 8);
    }
}

void putMask(QByteArray& out, const std::vector<uint8_t>& v)
{
    const bool binary = std::all_of(v.begin(), v.end(), [](uint8_t x) { return x <= 1; });
    QByteArray body;
    if (binary) {
        body.fill(0, qsizetype((v.size() + 7) / 8));
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i]) body[qsizetype(i / 8)] = char(body[qsizetype(i / 8)] | (1 << (i % 8)));
    } else {
        body = QByteArray(reinterpret_cast<const char*>(v.data()), qsizetype(v.size()));
    }
    putRecord(out, binary ? 0 : 1, quint32(v.size()), v.empty() ? QByteArray() : compress(body));
}

void getMask(StreamBytes::Reader& r, std::vector<uint8_t>& v)
{
    v.clear();
    const quint8 tag = r.u8();
    const quint32 count = r.u32();
    const quint32 bytes = r.u32();
    if (!r.ok || tag > 1 || !r.take(bytes)) {
        r.ok = false;
        return;
    }
    const char* p = r.p;
    r.p += bytes;
    if (count == 0) return;

    QByteArray body;
    if (!decompress(p, bytes, body, tag == 0 ? qint64((count + 7) / 8) : qint64(count))) {
        r.ok = false;
        return;
    }
    v.resize(count);
    if (tag == 0) {
        for (quint32 i = 0; i < count; ++i)
            v[i] = (body[qsizetype(i / 8)] >> (i % 8)) & 1;
    } else {
        std::memcpy(v.data(), body.constData(), count);
    }
}

}   // namespace ArrayCodec
