// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

// Process-level measurements (CPU time, memory, thread count) for Windows,
// Linux and macOS.

#pragma once

#include <cstddef>
#include <string>

namespace bench
{
  struct ProcessSnapshot
  {
    double cpu_seconds     = 0.0;   // user + kernel CPU time of the whole process
    size_t rss_bytes       = 0;     // current resident set / working set
    size_t peak_rss_bytes  = 0;     // peak since process start
    int    threads         = -1;    // number of threads in the process (-1: unknown)
  };

  // include_threads = false skips the (on Windows slow) thread enumeration; use it
  // anywhere the calling thread must not be stalled.
  ProcessSnapshot sampleProcess(bool include_threads = true);

  std::string     osDescription();
  std::string     cpuDescription();
  unsigned        hardwareThreads();

  // Windows only: raise the system timer resolution to 1 ms so that
  // sleep-based pacing is accurate. No-op elsewhere.
  void            raiseTimerResolution();
}
