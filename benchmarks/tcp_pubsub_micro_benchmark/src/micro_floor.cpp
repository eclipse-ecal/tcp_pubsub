// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

// Reference "floor" benchmarks: they do not use tcp_pubsub at all. They measure
// what the platform costs for the primitives any implementation is built from,
// so that the numbers of the library benchmarks (micro_api.cpp) and of the
// end-to-end harness can be put into context:
//
//   - copying a payload (PERF-3), with and without a fresh allocation (PERF-4)
//   - handing work to another thread: condition variable, spinning, asio::post
//     (the cost of an additional hop, e.g. "pooled" vs "inline" delivery)
//   - raw TCP loopback round trips with TCP_NODELAY (the latency floor)
//   - the cost of one synchronous write and of initiating one asynchronous write
//     on the calling thread (what publish() pays per subscriber today)
//
// "gap_us" is the idle time before every measured operation. 0 = back to back
// (hot caches, threads still running); 10000 = a 100 Hz stream (threads asleep,
// CPU possibly in a power state): the situation of the real workload W-1.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <benchmark/benchmark.h>

#include "micro_common.h"

namespace
{
  using namespace microbench;

  // ---------------------------------------------------------------------------
  // Copies
  // ---------------------------------------------------------------------------

  void BM_Floor_Memcpy(benchmark::State& state)
  {
    const size_t       size = static_cast<size_t>(state.range(0));
    std::vector<char>  src(size, 1);
    std::vector<char>  dst(size, 0);

    for (auto _ : state)
    {
      std::memcpy(dst.data(), src.data(), size);
      benchmark::DoNotOptimize(dst.data());
      benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
  }
  BENCHMARK(BM_Floor_Memcpy)->Name("Floor_Memcpy")->Arg(64)->Arg(64 << 10)->Arg(1 << 20)->Arg(16 << 20);

  // The buffer handling of the current implementation for a message that cannot
  // use the pool: allocate + zero-fill (vector::resize), copy, free.
  void BM_Floor_AllocZeroCopyFree(benchmark::State& state)
  {
    const size_t       size = static_cast<size_t>(state.range(0));
    std::vector<char>  src(size, 1);

    for (auto _ : state)
    {
      std::vector<char> buffer(size);
      std::memcpy(buffer.data(), src.data(), size);
      benchmark::DoNotOptimize(buffer.data());
      benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
  }
  BENCHMARK(BM_Floor_AllocZeroCopyFree)->Name("Floor_AllocZeroCopyFree")->Arg(64)->Arg(64 << 10)->Arg(1 << 20)->Arg(16 << 20);

  // ---------------------------------------------------------------------------
  // Thread hand-off
  // ---------------------------------------------------------------------------

  // Two threads, mutex + condition variable. Reports the one-way time (RTT / 2).
  void BM_Floor_HandoffCondvar(benchmark::State& state)
  {
    const int64_t gap_us = state.range(1);

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    request = false;
    bool                    reply   = false;
    bool                    stop    = false;

    std::thread worker([&]
                       {
                         std::unique_lock<std::mutex> lock(mutex);
                         for (;;)
                         {
                           cv.wait(lock, [&] { return request || stop; });
                           if (stop) return;
                           request = false;
                           reply   = true;
                           cv.notify_all();
                         }
                       });

    for (auto _ : state)
    {
      idle(gap_us);
      const auto t0 = Clock::now();
      {
        const std::lock_guard<std::mutex> lock(mutex);
        request = true;
      }
      cv.notify_all();
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return reply; });
        reply = false;
      }
      state.SetIterationTime(seconds(Clock::now() - t0) / 2.0);
    }

    {
      const std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_all();
    worker.join();
    state.SetLabel("one-way = round trip / 2");
  }

  // Two threads spinning on atomics: the best case of any hand-off (no sleeping,
  // no kernel). Needs two free cores.
  void BM_Floor_HandoffSpin(benchmark::State& state)
  {
    std::atomic<int>  flag{0};   // 0 idle, 1 request, 2 reply, 3 stop
    std::thread worker([&]
                       {
                         for (;;)
                         {
                           int value;
                           while ((value = flag.load(std::memory_order_acquire)) == 0 || value == 2) cpuRelax();
                           if (value == 3) return;
                           flag.store(2, std::memory_order_release);
                         }
                       });

    for (auto _ : state)
    {
      flag.store(0, std::memory_order_release);
      const auto t0 = Clock::now();
      flag.store(1, std::memory_order_release);
      while (flag.load(std::memory_order_acquire) != 2) cpuRelax();
      state.SetIterationTime(seconds(Clock::now() - t0) / 2.0);
    }

    flag.store(3, std::memory_order_release);
    worker.join();
    state.SetLabel("one-way = round trip / 2");
  }
  BENCHMARK(BM_Floor_HandoffSpin)->Name("Floor_Handoff_spin")->UseManualTime()->Iterations(200000);

  struct Loop
  {
    asio::io_context                                           context;
    asio::executor_work_guard<asio::io_context::executor_type> guard{context.get_executor()};
    std::thread                                                thread{[this] { context.run(); }};

    ~Loop()
    {
      guard.reset();
      thread.join();
    }
  };

  // Time from "just before asio::post" to "handler entered" on an idle I/O thread:
  // the cost of one hop to a library thread (what an extra thread hop adds).
  void BM_Floor_PostToHandler(benchmark::State& state)
  {
    const int64_t gap_us = state.range(1);

    Loop                 loop;
    std::atomic<int64_t> t_handler{0};

    for (auto _ : state)
    {
      idle(gap_us);
      t_handler.store(0, std::memory_order_relaxed);
      const int64_t t0 = nowNs();
      asio::post(loop.context, [&] { t_handler.store(nowNs(), std::memory_order_release); });
      int64_t t1 = 0;
      spinWait([&] { return (t1 = t_handler.load(std::memory_order_acquire)) != 0; });
      state.SetIterationTime(static_cast<double>(t1 - t0) * 1e-9);
    }
  }

  // Duration of the asio::post call itself on the calling thread: the cost of the
  // publish() hot path if publishing is "post a message to the connection's loop".
  void BM_Floor_PostCall(benchmark::State& state)
  {
    const int64_t gap_us = state.range(1);

    Loop             loop;
    std::atomic<int> done{0};

    for (auto _ : state)
    {
      idle(gap_us);
      done.store(0, std::memory_order_relaxed);
      const auto t0 = Clock::now();
      asio::post(loop.context, [&] { done.store(1, std::memory_order_release); });
      state.SetIterationTime(seconds(Clock::now() - t0));
      spinWait([&] { return done.load(std::memory_order_acquire) == 1; });
    }
  }

  // ---------------------------------------------------------------------------
  // Raw TCP over loopback
  // ---------------------------------------------------------------------------

  struct TcpPair
  {
    asio::io_context      client_context;
    asio::io_context      server_context;
    asio::ip::tcp::socket client{client_context};
    asio::ip::tcp::socket server{server_context};

    TcpPair()
    {
      asio::ip::tcp::acceptor acceptor(server_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
      client.connect(acceptor.local_endpoint());
      acceptor.accept(server);
      client.set_option(asio::ip::tcp::no_delay(true));
      server.set_option(asio::ip::tcp::no_delay(true));
    }
  };

  // Blocking echo: the latency floor of TCP on this machine, with TCP_NODELAY.
  // One-way time = round trip / 2.
  void BM_Floor_TcpPingPong(benchmark::State& state)
  {
    const size_t  size   = static_cast<size_t>(state.range(0));
    const int64_t gap_us = state.range(1);

    TcpPair           pair;
    std::vector<char> message(size, 7);
    std::vector<char> reply(size);

    std::thread echo([&]
                     {
                       std::vector<char> buffer(size);
                       asio::error_code  ec;
                       for (;;)
                       {
                         asio::read(pair.server, asio::buffer(buffer), ec);
                         if (ec) return;
                         asio::write(pair.server, asio::buffer(buffer), ec);
                         if (ec) return;
                       }
                     });

    for (auto _ : state)
    {
      idle(gap_us);
      const auto t0 = Clock::now();
      asio::write(pair.client, asio::buffer(message));
      asio::read(pair.client, asio::buffer(reply));
      state.SetIterationTime(seconds(Clock::now() - t0) / 2.0);
    }

    asio::error_code ec;
    pair.client.close(ec);
    echo.join();
    state.SetLabel("one-way = round trip / 2");
  }

  // A reader that drains the connection, so that the write benchmarks never stall
  // on a full socket buffer.
  struct Drain
  {
    explicit Drain(asio::ip::tcp::socket& socket)
      : thread_([&socket]
                {
                  std::vector<char> buffer(1 << 20);
                  asio::error_code  ec;
                  while (!ec) socket.read_some(asio::buffer(buffer), ec);
                })
    {}
    std::thread thread_;
  };

  // Cost of one blocking send of `size` bytes on the calling thread.
  void BM_Floor_TcpSyncWrite(benchmark::State& state)
  {
    const size_t  size   = static_cast<size_t>(state.range(0));
    const int64_t gap_us = state.range(1);

    TcpPair           pair;
    std::vector<char> data(size, 3);
    Drain             drain(pair.server);

    for (auto _ : state)
    {
      idle(gap_us);
      const auto t0 = Clock::now();
      asio::write(pair.client, asio::buffer(data));
      state.SetIterationTime(seconds(Clock::now() - t0));
    }

    asio::error_code ec;
    pair.client.close(ec);
    drain.thread_.join();
  }

  // Cost of *initiating* one asynchronous write on the calling thread (the part
  // of publish() that runs on the user's thread in the current implementation).
  // Completion is awaited outside of the measured time.
  void BM_Floor_TcpAsyncWriteInitiate(benchmark::State& state)
  {
    const size_t  size   = static_cast<size_t>(state.range(0));
    const int64_t gap_us = state.range(1);

    TcpPair           pair;
    std::vector<char> data(size, 3);
    Drain             drain(pair.server);

    asio::executor_work_guard<asio::io_context::executor_type> guard(pair.client_context.get_executor());
    std::thread io([&] { pair.client_context.run(); });

    std::atomic<int> done{0};
    for (auto _ : state)
    {
      idle(gap_us);
      done.store(0, std::memory_order_relaxed);
      const auto t0 = Clock::now();
      asio::async_write(pair.client, asio::buffer(data), [&](const asio::error_code&, size_t) { done.store(1, std::memory_order_release); });
      state.SetIterationTime(seconds(Clock::now() - t0));
      spinWait([&] { return done.load(std::memory_order_acquire) == 1; });
    }

    guard.reset();
    asio::error_code ec;
    pair.client.close(ec);
    io.join();
    drain.thread_.join();
  }

  // ---------------------------------------------------------------------------
  // Registration: every benchmark is run back to back (gap 0) and as a 100 Hz
  // stream (gap 10 ms), with fixed iteration counts (manual timing).
  // ---------------------------------------------------------------------------

  struct Registrar
  {
    Registrar()
    {
      registerGapped("Floor_Handoff_condvar",        BM_Floor_HandoffCondvar,        {0});
      registerGapped("Floor_PostToHandler",          BM_Floor_PostToHandler,         {0});
      registerGapped("Floor_PostCall",               BM_Floor_PostCall,              {0});
      registerGapped("Floor_TcpPingPong",            BM_Floor_TcpPingPong,           {64, 64 << 10});
      registerGapped("Floor_TcpSyncWrite",           BM_Floor_TcpSyncWrite,          {64, 64 << 10, 1 << 20});
      registerGapped("Floor_TcpAsyncWriteInitiate",  BM_Floor_TcpAsyncWriteInitiate, {64, 64 << 10, 1 << 20});
    }
  } registrar;
}
