// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <tcp_pubsub/subscriber.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "subscriber_impl.h"
#include "tcp_pubsub/callback_data.h"
#include "tcp_pubsub/subscriber_session.h"
#include <tcp_pubsub/executor.h>

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
  { return subscriber_impl_->addSession(std::vector<std::pair<std::string, uint16_t>>{{address, port}}, max_reconnection_attempts); }

  std::shared_ptr<SubscriberSession> Subscriber::addSession(const std::vector<std::pair<std::string, uint16_t>>& publisher_list, int max_reconnection_attempts)
    { return subscriber_impl_->addSession(publisher_list, max_reconnection_attempts); }


  std::vector<std::shared_ptr<SubscriberSession>> Subscriber::getSessions() const
    { return subscriber_impl_->getSessions(); }

  void Subscriber::setCallback(const std::function<void(const CallbackData& callback_data)>& callback_function, bool synchronous_execution)
    { subscriber_impl_->setCallback(callback_function, synchronous_execution); }

  void Subscriber::clearCallback()
    { subscriber_impl_->setCallback([](const auto&){}, true); }

  void Subscriber::cancel()
    { subscriber_impl_->cancel(); }
}
