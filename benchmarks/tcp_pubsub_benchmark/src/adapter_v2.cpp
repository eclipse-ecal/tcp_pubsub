// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

// Adapter for the tcp_pubsub 2.0.1 public API.

#include "adapter.h"

#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <tcp_pubsub/callback_data.h>
#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>
#include <tcp_pubsub/subscriber_session.h>

namespace bench
{
  namespace
  {
    // Only warnings and worse: logging of every connection event would
    // disturb the measurement.
    void quietLogger(const tcp_pubsub::logger::LogLevel level, const std::string& message)
    {
      if (level >= tcp_pubsub::logger::LogLevel::Warning)
      {
        std::cerr << "[tcp_pubsub] " << message << std::endl;
      }
    }

    class V2Publisher : public IPublisher
    {
    public:
      explicit V2Publisher(const std::shared_ptr<tcp_pubsub::Executor>& executor)
        : publisher_(executor, "127.0.0.1", 0)
      {}

      uint16_t port() const override            { return publisher_.getPort(); }
      size_t   subscriberCount() const override { return publisher_.getSubscriberCount(); }
      void     publish(const void* data, size_t size) override
      {
        publisher_.send(static_cast<const char*>(data), size);
      }

    private:
      tcp_pubsub::Publisher publisher_;
    };

    class V2Subscriber : public ISubscriber
    {
    public:
      V2Subscriber(const std::shared_ptr<tcp_pubsub::Executor>& executor, bool inline_delivery)
        : subscriber_(executor)
        , inline_delivery_(inline_delivery)
      {}

      void connect(const std::string& host, uint16_t port) override
      {
        sessions_.push_back(subscriber_.addSession(host, port, -1));
      }

      void setHandler(MessageHandler handler) override
      {
        subscriber_.setCallback([handler](const tcp_pubsub::CallbackData& data)
                                {
                                  handler(data.buffer_->data(), data.buffer_->size());
                                }
                                , inline_delivery_);
      }

      bool allConnected() const override
      {
        for (const auto& session : sessions_)
        {
          if (!session->isConnected()) return false;
        }
        return !sessions_.empty();
      }

    private:
      tcp_pubsub::Subscriber                                subscriber_;
      bool                                                  inline_delivery_;
      std::vector<std::shared_ptr<tcp_pubsub::SubscriberSession>> sessions_;
    };

    class V2Context : public IContext
    {
    public:
      explicit V2Context(const AdapterOptions& options)
        : options_ (options)
        , executor_(std::make_shared<tcp_pubsub::Executor>(options.io_threads, quietLogger))
      {}

      std::unique_ptr<IPublisher> createPublisher() override
      {
        return std::unique_ptr<IPublisher>(new V2Publisher(executor_));
      }

      std::unique_ptr<ISubscriber> createSubscriber() override
      {
        return std::unique_ptr<ISubscriber>(new V2Subscriber(executor_, options_.inline_delivery));
      }

    private:
      AdapterOptions                        options_;
      std::shared_ptr<tcp_pubsub::Executor> executor_;
    };
  }

  std::unique_ptr<IContext> createContext(const AdapterOptions& options)
  {
    return std::unique_ptr<IContext>(new V2Context(options));
  }

  const char* adapterName() { return "tcp_pubsub-v2 (2.0.1 API)"; }
}
