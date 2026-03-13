#pragma once

#include <array>
#include <cstdint>

namespace openmoq::o_rly::stats {

// ---------------------------------------------------------------------------
// BoundedHistogram
//
// Lightweight fixed-boundary histogram.  Stores per-range (non-cumulative)
// counts internally; fillCumulative() converts to cumulative le-buckets.
//
// Template parameter N is the number of explicit boundaries.  Storage is:
//   buckets[0..N-1] = count of observations in each range
//   buckets[N]      = count of observations above the last boundary (+Inf)
// ---------------------------------------------------------------------------

template <size_t N> struct BoundedHistogram {
  explicit BoundedHistogram(const std::array<uint64_t, N>& b) : bounds(b) {}

  void addValue(uint64_t val) {
    ++count;
    sum += val;
    for (size_t i = 0; i < N; ++i) {
      if (val <= bounds[i]) {
        ++buckets[i];
        return;
      }
    }
    ++buckets[N]; // above all explicit bounds → +Inf bucket
  }

  // Fill a cumulative bucket array of size N+1.
  // cumulative[i] = # observations <= bounds[i]
  template <size_t M> void fillCumulative(std::array<uint64_t, M>& out) const {
    static_assert(M == N + 1, "output array must be N+1");
    uint64_t running = 0;
    for (size_t i = 0; i < N; ++i) {
      running += buckets[i];
      out[i] = running;
    }
    out[N] = count; // every observation is <= +Inf
  }

  const std::array<uint64_t, N>& bounds;
  std::array<uint64_t, N + 1> buckets{};
  uint64_t sum{0};
  uint64_t count{0};
};

} // namespace openmoq::o_rly::stats
