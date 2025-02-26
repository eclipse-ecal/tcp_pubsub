// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <recycle/shared_pool.hpp>

#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/subscriber_session.h>
#include <tcp_pubsub/callback_data.h>

#include "tcp_pubsub/tcp_pubsub_logger.h"

namespace tcp_pubsub {
class Subscriber_Impl : public std::enable_shared_from_this<Subscriber_Impl>
{
  ////////////////////////////////////////////////
  // Constructor & Destructor
  ////////////////////////////////////////////////
  public:
    // Constructor
    Subscriber_Impl(const std::shared_ptr<Executor>& executor);

    // Copy
    Subscriber_Impl(const Subscriber_Impl&)            = delete;
    Subscriber_Impl& operator=(const Subscriber_Impl&) = delete;

    // Move
    Subscriber_Impl& operator=(Subscriber_Impl&&)      = delete;
    Subscriber_Impl(Subscriber_Impl&&)                 = delete;

    // Destructor
    ~Subscriber_Impl();

  ////////////////////////////////////////////////
  // Session Management
  ////////////////////////////////////////////////
  public: 
    std::shared_ptr<SubscriberSession>              addSession(const std::vector<std::pair<std::string, uint16_t>>& publisher_list, int max_reconnection_attempts);
    std::vector<std::shared_ptr<SubscriberSession>> getSessions() const;

    void setCallback(const std::function<void(const CallbackData& callback_data)>& callback_function, bool synchronous_execution);
  private:
    void setCallbackToSession(const std::shared_ptr<SubscriberSession>& session);

  public:
    void cancel();

  private:
    std::string subscriberIdString() const;

  ////////////////////////////////////////////////
  // Member variables
  ////////////////////////////////////////////////
  private:
    // Asio
    const std::shared_ptr<Executor>                 executor_;                 /// Global Executor

    // List of all Sessions
    mutable std::mutex                              session_list_mutex_;
    std::vector<std::shared_ptr<SubscriberSession>> session_list_;

    // Callback
    mutable std::mutex                              last_callback_data_mutex_;
    std::condition_variable                         last_callback_data_cv_;
    CallbackData                                    last_callback_data_;

    std::atomic<bool>                               user_callback_is_synchronous_;
    std::function<void(const CallbackData&)>        synchronous_user_callback_;

    std::unique_ptr<std::thread>                    callback_thread_;
    std::atomic<bool>                               callback_thread_stop_;

    // Buffer pool
    struct buffer_pool_lock_policy_
    {
      using mutex_type = std::mutex;
      using lock_type  = std::lock_guard<mutex_type>;
    };
    recycle::shared_pool<std::vector<char>, buffer_pool_lock_policy_> buffer_pool_;                 /// Buffer pool that let's us reuse memory chunks

    // Log function
    const tcp_pubsub::logger::logger_t log_;
  };
  } // namespace tcp_pubsub
