// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <tcp_pubsub/subscriber.h>

#include <tcp_pubsub/executor.h>

#include "subscriber_impl.h"
#include "executor_impl.h"

namespace tcp_pubsub
{
  Subscriber::Subscriber(const std::shared_ptr<Executor>& executor)
    : subscriber_impl_(std::make_shared<Subscriber_Impl>(executor))
  {}

  Subscriber::~Subscriber()
  {
    subscriber_impl_->cancel();
  }

  std::shared_ptr<SubscriberSession> Subscriber::addSession(const std::string& address, uint16_t port, int max_reconnection_attempts)
    { return subscriber_impl_->addSession(address, port, max_reconnection_attempts); }

  std::vector<std::shared_ptr<SubscriberSession>> Subscriber::getSessions() const
    { return subscriber_impl_->getSessions(); }

  void Subscriber::setCallback(const std::function<void(const CallbackData& callback_data)>& callback_function, bool synchronous_execution)
    { subscriber_impl_->setCallback(callback_function, synchronous_execution); }

  void Subscriber::clearCallback()
    { subscriber_impl_->setCallback([](const auto&){}, true); }

  void Subscriber::cancel()
    { subscriber_impl_->cancel(); }
}
