// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <tcp_pubsub/subscriber_session.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "subscriber_session_impl.h"

namespace tcp_pubsub
{
  SubscriberSession::SubscriberSession(const std::shared_ptr<SubscriberSession_Impl>& impl)
    : subscriber_session_impl_(impl)
  {}

  SubscriberSession::~SubscriberSession()
  {
    subscriber_session_impl_->cancel();
  }

  std::vector<std::pair<std::string, uint16_t>> SubscriberSession::getPublisherList() const
    { return subscriber_session_impl_->getPublisherList(); }

  std::pair<std::string, uint16_t> SubscriberSession::getConnectedPublisher() const
    { return subscriber_session_impl_->getConnectedPublisher(); }

  void SubscriberSession::cancel()
    { subscriber_session_impl_->cancel(); }

  bool SubscriberSession::isConnected() const
    { return subscriber_session_impl_->isConnected(); }
}
