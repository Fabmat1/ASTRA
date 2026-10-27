#ifndef ARRAYCODEC_H
#define ARRAYCODEC_H

#include <QByteArray>

#include <cstdint>
#include <vector>

#include "core/StreamBytes.h"

/*  Lossless compression of the numeric arrays in ASTRA's data files (format
 *  version 2 of DataStore).
 *
 *  zlib on raw doubles gains almost nothing: the low mantissa bytes are noise.
 *  What does compress is structure, so each array is stored as its IEEE bit
 *  patterns run through whichever of four transforms leaves the smallest
 *  result (none, XOR with the previous value, first or second integer
 *  difference), byte-shuffled so the bytes of one significance sit together,
 *  then zstd. An array whose every value is exactly a float32 (many archives
 *  ship float32 fluxes) is stored in 4-byte lanes. Everything works on the bit
 *  patterns, so NaN payloads, ±Inf, -0.0 and denormals come back unchanged.
 *
 *  A record is: u8 tag, u32 count, u32 byte length, the bytes.               */
namespace ArrayCodec {

/// Appends `v` as one record.
void putF64s(QByteArray& out, const std::vector<double>& v);

/// Reads one record. A truncated or corrupt record sets r.ok to false and
/// leaves `v` empty.
void getF64s(StreamBytes::Reader& r, std::vector<double>& v);

/// 0/1 flags packed eight to a byte; any other value stores the bytes as they
/// are. Then zstd.
void putMask(QByteArray& out, const std::vector<uint8_t>& v);
void getMask(StreamBytes::Reader& r, std::vector<uint8_t>& v);

/// zstd at the level the data files use. `decompress` checks the frame's
/// stated size against `expected` (pass -1 to accept the frame's own size)
/// and returns false on any mismatch or error.
QByteArray compress(const char* data, qsizetype size);
inline QByteArray compress(const QByteArray& b) { return compress(b.constData(), b.size()); }
bool decompress(const char* data, qsizetype size, QByteArray& out, qint64 expected = -1);

/// A stable 64-bit FNV-1a hash of raw bytes (the fit sidecars' address).
quint64 hash64(const char* data, qsizetype size, quint64 seed = 0xcbf29ce484222325ull);

}   // namespace ArrayCodec

#endif // ARRAYCODEC_H
