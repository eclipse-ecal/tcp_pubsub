// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <tcp_pubsub/executor.h>

#include <cstddef>
#include <memory>
#include <utility>

#include "executor_impl.h"
#include "tcp_pubsub/tcp_pubsub_logger.h"

namespace tcp_pubsub
{
  Executor::Executor(size_t thread_count, const tcp_pubsub::logger::logger_t & log_function)
    : executor_impl_(std::make_shared<Executor_Impl>(log_function))
  {
    executor_impl_->start(thread_count);
  }

  Executor::~Executor()
  {
    // The executor may have been moved from
    if (executor_impl_)
      executor_impl_->shutdown();
  }

  // Move
  Executor& Executor::operator=(Executor&& other) noexcept
  {
    if (this != &other)
    {
      // The executor that gets replaced has to be shut down properly
      if (executor_impl_)
        executor_impl_->shutdown();

      executor_impl_ = std::move(other.executor_impl_);
    }
    return *this;
  }

  Executor::Executor(Executor&&) noexcept                 = default;
}