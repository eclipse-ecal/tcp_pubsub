// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/executor.h>

#include "executor_impl.h"
#include "publisher_impl.h"

namespace tcp_pubsub
{
  Publisher::Publisher(const std::shared_ptr<Executor>& executor, const std::string& address, uint16_t port)
    : publisher_impl_(std::make_shared<Publisher_Impl>(executor))
  {
    publisher_impl_->start(address, port);
  }

  Publisher::Publisher(const std::shared_ptr<Executor>& executor, uint16_t port)
    : Publisher(executor, "::", port)
  {}

  Publisher::~Publisher()
  { publisher_impl_->cancel(); }

  uint16_t Publisher::getPort() const
    { return publisher_impl_->getPort(); }

  size_t Publisher::getSubscriberCount() const
    { return publisher_impl_->getSubscriberCount(); }

  bool Publisher::isRunning() const
  { return publisher_impl_->isRunning(); }

  bool Publisher::send(const char* const data, size_t size) const
    { return this->send({ {data, size} }); }

  bool Publisher::send(const std::vector<std::pair<const char* const, const size_t>>& buffers) const
    { return publisher_impl_->send(buffers); }

  void Publisher::cancel()
    { publisher_impl_->cancel(); }
}
