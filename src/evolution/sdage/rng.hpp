// Imported from sdage (SdAgeInterpolation @ df5cced, 2026-11-13), the C++ port of
// H. Dawson's sdfit 1.0.0 hot-subdwarf track fitting. ASTRA keeps it in namespace
// sdage; see tracks.hpp for the list of changes made here.

// Small, fast, reproducible random number generator (xoshiro256++).
//
// std::normal_distribution & friends are implementation defined, so results would
// differ between compilers.  Everything here is fully specified, which makes a fit
// reproducible for a given seed on every platform.
#pragma once

#include <cmath>
#include <cstdint>

namespace sdage {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        // splitmix64 expands the seed into the 256 bit state
        for (auto& word : s_) {
            seed += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            word = z ^ (z >> 31);
        }
        has_spare_ = false;
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    /// Uniform in [0, 1).
    double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    /// Uniform in [lo, hi).
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

    /// Uniform integer in [0, n).
    std::uint64_t below(std::uint64_t n) {
        // multiply-shift; the bias is < 2^-53 for any n we use
        return static_cast<std::uint64_t>(uniform() * static_cast<double>(n));
    }

    /// Standard normal deviate (Marsaglia polar method).
    double normal() {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        double u, v, r2;
        do {
            u = 2.0 * uniform() - 1.0;
            v = 2.0 * uniform() - 1.0;
            r2 = u * u + v * v;
        } while (r2 >= 1.0 || r2 == 0.0);
        const double f = std::sqrt(-2.0 * std::log(r2) / r2);
        spare_ = v * f;
        has_spare_ = true;
        return u * f;
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    std::uint64_t s_[4]{};
    double spare_ = 0.0;
    bool has_spare_ = false;
};

/// Derive an independent stream seed from a base seed and an index (e.g. star number),
/// so that results do not depend on the number of threads or the processing order.
inline std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t index) {
    std::uint64_t z = seed * 0x9E3779B97F4A7C15ULL + (index + 1) * 0xD1B54A32D192ED03ULL;
    z = (z ^ (z >> 32)) * 0xDABA0B6EB09322E3ULL;
    z = (z ^ (z >> 32)) * 0xDABA0B6EB09322E3ULL;
    return z ^ (z >> 32);
}

}  // namespace sdage
