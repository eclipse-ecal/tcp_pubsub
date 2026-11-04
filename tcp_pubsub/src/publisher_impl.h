// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <recycle/shared_pool.hpp>

#include "publisher_session.h"
#include "tcp_pubsub/tcp_pubsub_logger.h"
#include <tcp_pubsub/executor.h>

namespace tcp_pubsub
{
  class Publisher_Impl : public std::enable_shared_from_this<Publisher_Impl>
  {

  ////////////////////////////////////////////////
  // Constructor & Destructor
  ////////////////////////////////////////////////
  
  public:
    // Constructor
    Publisher_Impl(const std::shared_ptr<Executor>& executor);

    // Copy
    Publisher_Impl(const Publisher_Impl&)            = delete;
    Publisher_Impl& operator=(const Publisher_Impl&) = delete;

    // Move
    Publisher_Impl& operator=(Publisher_Impl&&)      = delete;
    Publisher_Impl(Publisher_Impl&&)                 = delete;

    // Destructor
    ~Publisher_Impl();

  ////////////////////////////////////////////////
  // Start & Stop
  ////////////////////////////////////////////////
  
  public:
    bool start(const std::string& address, uint16_t port);
    void cancel();

    /**
     * @brief Releases the reference to the Executor that is only needed to keep it alive.
     *
     * Called by the destructor of the public Publisher (after cancel()), so
     * the Executor shuts down on the thread of the user and not on one of its
     * own worker threads, when the Publisher was the last user of it.
     * The Publisher must not be started or used after that.
     */
    void releaseExecutor();

  private:
    void acceptClient();

  ////////////////////////////////////////////////
  // Send data
  ////////////////////////////////////////////////
  
  public:
    bool send(const std::vector<std::pair<const char* const, const size_t>>& payloads);

  ////////////////////////////////////////////////
  // (Status-) getters
  ////////////////////////////////////////////////

  public:
    uint16_t           getPort()            const;
    size_t             getSubscriberCount() const;

    bool               isRunning()          const;

  private:
    std::string toString(const asio::ip::tcp::endpoint& endpoint) const;
    std::string localEndpointToString() const;

  ////////////////////////////////////////////////
  // Member variables
  ////////////////////////////////////////////////
  
  private:
    std::atomic<bool> is_running_;                                              /// Indicates whether this publisher is running and can send data. May be false, if e.g. binding to the given address has failed.

    // Asio
    // The executor is shared between all users of it. executor_keepalive_ only keeps it alive and is never
    // read after the construction, so it can be reset by releaseExecutor() without synchronization.
    // Everything that is used from the worker threads is cached in io_context_ and log_ instead, which never change.
    std::shared_ptr<Executor>                      executor_keepalive_;         /// Keeps the executor alive while the publisher exists; reset by releaseExecutor().
    const std::shared_ptr<asio::io_context>        io_context_;                 /// The io_context of the executor. Used for creating the acceptor and the sessions.
    asio::ip::tcp::acceptor                        acceptor_;                   /// Acceptor used for waiting for clients (i.e. subscribers)
                                                
    // Logger                                    
    const logger::logger_t                         log_;                        /// Function for logging
                                                   
    // Sessions                                       
    mutable std::mutex                             publisher_sessions_mutex_;   
    std::vector<std::shared_ptr<PublisherSession>> publisher_sessions_;         /// List of all sessions (i.e. connections to subsribers)

    // Buffer pool
    struct buffer_pool_lock_policy_
    {
      using mutex_type = std::mutex;
      using lock_type  = std::lock_guard<mutex_type>;
    };
    recycle::shared_pool<std::vector<char>, buffer_pool_lock_policy_> buffer_pool; /// Buffer pool that let's us reuse memory chunks
  };
}
