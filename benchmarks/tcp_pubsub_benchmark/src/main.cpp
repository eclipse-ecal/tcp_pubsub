// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "adapter.h"
#include "harness.h"
#include "platform.h"

namespace
{
  void printUsage()
  {
    std::cout <<
      "tcp_pubsub_benchmark - end-to-end latency, throughput and resource benchmark\n"
      "\n"
      "  --list                 list the scenarios and exit\n"
      "  --scenario <x>         scenario name, group (small|large|fanout|pairs|idle|throughput|slow) or 'all' (default: all)\n"
      "                         may be given multiple times\n"
      "  --duration <s>         measured seconds per scenario (default 10)\n"
      "  --warmup <s>           unmeasured traffic before measuring (default 1)\n"
      "  --repeat <n>           run every scenario n times (default 1)\n"
      "  --io-threads <n>       worker threads of each side's execution context (default 2)\n"
      "  --delivery <mode>      pooled (library default, handler on its own thread) | inline (handler on the I/O side)\n"
      "  --label <text>         free text stored in the report (e.g. 'baseline v1, commit abc123')\n"
      "  --json <file>          write all results as JSON\n"
      "  --verbose              print where messages went missing (sequence gaps)\n"
      "\n"
      "Build in Release mode and keep the machine quiet; see docs/rewrite/05-benchmark.md.\n";
  }

  std::string jsonEscape(const std::string& s)
  {
    std::string out;
    for (const char c : s)
    {
      switch (c)
      {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
      }
    }
    return out;
  }

  std::string num(double v)
  {
    std::ostringstream ss;
    ss << std::setprecision(6) << v;
    return ss.str();
  }

  std::string summaryJson(const bench::Summary& s)
  {
    std::ostringstream ss;
    ss << "{\"count\":" << s.count
       << ",\"mean_us\":" << num(s.mean_us) << ",\"min_us\":" << num(s.min_us)
       << ",\"p50_us\":"  << num(s.p50_us)  << ",\"p90_us\":" << num(s.p90_us)
       << ",\"p99_us\":"  << num(s.p99_us)  << ",\"p999_us\":" << num(s.p999_us)
       << ",\"max_us\":"  << num(s.max_us)  << "}";
    return ss.str();
  }

  std::string isoNow()
  {
    const std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buffer;
  }

  std::string compilerDescription()
  {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
  }

  bool isReleaseBuild()
  {
#ifdef NDEBUG
    return true;
#else
    return false;
#endif
  }

  void printResult(const bench::RunResult& r)
  {
    const bench::Scenario& sc = r.scenario;
    std::cout << "== " << sc.name << "  (" << sc.description << ")  run " << r.run_index + 1 << "\n";
    if (!r.error.empty())
    {
      std::cout << "   FAILED: " << r.error << "\n\n";
      return;
    }

    std::cout << std::fixed;
    if (sc.idle)
    {
      std::cout << std::setprecision(1)
                << "   idle (no traffic)   cpu " << r.cpu_percent << " %   threads " << r.threads
                << "   rss " << r.rss_mb_end << " MiB   connect " << r.connect_ms << " ms   teardown " << r.teardown_ms << " ms\n\n";
      return;
    }
    std::cout << "   delivered " << r.received_total << "/" << r.expected_total
              << " (" << std::setprecision(2) << r.delivered_ratio * 100.0 << " %)   "
              << std::setprecision(1) << r.received_msgs_per_s << " msg/s   "
              << r.received_mbytes_per_s << " MiB/s received\n";
    if (r.pacing_overruns > 0)
    {
      std::cout << "   NOTE: " << r.pacing_overruns << " publish slots were skipped because the publishing thread was stalled; "
                << "the machine is not quiet enough for a clean measurement.\n";
    }
    if (r.latency.count > 0)
    {
      std::cout << std::setprecision(1)
                << "   latency [us]   mean " << r.latency.mean_us << "  p50 " << r.latency.p50_us << "  p90 " << r.latency.p90_us
                << "  p99 " << r.latency.p99_us << "  p99.9 " << r.latency.p999_us << "  max " << r.latency.max_us << "\n";
    }
    if (r.publish_call.count > 0)
    {
      std::cout << std::setprecision(1)
                << "   publish() [us] mean " << r.publish_call.mean_us << "  p50 " << r.publish_call.p50_us
                << "  p99 " << r.publish_call.p99_us << "  max " << r.publish_call.max_us << "\n";
    }
    std::cout << std::setprecision(1)
              << "   cpu " << r.cpu_percent << " %   threads " << r.threads
              << "   rss " << r.rss_mb_end << " MiB (process peak so far " << r.rss_mb_peak << ", growth " << std::showpos << r.rss_mb_growth << std::noshowpos << ")"
              << "   connect " << r.connect_ms << " ms   teardown " << r.teardown_ms << " ms\n\n";
  }

  void printTable(const std::vector<bench::RunResult>& results)
  {
    std::cout << std::left << std::setw(18) << "scenario" << std::right
              << std::setw(4)  << "run"
              << std::setw(10) << "p50 us" << std::setw(10) << "p99 us" << std::setw(10) << "p99.9 us" << std::setw(11) << "max us"
              << std::setw(11) << "delivered" << std::setw(8) << "cpu %" << std::setw(9) << "threads" << std::setw(10) << "rss MiB"
              << std::setw(13) << "teardown ms" << "\n";
    for (const bench::RunResult& r : results)
    {
      std::cout << std::left << std::setw(18) << r.scenario.name << std::right << std::setw(4) << r.run_index + 1;
      if (!r.error.empty())
      {
        std::cout << "   FAILED: " << r.error << "\n";
        continue;
      }
      std::cout << std::fixed << std::setprecision(1);
      if (r.scenario.idle)
      {
        std::cout << std::setw(10) << "-" << std::setw(10) << "-" << std::setw(10) << "-" << std::setw(11) << "-" << std::setw(11) << "-"
                  << std::setw(8) << r.cpu_percent << std::setw(9) << r.threads
                  << std::setw(10) << r.rss_mb_end << std::setw(13) << r.teardown_ms << "\n";
        continue;
      }
      std::cout
                << std::setw(10) << r.latency.p50_us << std::setw(10) << r.latency.p99_us
                << std::setw(10) << r.latency.p999_us << std::setw(11) << r.latency.max_us
                << std::setw(10) << r.delivered_ratio * 100.0 << "%"
                << std::setw(8) << r.cpu_percent << std::setw(9) << r.threads
                << std::setw(10) << r.rss_mb_end << std::setw(13) << r.teardown_ms << "\n";
    }
  }

  void writeJson(const std::string& path, const std::string& label, const bench::RunOptions& options, const std::vector<bench::RunResult>& results)
  {
    std::ofstream out(path);
    if (!out) { std::cerr << "Cannot write " << path << "\n"; return; }

    out << "{\n  \"meta\": {\n"
        << "    \"label\": \""       << jsonEscape(label) << "\",\n"
        << "    \"adapter\": \""     << jsonEscape(bench::adapterName()) << "\",\n"
        << "    \"date_utc\": \""    << isoNow() << "\",\n"
        << "    \"os\": \""          << jsonEscape(bench::osDescription()) << "\",\n"
        << "    \"cpu\": \""         << jsonEscape(bench::cpuDescription()) << "\",\n"
        << "    \"hardware_threads\": " << bench::hardwareThreads() << ",\n"
        << "    \"compiler\": \""    << jsonEscape(compilerDescription()) << "\",\n"
        << "    \"release_build\": " << (isReleaseBuild() ? "true" : "false") << ",\n"
        << "    \"duration_s\": "    << num(options.duration_s) << ",\n"
        << "    \"warmup_s\": "      << num(options.warmup_s) << ",\n"
        << "    \"io_threads\": "    << options.adapter.io_threads << ",\n"
        << "    \"delivery\": \""    << (options.adapter.inline_delivery ? "inline" : "pooled") << "\"\n"
        << "  },\n  \"results\": [\n";

    for (size_t i = 0; i < results.size(); ++i)
    {
      const bench::RunResult& r = results[i];
      const bench::Scenario&  sc = r.scenario;
      out << "    {\"scenario\":\"" << jsonEscape(sc.name) << "\",\"group\":\"" << sc.group << "\",\"run\":" << r.run_index + 1
          << ",\"message_size\":" << sc.message_size << ",\"rate_hz\":" << num(sc.rate_hz)
          << ",\"pairs\":" << sc.pairs << ",\"fanout\":" << sc.fanout;
      if (!r.error.empty())
      {
        out << ",\"error\":\"" << jsonEscape(r.error) << "\"}";
      }
      else
      {
        out << ",\"sent\":" << r.sent_total << ",\"pacing_overruns\":" << r.pacing_overruns << ",\"received\":" << r.received_total << ",\"expected\":" << r.expected_total
            << ",\"delivered_ratio\":" << num(r.delivered_ratio)
            << ",\"received_msgs_per_s\":" << num(r.received_msgs_per_s)
            << ",\"received_mib_per_s\":" << num(r.received_mbytes_per_s)
            << ",\"measured_s\":" << num(r.measured_seconds)
            << ",\"latency\":" << summaryJson(r.latency)
            << ",\"publish_call\":" << summaryJson(r.publish_call)
            << ",\"cpu_percent\":" << num(r.cpu_percent) << ",\"threads\":" << r.threads
            << ",\"rss_mib_end\":" << num(r.rss_mb_end) << ",\"rss_mib_peak\":" << num(r.rss_mb_peak)
            << ",\"rss_mib_growth\":" << num(r.rss_mb_growth)
            << ",\"connect_ms\":" << num(r.connect_ms) << ",\"teardown_ms\":" << num(r.teardown_ms) << "}";
      }
      out << (i + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
  }
}

int main(int argc, char** argv)
{
  bench::RunOptions        options;
  std::vector<std::string> selectors;
  std::string              json_path;
  std::string              label;
  int                      repeat = 1;
  bool                     list_only = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string
    {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(2); }
      return argv[++i];
    };

    if      (arg == "--list")        list_only = true;
    else if (arg == "--scenario")    selectors.push_back(value());
    else if (arg == "--duration")    options.duration_s = std::atof(value().c_str());
    else if (arg == "--warmup")      options.warmup_s   = std::atof(value().c_str());
    else if (arg == "--repeat")      repeat = std::atoi(value().c_str());
    else if (arg == "--io-threads")  options.adapter.io_threads = static_cast<size_t>(std::atoi(value().c_str()));
    else if (arg == "--delivery")    options.adapter.inline_delivery = (value() == "inline");
    else if (arg == "--label")       label = value();
    else if (arg == "--json")        json_path = value();
    else if (arg == "--verbose")     options.verbose = true;
    else if (arg == "--help" || arg == "-h") { printUsage(); return 0; }
    else { std::cerr << "Unknown argument: " << arg << "\n"; printUsage(); return 2; }
  }

  const std::vector<bench::Scenario> all = bench::allScenarios();

  if (list_only)
  {
    for (const auto& sc : all) std::cout << std::left << std::setw(18) << sc.name << std::setw(12) << sc.group << sc.description << "\n";
    return 0;
  }

  if (selectors.empty()) selectors.push_back("all");

  std::vector<bench::Scenario> selected;
  for (const auto& sc : all)
  {
    for (const auto& sel : selectors)
    {
      if (sel == "all" || sel == sc.name || sel == sc.group) { selected.push_back(sc); break; }
    }
  }
  if (selected.empty()) { std::cerr << "No scenario matches.\n"; return 2; }

  if (!isReleaseBuild())
  {
    std::cerr << "WARNING: this is not a Release build (NDEBUG not defined). Results are NOT representative.\n";
  }

  bench::raiseTimerResolution();

  std::cout << "Adapter:   " << bench::adapterName() << "\n"
            << "OS / CPU:  " << bench::osDescription() << " / " << bench::cpuDescription() << " (" << bench::hardwareThreads() << " hardware threads)\n"
            << "Settings:  duration " << options.duration_s << " s, warmup " << options.warmup_s << " s, io-threads " << options.adapter.io_threads
            << ", delivery " << (options.adapter.inline_delivery ? "inline" : "pooled") << ", repeat " << repeat << "\n\n";

  std::vector<bench::RunResult> results;
  for (const auto& sc : selected)
  {
    for (int run = 0; run < repeat; ++run)
    {
      std::cout << "running " << sc.name << " ..." << std::endl;
      results.push_back(bench::runScenario(sc, options, run));
      printResult(results.back());
    }
  }

  std::cout << "Summary\n";
  printTable(results);

  if (!json_path.empty())
  {
    writeJson(json_path, label, options, results);
    std::cout << "\nWrote " << json_path << "\n";
  }

  return 0;
}
