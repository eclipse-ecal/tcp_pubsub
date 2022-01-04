// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <memory>
#include <string>
#include <stdint.h>

#include "tcp_pubsub_version.h"
#include "tcp_pubsub_export.h"

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
     * @brief Get the address used when creating the Session
     * 
     * @return The address / hostname of this Session
     */
    TCP_PUBSUB_EXPORT std::string getAddress() const;

    /**
     * @brief Get the port used when creating the Session.
     * 
     * @return The port this Session is connecting to
     */
    TCP_PUBSUB_EXPORT uint16_t    getPort()    const;

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