// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

// Per-call cost of the library API, measured through the adapter interface of
// the end-to-end harness (so the same benchmarks can run against the rewrite).
//
// The question these answer: what does publish() cost the *calling thread*
// (FR-PUB-6: it must not wait for the network), and how does that scale with
// the payload size and with the number of connected subscribers?
//
//   Api_Publish_NoSubscriber   publish() while nobody is connected (FR-PUB-7)
//   Api_Publish                publish() to N connected subscribers; gap_us = idle
//                              time before every call (0 = back to back,
//                              10000 = a 100 Hz stream)
//
// Only the duration of the publish() call is measured (manual timing). The
// counter "delivered" is the fraction of the published messages that reached the
// handlers (latest-wins drops are expected for back-to-back publishing).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "adapter.h"
#include "micro_common.h"

namespace
{
  using namespace microbench;

  struct Topology
  {
    std::atomic<uint64_t>                            received{0};   // must outlive the subscribers
    std::unique_ptr<bench::IContext>                 publisher_context;
    std::unique_ptr<bench::IContext>                 subscriber_context;
    std::unique_ptr<bench::IPublisher>               publisher;
    std::vector<std::unique_ptr<bench::ISubscriber>> subscribers;     // destroyed first

    explicit Topology(size_t subscriber_count)
    {
      bench::AdapterOptions options;
      options.io_threads = 2;
      publisher_context  = bench::createContext(options);
      subscriber_context = bench::createContext(options);
      publisher          = publisher_context->createPublisher();

      for (size_t i = 0; i < subscriber_count; ++i)
      {
        std::unique_ptr<bench::ISubscriber> subscriber = subscriber_context->createSubscriber();
        subscriber->connect("127.0.0.1", publisher->port());
        subscriber->setHandler([this](const char*, size_t) { received.fetch_add(1, std::memory_order_relaxed); });
        subscribers.push_back(std::move(subscriber));
      }

      const auto deadline = Clock::now() + std::chrono::seconds(15);
      for (;;)
      {
        bool all = publisher->subscriberCount() >= subscriber_count;
        for (size_t i = 0; i < subscribers.size() && all; ++i) all = subscribers[i]->allConnected();
        if (all) break;
        if (Clock::now() > deadline) throw std::runtime_error("timeout while waiting for all connections");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));   // handshakes settle
    }
  };

  void BM_Api_Publish_NoSubscriber(benchmark::State& state)
  {
    const size_t       size = static_cast<size_t>(state.range(0));
    Topology           topology(0);
    std::vector<char>  data(size, 5);

    for (auto _ : state)
    {
      topology.publisher->publish(data.data(), data.size());
    }
  }
  BENCHMARK(BM_Api_Publish_NoSubscriber)->Name("Api_Publish_NoSubscriber")->Arg(64)->Arg(64 << 10)->Arg(1 << 20);

  void BM_Api_Publish(benchmark::State& state)
  {
    const size_t  size              = static_cast<size_t>(state.range(0));
    const size_t  subscriber_count  = static_cast<size_t>(state.range(1));
    const int64_t gap_us            = state.range(2);

    std::unique_ptr<Topology> topology;
    try
    {
      topology.reset(new Topology(subscriber_count));
    }
    catch (const std::exception& e)
    {
      state.SkipWithError(e.what());
      return;
    }

    std::vector<char> data(size, 5);

    // Warm up: first messages allocate buffers, wake threads, etc.
    for (int i = 0; i < 50; ++i)
    {
      topology->publisher->publish(data.data(), data.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint64_t received_before = topology->received.load();

    for (auto _ : state)
    {
      idle(gap_us);
      const auto t0 = Clock::now();
      topology->publisher->publish(data.data(), data.size());
      state.SetIterationTime(seconds(Clock::now() - t0));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(size >= (1u << 20) ? 1000 : 300));
    const double expected = static_cast<double>(state.iterations()) * static_cast<double>(subscriber_count);
    state.counters["delivered"] = static_cast<double>(topology->received.load() - received_before) / expected;
  }

  struct Registrar
  {
    Registrar()
    {
      // {size, subscribers}
      const std::vector<std::pair<int64_t, int64_t>> topologies = {
        {64, 1}, {64, 10}, {64, 100},
        {64 << 10, 1}, {64 << 10, 10},
        {1 << 20, 1},
      };

      for (const int64_t gap_us : {int64_t(0), kStreamGapUs})
      {
        for (const auto& t : topologies)
        {
          benchmark::Benchmark* b = benchmark::RegisterBenchmark("Api_Publish", BM_Api_Publish);
          b->ArgNames({"size", "subs", "gap_us"})->Args({t.first, t.second, gap_us})->UseManualTime()
           ->Iterations(iterationsFor(t.first * t.second, gap_us));
        }
      }
    }
  } registrar;
}
