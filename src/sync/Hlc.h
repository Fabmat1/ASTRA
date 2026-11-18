#pragma once

// Hybrid logical clock: the timestamps sync orders changes by.
//
// 48 bits of wall-clock milliseconds, 16 bits of counter. A stamp is never
// smaller than one handed out or seen before, so the order of changes holds
// even when the system clock steps back; as long as clocks are roughly right
// it also matches the order people did things in on different machines.

#include <QDateTime>
#include <QtGlobal>

namespace astra::sync::hlc {

inline constexpr int kCounterBits = 16;

inline qint64 fromWallMs(qint64 ms) { return ms << kCounterBits; }
inline qint64 wallMs(qint64 stamp) { return stamp >> kCounterBits; }

/// The next stamp after `last` (the largest handed out or seen so far).
inline qint64 next(qint64 last, qint64 nowMs = QDateTime::currentMSecsSinceEpoch())
{
    const qint64 wall = fromWallMs(nowMs);
    return wall > last ? wall : last + 1;
}

/// A stamp received from another device, unless it lies further ahead of this
/// machine's clock than `toleranceMs`: such a stamp would win every later
/// comparison, so it is not taken over. Returns the new "largest seen".
inline qint64 observe(qint64 last, qint64 remote,
                      qint64 nowMs = QDateTime::currentMSecsSinceEpoch(),
                      qint64 toleranceMs = 10 * 60 * 1000)
{
    if (wallMs(remote) > nowMs + toleranceMs) return last;
    return remote > last ? remote : last;
}

}   // namespace astra::sync::hlc
