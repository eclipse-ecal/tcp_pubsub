// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "harness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "platform.h"

namespace bench
{
  namespace
  {
    using Clock = std::chrono::steady_clock;

    constexpr size_t kHeaderBytes = 16;   // [u64 sequence][i64 send time in ns of steady_clock]

    int64_t nowNs()
    {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    // Shared between the pacing thread and the handlers
    struct RunState
    {
      // Messages sent before this instant (warm-up) are ignored by the handlers
      std::atomic<int64_t> measure_start_ns{std::numeric_limits<int64_t>::max()};
    };

    // Written only by the handler of one subscriber; read after all
    // subscribers have been destroyed (which joins the handler threads).
    struct SubStats
    {
      std::vector<int64_t> latency_ns;
      size_t               cap       = 0;
      uint64_t             received  = 0;
      uint64_t             bytes     = 0;
      int                  sleep_ms  = 0;

      // Diagnostics: where do messages go missing? (sequence numbers are per publisher)
      uint64_t             first_seq = 0;
      uint64_t             last_seq  = 0;
      uint64_t             gap_count = 0;
      std::vector<std::pair<uint64_t, uint64_t>> gaps;   // (last seq before the gap, number of missing messages), first few only
    };

    struct PublisherCtl
    {
      std::unique_ptr<IPublisher> publisher;
      std::vector<char>           buffer;
      uint64_t                    sequence = 0;
      uint64_t                    sent     = 0;   // inside the measurement window
    };

    double toMs(const Clock::duration d)
    {
      return std::chrono::duration<double, std::milli>(d).count();
    }

    double toMiB(size_t bytes)
    {
      return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    void runImpl(const Scenario& sc, const RunOptions& opt, RunResult& result)
    {
      if (sc.message_size < kHeaderBytes) throw std::invalid_argument("message_size must be >= 16");

      const double duration_s = (sc.max_duration_s > 0) ? std::min(sc.max_duration_s, opt.duration_s) : opt.duration_s;
      const bool   paced      = (sc.rate_hz > 0) && !sc.idle;

      // Declaration order = reverse destruction order: subscribers first, then
      // publishers, then the contexts. The statistics must outlive the subscribers.
      RunState state;
      std::vector<std::unique_ptr<SubStats>> stats;
      std::unique_ptr<IContext>              publisher_context = createContext(opt.adapter);
      std::unique_ptr<IContext>              subscriber_context = createContext(opt.adapter);
      std::vector<PublisherCtl>              publishers;
      std::vector<std::unique_ptr<ISubscriber>> subscribers;

      // ---- Set up ---------------------------------------------------------
      const size_t sample_cap = paced
          ? static_cast<size_t>(sc.rate_hz * (duration_s + opt.warmup_s) * 1.5) + 1024
          : (1u << 18);

      publishers.resize(sc.pairs);
      for (PublisherCtl& p : publishers)
      {
        p.publisher = publisher_context->createPublisher();
        p.buffer.assign(sc.message_size, 0x5a);
      }

      const auto connect_start = Clock::now();
      for (size_t pair = 0; pair < sc.pairs; ++pair)
      {
        for (size_t i = 0; i < sc.fanout; ++i)
        {
          std::unique_ptr<SubStats> st(new SubStats);
          st->cap = sample_cap;
          st->latency_ns.reserve(sample_cap);
          st->sleep_ms = sc.callback_sleep_ms;
          SubStats* st_ptr = st.get();
          stats.push_back(std::move(st));

          std::unique_ptr<ISubscriber> subscriber = subscriber_context->createSubscriber();
          subscriber->connect("127.0.0.1", publishers[pair].publisher->port());
          subscriber->setHandler([&state, st_ptr](const char* data, size_t size)
                                 {
                                   if (size < kHeaderBytes) return;
                                   const int64_t now = nowNs();
                                   int64_t  t_send;
                                   uint64_t sequence;
                                   std::memcpy(&sequence, data,     sizeof(sequence));
                                   std::memcpy(&t_send,   data + 8, sizeof(t_send));
                                   if (t_send < state.measure_start_ns.load(std::memory_order_relaxed)) return;

                                   if (st_ptr->first_seq == 0) st_ptr->first_seq = sequence;
                                   else if (sequence > st_ptr->last_seq + 1)
                                   {
                                     ++st_ptr->gap_count;
                                     if (st_ptr->gaps.size() < 32) st_ptr->gaps.emplace_back(st_ptr->last_seq, sequence - st_ptr->last_seq - 1);
                                   }
                                   st_ptr->last_seq = sequence;

                                   if (st_ptr->latency_ns.size() < st_ptr->cap) st_ptr->latency_ns.push_back(now - t_send);
                                   ++st_ptr->received;
                                   st_ptr->bytes += size;
                                   if (st_ptr->sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st_ptr->sleep_ms));
                                 });
          subscribers.push_back(std::move(subscriber));
        }
      }

      // Wait until everything is connected
      {
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        for (;;)
        {
          bool all = true;
          for (size_t pair = 0; pair < sc.pairs && all; ++pair)
          {
            if (publishers[pair].publisher->subscriberCount() < sc.fanout) all = false;
          }
          for (size_t i = 0; i < subscribers.size() && all; ++i)
          {
            if (!subscribers[i]->allConnected()) all = false;
          }
          if (all) break;
          if (Clock::now() > deadline) throw std::runtime_error("timeout while waiting for all connections");
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      result.connect_ms = toMs(Clock::now() - connect_start);

      // Let the handshakes settle
      std::this_thread::sleep_for(std::chrono::milliseconds(300));

      // ---- Run ------------------------------------------------------------
      std::vector<int64_t> publish_ns;
      publish_ns.reserve(paced
          ? static_cast<size_t>(sc.rate_hz * static_cast<double>(sc.pairs) * (duration_s + opt.warmup_s) * 1.5) + 1024
          : (1u << 18));

      const Clock::duration period = paced
          ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / sc.rate_hz))
          : Clock::duration::zero();

      const ProcessSnapshot before_traffic = sampleProcess();

      const auto t_begin  = Clock::now() + std::chrono::milliseconds(50);
      const auto warm_end = t_begin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(opt.warmup_s));
      const auto t_end    = warm_end + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(duration_s));

      // Publishers are phase-shifted against each other, as independent
      // applications would be
      std::vector<Clock::time_point> next(publishers.size(), t_begin);
      if (paced)
      {
        for (size_t i = 0; i < next.size(); ++i)
        {
          next[i] = t_begin + (period * static_cast<Clock::rep>(i)) / static_cast<Clock::rep>(next.size());
        }
      }

      bool            measuring = false;
      uint64_t        pacing_overruns = 0;
      ProcessSnapshot window_start;
      Clock::time_point window_start_time;

      for (;;)
      {
        size_t idx    = 0;
        auto   target = Clock::now();
        if (sc.idle)
        {
          target = measuring ? t_end : warm_end;
        }
        else if (paced)
        {
          idx    = static_cast<size_t>(std::min_element(next.begin(), next.end()) - next.begin());
          target = next[idx];
        }

        const auto wake = std::min(target, t_end);   // never sleep past the end
        if (Clock::now() < wake) std::this_thread::sleep_until(wake);

        const auto now = Clock::now();
        if (!measuring && now >= warm_end)
        {
          measuring         = true;
          window_start      = sampleProcess(false);   // must be cheap: the pacing thread must not stall
          window_start_time = now;
          state.measure_start_ns.store(nowNs(), std::memory_order_relaxed);
        }
        if (now >= t_end) break;
        if (sc.idle) continue;
        if (paced && now < target) continue;

        PublisherCtl& p = publishers[idx];
        ++p.sequence;
        const int64_t t_send = nowNs();
        std::memcpy(p.buffer.data(),     &p.sequence, sizeof(p.sequence));
        std::memcpy(p.buffer.data() + 8, &t_send,     sizeof(t_send));
        p.publisher->publish(p.buffer.data(), p.buffer.size());
        const int64_t t_after = nowNs();

        if (measuring)
        {
          ++p.sent;
          if (publish_ns.size() < publish_ns.capacity()) publish_ns.push_back(t_after - t_send);
        }
        if (paced)
        {
          next[idx] += period;
          // If the pacing thread was stalled for more than a period, skip the missed
          // slots instead of publishing a burst: a burst would be dropped by the
          // library's latest-wins queues and be mistaken for message loss.
          const auto after = Clock::now();
          while (next[idx] + period <= after)
          {
            next[idx] += period;
            if (measuring) ++pacing_overruns;
          }
        }
      }

      const ProcessSnapshot window_end      = sampleProcess();
      const auto            window_end_time = Clock::now();

      // Let messages that are still in flight arrive
      std::this_thread::sleep_for(std::chrono::milliseconds(sc.message_size >= (1u << 20) ? 1000 : 300));

      // ---- Tear down (timed) ----------------------------------------------
      const auto teardown_start = Clock::now();
      subscribers.clear();
      uint64_t sent_total = 0;
      const uint64_t last_published_by_first_publisher = publishers.empty() ? 0 : publishers[0].sequence;
      for (const PublisherCtl& p : publishers) sent_total += p.sent;
      publishers.clear();
      publisher_context.reset();
      subscriber_context.reset();
      result.teardown_ms = toMs(Clock::now() - teardown_start);

      // ---- Evaluate -------------------------------------------------------
      if (opt.verbose)
      {
        for (size_t i = 0; i < stats.size() && i < 3; ++i)
        {
          const SubStats& st = *stats[i];
          std::cerr << "  [verbose] subscriber " << i << ": first seq " << st.first_seq << ", last seq " << st.last_seq
                    << " (publisher 0 sent up to " << last_published_by_first_publisher << ")"
                    << ", received " << st.received << ", gaps " << st.gap_count << " (first missing seq):";
          for (const auto& g : st.gaps) std::cerr << " " << g.first + 1 << "(x" << g.second << ")";
          std::cerr << "\n";
        }
      }
      std::vector<int64_t> latency_ns;
      uint64_t received_total = 0;
      uint64_t received_bytes = 0;
      for (const auto& st : stats)
      {
        latency_ns.insert(latency_ns.end(), st->latency_ns.begin(), st->latency_ns.end());
        received_total += st->received;
        received_bytes += st->bytes;
      }

      result.measured_seconds = std::chrono::duration<double>(window_end_time - window_start_time).count();
      result.sent_total       = sent_total;
      result.pacing_overruns  = pacing_overruns;
      result.received_total   = received_total;
      result.expected_total   = sent_total * sc.fanout;
      result.delivered_ratio  = result.expected_total ? static_cast<double>(received_total) / static_cast<double>(result.expected_total) : 0.0;
      result.received_msgs_per_s    = static_cast<double>(received_total) / result.measured_seconds;
      result.received_mbytes_per_s  = static_cast<double>(received_bytes) / (1024.0 * 1024.0) / result.measured_seconds;
      result.latency          = summarize(latency_ns);
      result.publish_call     = summarize(publish_ns);

      result.cpu_percent      = (window_end.cpu_seconds - window_start.cpu_seconds) / result.measured_seconds * 100.0;
      result.threads          = window_end.threads;
      result.rss_mb_end       = toMiB(window_end.rss_bytes);
      result.rss_mb_peak      = toMiB(window_end.peak_rss_bytes);
      result.rss_mb_growth    = toMiB(window_end.rss_bytes) - toMiB(before_traffic.rss_bytes);
    }
  }

  std::vector<Scenario> allScenarios()
  {
    std::vector<Scenario> list;

    auto add = [&list](const std::string& name, const std::string& group, const std::string& description,
                       size_t size, double rate, size_t pairs, size_t fanout)
    {
      Scenario sc;
      sc.name = name; sc.group = group; sc.description = description;
      sc.message_size = size; sc.rate_hz = rate; sc.pairs = pairs; sc.fanout = fanout;
      list.push_back(sc);
      return list.size() - 1;
    };

    // W-1: small messages at 100 Hz
    add("small-64B",   "small", "1 publisher -> 1 subscriber, 64 B at 100 Hz",    64,        100, 1, 1);
    add("small-1KiB",  "small", "1 publisher -> 1 subscriber, 1 KiB at 100 Hz",   1024,      100, 1, 1);
    add("small-64KiB", "small", "1 publisher -> 1 subscriber, 64 KiB at 100 Hz",  64 * 1024, 100, 1, 1);

    // W-2: large messages at 15 Hz
    add("large-1MiB",  "large", "1 publisher -> 1 subscriber, 1 MiB at 15 Hz",   1u << 20,  15, 1, 1);
    add("large-4MiB",  "large", "1 publisher -> 1 subscriber, 4 MiB at 15 Hz",   4u << 20,  15, 1, 1);
    add("large-16MiB", "large", "1 publisher -> 1 subscriber, 16 MiB at 15 Hz",  16u << 20, 15, 1, 1);

    // W-3: scale
    add("fanout-10",   "fanout", "1 publisher -> 10 subscribers, 64 KiB at 100 Hz",  64 * 1024, 100, 1, 10);
    add("fanout-100",  "fanout", "1 publisher -> 100 subscribers, 16 KiB at 100 Hz", 16 * 1024, 100, 1, 100);
    add("pairs-100",   "pairs",  "100 publishers, each with 1 subscriber (own Subscriber object), 1 KiB at 100 Hz each", 1024, 100, 100, 1);

    // PERF-6: idle cost with many connections
    {
      const size_t i = add("idle-100", "idle", "100 publisher/subscriber pairs connected, no traffic", 1024, 0, 100, 1);
      list[i].idle           = true;
      list[i].max_duration_s = 5;
    }

    // Maximum throughput of a subscriber that keeps up (publisher sends as fast as it can)
    add("throughput-64KiB", "throughput", "1 -> 1, 64 KiB, unpaced",  64 * 1024, 0, 1, 1);
    add("throughput-1MiB",  "throughput", "1 -> 1, 1 MiB, unpaced",   1u << 20,  0, 1, 1);

    // Latest-wins and bounded memory with a consumer that is slower than the publisher
    {
      const size_t i = add("slow-consumer", "slow", "1 -> 1, 1 MiB at 100 Hz, handler needs 50 ms per message", 1u << 20, 100, 1, 1);
      list[i].callback_sleep_ms = 50;
    }

    return list;
  }

  RunResult runScenario(const Scenario& scenario, const RunOptions& options, int run_index)
  {
    RunResult result;
    result.scenario  = scenario;
    result.run_index = run_index;
    try
    {
      runImpl(scenario, options, result);
    }
    catch (const std::exception& e)
    {
      result.error = e.what();
    }
    return result;
  }
}
