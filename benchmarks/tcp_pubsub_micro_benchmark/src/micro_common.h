// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

#include <benchmark/benchmark.h>

namespace microbench
{
  using Clock = std::chrono::steady_clock;

  // Idle time that represents a 100 Hz stream (workload W-1)
  constexpr int64_t kStreamGapUs = 10000;

  inline double seconds(const Clock::duration d)
  {
    return std::chrono::duration<double>(d).count();
  }

  inline int64_t nowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  }

  // Sleeps (the benchmark main raises the Windows timer resolution to 1 ms)
  inline void idle(const int64_t gap_us)
  {
    if (gap_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(gap_us));
  }

  inline void cpuRelax()
  {
#if defined(_M_ARM64)
    __yield();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
  }

  // Busy-waits on the benchmark thread. Used to wait for an operation to finish
  // *outside* of the measured time without adding a sleep/wake-up to it.
  template <class Predicate>
  void spinWait(Predicate predicate)
  {
    int spins = 0;
    while (!predicate())
    {
      if (++spins < 4000) cpuRelax();
      else                std::this_thread::yield();
    }
  }

  // Number of iterations for manually timed benchmarks: a stream gap needs few
  // (every iteration takes the gap), back-to-back runs are bounded by the data volume.
  inline int64_t iterationsFor(const int64_t size, const int64_t gap_us)
  {
    if (gap_us > 0) return 300;
    const int64_t by_volume = 2000000000LL / std::max<int64_t>(size, 64);
    return std::max<int64_t>(300, std::min<int64_t>(20000, by_volume));
  }

  // Registers fn once for every size and for the two gaps (back to back and
  // 100 Hz stream). Arguments seen by fn: range(0) = size, range(1) = gap_us.
  // All of them are manually timed (state.SetIterationTime).
  inline void registerGapped(const char* name, void (*fn)(benchmark::State&), const std::vector<int64_t>& sizes)
  {
    for (const int64_t gap_us : {int64_t(0), kStreamGapUs})
    {
      for (const int64_t size : sizes)
      {
        benchmark::Benchmark* b = benchmark::RegisterBenchmark(name, fn);
        b->ArgNames({"size", "gap_us"})->Args({size, gap_us})->UseManualTime()->Iterations(iterationsFor(size, gap_us));
      }
    }
  }
}
