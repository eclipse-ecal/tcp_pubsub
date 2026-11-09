// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "adapter.h"
#include "stats.h"

namespace bench
{
  // One benchmark case. Topology: `pairs` independent publishers, each with
  // `fanout` subscribers (every subscriber is its own Subscriber object with one
  // connection to "its" publisher).
  struct Scenario
  {
    std::string name;
    std::string group;                 // small | large | fanout | pairs | idle | throughput | slow
    std::string description;
    size_t      message_size   = 1024; // bytes, >= 16 (the first 16 bytes carry sequence and send time)
    double      rate_hz        = 100;  // messages per second and publisher; 0 = as fast as possible
    size_t      pairs          = 1;
    size_t      fanout         = 1;
    bool        idle           = false; // connections only, no traffic
    int         callback_sleep_ms = 0;  // simulated slow consumer
    double      max_duration_s = 0;     // 0 = use the global duration
  };

  struct RunOptions
  {
    double       duration_s = 10.0;     // measured time per scenario
    double       warmup_s   = 1.0;      // traffic before the measurement starts (not measured)
    bool         verbose    = false;    // print where messages went missing
    AdapterOptions adapter;
  };

  struct RunResult
  {
    Scenario scenario;
    int      run_index = 0;
    std::string error;                  // non-empty: the scenario failed

    // traffic
    uint64_t sent_total      = 0;       // messages published inside the measurement window (all publishers)
    uint64_t pacing_overruns = 0;       // schedule slots skipped because the pacing thread was late by > 1 period (harness health)
    uint64_t received_total  = 0;       // messages delivered to all subscribers
    uint64_t expected_total  = 0;       // sent_total * fanout
    double   delivered_ratio = 0;       // received_total / expected_total
    double   received_msgs_per_s = 0;
    double   received_mbytes_per_s = 0;
    double   measured_seconds = 0;

    // latency from "just before publish()" to "handler entry", all subscribers merged
    Summary  latency;
    // duration of the publish() call itself
    Summary  publish_call;

    // process
    double   cpu_percent     = 0;       // 100 = one fully used core, whole process (both sides)
    int      threads         = -1;
    double   rss_mb_end      = 0;
    double   rss_mb_peak     = 0;
    double   rss_mb_growth   = 0;       // end of window minus before traffic

    // lifecycle
    double   connect_ms      = 0;       // creating all subscribers until everything is connected
    double   teardown_ms     = 0;       // destroying subscribers, publishers and contexts
  };

  std::vector<Scenario> allScenarios();

  RunResult runScenario(const Scenario& scenario, const RunOptions& options, int run_index);
}
