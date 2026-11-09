// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bench
{
  struct Summary
  {
    size_t count   = 0;
    double mean_us = 0;
    double min_us  = 0;
    double p50_us  = 0;
    double p90_us  = 0;
    double p99_us  = 0;
    double p999_us = 0;
    double max_us  = 0;
  };

  // Nearest-rank percentile on sorted data
  inline double percentileNs(const std::vector<int64_t>& sorted, double p)
  {
    if (sorted.empty()) return 0.0;
    size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    if (rank < 1) rank = 1;
    if (rank > sorted.size()) rank = sorted.size();
    return static_cast<double>(sorted[rank - 1]);
  }

  // Sorts the samples (nanoseconds) and returns the summary in microseconds.
  inline Summary summarize(std::vector<int64_t>& samples_ns)
  {
    Summary summary;
    if (samples_ns.empty()) return summary;

    std::sort(samples_ns.begin(), samples_ns.end());

    double sum = 0;
    for (const int64_t value : samples_ns) sum += static_cast<double>(value);

    summary.count   = samples_ns.size();
    summary.mean_us = sum / static_cast<double>(samples_ns.size()) / 1000.0;
    summary.min_us  = static_cast<double>(samples_ns.front()) / 1000.0;
    summary.p50_us  = percentileNs(samples_ns, 0.50)  / 1000.0;
    summary.p90_us  = percentileNs(samples_ns, 0.90)  / 1000.0;
    summary.p99_us  = percentileNs(samples_ns, 0.99)  / 1000.0;
    summary.p999_us = percentileNs(samples_ns, 0.999) / 1000.0;
    summary.max_us  = static_cast<double>(samples_ns.back()) / 1000.0;
    return summary;
  }
}
