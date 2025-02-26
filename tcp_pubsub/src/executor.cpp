// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <tcp_pubsub/executor.h>

#include <cstddef>
#include <memory>

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
    executor_impl_->stop();
  }

  // Move
  Executor& Executor::operator=(Executor&&) noexcept      = default;
  Executor::Executor(Executor&&) noexcept                 = default;
}