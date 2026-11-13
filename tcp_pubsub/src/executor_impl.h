// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include <asio.hpp> // IWYU pragma: keep

#include "tcp_pubsub/tcp_pubsub_logger.h"

namespace tcp_pubsub
{
  class Executor_Impl
  {
  public:
    Executor_Impl(const logger::logger_t& log_function);
    ~Executor_Impl();

    // Copy
    Executor_Impl(const Executor_Impl&)            = delete;
    Executor_Impl& operator=(const Executor_Impl&) = delete;

    // Move
    Executor_Impl& operator=(Executor_Impl&&)      = delete;
    Executor_Impl(Executor_Impl&&)                 = delete;

  public:
    void start(size_t thread_count);

    /**
     * @brief Stops the worker threads and waits for them to terminate.
     *
     * Lets the io_context finish all work that is still pending (e.g. the
     * closing of sockets that have been cancelled) and joins all worker
     * threads. After this function has returned, no worker thread is running
     * any more and no handler will be executed any more.
     *
     * There is no forced-stop timeout: running handlers must return and
     * pending asynchronous operations must finish or be cancelled.
     *
     * If this function is called from one of the worker threads, it cannot
     * wait for the workers (a thread cannot join itself). In that case the
     * workers finish the pending work on their own and the threads are
     * detached. The workers only reference state that they own themselves, so
     * they are safe to outlive this object.
     *
     * Calling this function multiple times is safe.
     */
    void shutdown();

    std::shared_ptr<asio::io_context> ioService()   const;
    logger::logger_t                  logFunction() const;

  /////////////////////////////////////////
  // Member variables
  ////////////////////////////////////////

  private:
    const logger::logger_t                  log_;             /// Logger
    std::shared_ptr<asio::io_context>       io_context_;      /// global io service

    std::vector<std::thread>                thread_pool_;     /// Asio threadpool executing the io servic
    std::atomic<bool>                       shut_down_;       /// Whether shutdown() has been called already

    using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;
    std::shared_ptr<work_guard_t>           dummy_work_;      /// Dummy work, so the io_context will never run out of work and shut down, even if there is no publisher or subscriber at the moment
  };
}
