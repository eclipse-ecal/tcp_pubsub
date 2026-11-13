// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include "executor_impl.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <asio.hpp>

#include "tcp_pubsub/tcp_pubsub_logger.h"
#include "tcp_pubsub_logger_abstraction.h"

namespace tcp_pubsub
{
  Executor_Impl::Executor_Impl(const logger::logger_t& log_function)
    : log_(log_function)
    , io_context_(std::make_shared<asio::io_context>())
    , shut_down_(false)
    , dummy_work_(std::make_shared<work_guard_t>(io_context_->get_executor()))
  {
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "Executor: Creating Executor.");
#endif
  }

  Executor_Impl::~Executor_Impl()
  {
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
    std::stringstream ss;
    ss << std::this_thread::get_id();
    const std::string thread_id = ss.str();
    log_(logger::LogLevel::DebugVerbose, "Executor: Deleting from thread " + thread_id + "...");
#endif

    // Usually the executor has been shut down already. Make sure that no
    // thread is left running, in any case.
    shutdown();

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "Executor: Deleted.");
#endif
  }

  void Executor_Impl::start(size_t thread_count)
  {
    if (thread_count == 0)
    {
      log_(logger::LogLevel::Error, "Executor: At least one worker thread is required.");
      throw std::invalid_argument("Executor: At least one worker thread is required.");
    }

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "Executor: Starting Executor with " + std::to_string(thread_count) + " threads.");
#endif
    thread_pool_.reserve(thread_count);

    for (size_t i = 0; i < thread_count; i++)
    {
      // The worker must not keep the Executor_Impl alive, so it only captures
      // what it needs itself.
      thread_pool_.emplace_back([io_context = io_context_, log = log_]()
                                {
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
                                  std::stringstream ss;
                                  ss << std::this_thread::get_id();
                                  const std::string thread_id = ss.str();

                                  log(logger::LogLevel::Debug, "Executor: IoService::Run() in thread " + thread_id);
#else
                                  static_cast<void>(log);
#endif

                                  io_context->run();

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
                                  log(logger::LogLevel::Debug, "Executor: IoService: Shutdown of thread " + thread_id);
#endif

                                });
    }
  }

  void Executor_Impl::shutdown()
  {
    if (shut_down_.exchange(true))
      return;

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "Executor: Shutting down.");
#endif

    // Remove the dummy work. From now on, io_context::run() returns as soon
    // as all remaining work (e.g. sockets that are being closed) is done.
    dummy_work_.reset();

    const std::thread::id this_thread_id = std::this_thread::get_id();
    const bool called_from_worker = std::any_of(thread_pool_.begin(), thread_pool_.end(),
                                                [&this_thread_id](const std::thread& thread) { return thread.get_id() == this_thread_id; });

    if (called_from_worker)
    {
      // A thread cannot join itself and waiting for the other workers could
      // block them from finishing (e.g. work that is queued behind the handler
      // that is currently running on this thread). The workers finish the
      // remaining work on their own.
      for (std::thread& thread : thread_pool_)
      {
        thread.detach();
      }
      thread_pool_.clear();
      return;
    }

    for (std::thread& thread : thread_pool_)
    {
      if (thread.joinable())
        thread.join();
    }
    thread_pool_.clear();

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "Executor: All IoService threads have shut down successfully.");
#endif
  }

  std::shared_ptr<asio::io_context> Executor_Impl::ioService() const
  {
    return io_context_;
  }

  logger::logger_t Executor_Impl::logFunction() const
  {
    return log_;
  }

}
