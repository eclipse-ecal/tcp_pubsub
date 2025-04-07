// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include <asio.hpp> // IWYU pragma: keep

#include "tcp_pubsub/tcp_pubsub_logger.h"

namespace tcp_pubsub
{
  class Executor_Impl : public std::enable_shared_from_this<Executor_Impl>
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
    void stop();

    std::shared_ptr<asio::io_context> ioService()   const;
    logger::logger_t                  logFunction() const;



  /////////////////////////////////////////
  // Member variables
  ////////////////////////////////////////

  private:
    const logger::logger_t                  log_;             /// Logger
    std::shared_ptr<asio::io_context>       io_context_;      /// global io service

    std::vector<std::thread>                thread_pool_;     /// Asio threadpool executing the io servic
    using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;
    std::shared_ptr<work_guard_t>           dummy_work_;      /// Dummy work, so the io_context will never run out of work and shut down, even if there is no publisher or subscriber at the moment
  };
}
