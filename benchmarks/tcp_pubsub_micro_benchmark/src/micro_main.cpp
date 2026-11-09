// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <benchmark/benchmark.h>

#include <string>

#include "adapter.h"
#include "platform.h"

int main(int argc, char** argv)
{
  // Sleeps between operations must be accurate (Windows: default tick is 15.6 ms)
  bench::raiseTimerResolution();

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  benchmark::AddCustomContext("adapter", bench::adapterName());
  benchmark::AddCustomContext("os",      bench::osDescription());
  benchmark::AddCustomContext("cpu",     bench::cpuDescription());
#ifdef NDEBUG
  benchmark::AddCustomContext("release_build", "true");
#else
  benchmark::AddCustomContext("release_build", "false");
#endif

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
