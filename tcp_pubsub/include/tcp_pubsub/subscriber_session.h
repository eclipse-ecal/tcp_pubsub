// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tcp_pubsub/tcp_pubsub_version.h> // IWYU pragma: keep
#include <tcp_pubsub/tcp_pubsub_export.h>

namespace tcp_pubsub
{
  // Forward-declare Implementation
  class SubscriberSession_Impl;

  // Friend class
  class Subscriber_Impl;

  /**
   * @brief A Single connection to a publisher
   * 
   * A SubscriberSessions represents a single connection to a single Publisher.
   * SubscriberSessions never exist on their own; they always belong to a
   * Subscriber.
   * 
   * A SubscriberSession is created by Subscriber::addSession().
   */
  class SubscriberSession
  {
  friend Subscriber_Impl;

  private:
    TCP_PUBSUB_EXPORT SubscriberSession(const std::shared_ptr<SubscriberSession_Impl>& impl);

  public:
    // Copy
    TCP_PUBSUB_EXPORT SubscriberSession(const SubscriberSession&)            = default;
    TCP_PUBSUB_EXPORT SubscriberSession& operator=(const SubscriberSession&) = default;

    // Move
    TCP_PUBSUB_EXPORT SubscriberSession& operator=(SubscriberSession&&)      = default;
    TCP_PUBSUB_EXPORT SubscriberSession(SubscriberSession&&)                 = default;

    // Destructor
    TCP_PUBSUB_EXPORT ~SubscriberSession();

  public:
    /**
     * @brief Get the list of publishers used for creating the session.
     * 
     * The session will try to connect to the publishers in the order they are
     * present in that list. If a connection fails, it will try the next one.
     * 
     * For checking which publisher is currently connected, use getConnectedPublisher().
     * 
     * @return The list of publishers used for creating the session
     */
    TCP_PUBSUB_EXPORT std::vector<std::pair<std::string, uint16_t>> getPublisherList() const;

    /**
     * @brief Get the currently connected publisher address
     * 
     * @return The address and port of the currently connected publisher
     */
    TCP_PUBSUB_EXPORT std::pair<std::string, uint16_t>              getConnectedPublisher() const;

    /**
     * @brief Cancels the Session
     * 
     * This cancels the Session and closes the connection to the Publisher. 
     * If will automatically cause the SubscriberSession to remove itself from
     * the Subscriber it was created from. Once you release the shared_ptr to
     * it, the object will be deleted.
     */
    TCP_PUBSUB_EXPORT void        cancel();

    /**
     * @brief Returns whether this Session is connected to a Publisher
     * 
     * @return True, if the Session is connected to a publisher.
     */
    TCP_PUBSUB_EXPORT bool        isConnected() const;

  private:
    std::shared_ptr<SubscriberSession_Impl> subscriber_session_impl_;
  };
}
