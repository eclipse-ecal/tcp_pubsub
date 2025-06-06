// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <cstddef>
#include <memory>

#include "tcp_pubsub_logger.h"

#include <tcp_pubsub/tcp_pubsub_version.h> // IWYU pragma: keep
#include <tcp_pubsub/tcp_pubsub_export.h>

namespace tcp_pubsub
{
  // Foward-declare implementation
  class Executor_Impl;

  // Forward-declare friend classes
  class Publisher_Impl;
  class Subscriber_Impl;

  /**
   * @brief The Executor executes publisher and subscriber workload using a thread pool.
   *
   * The Exectur has to be passed as shared_ptr to the constructor of any
   * Publisher and Subscriber. It contains two things:
   *
   *  - A thread pool that executes workload generated by Publisher and
   *    Subsriber connection (e.g. sending and receiving data)
   *  - A logging function that gets used when output has to be logged.
   *
   */
  class Executor
  {
  public:
    /**
     * @brief Creates a new executor
     *
     * @param[in] thread_count
     *              The amount of threads that shall execute workload
     *
     * @param[in] log_function
     *              A function (LogLevel, string)->void that is used for logging.
     */
    TCP_PUBSUB_EXPORT Executor(size_t thread_count, const tcp_pubsub::logger::logger_t& log_function = tcp_pubsub::logger::default_logger);
    TCP_PUBSUB_EXPORT ~Executor();

    // Copy
    Executor(const Executor&)            = delete;
    Executor& operator=(const Executor&) = delete;

    // Move
    TCP_PUBSUB_EXPORT Executor& operator=(Executor&&) noexcept;
    TCP_PUBSUB_EXPORT Executor(Executor&&) noexcept;

  private:
    friend ::tcp_pubsub::Publisher_Impl;
    friend ::tcp_pubsub::Subscriber_Impl;
    std::shared_ptr<Executor_Impl> executor_impl_;
  };
}
