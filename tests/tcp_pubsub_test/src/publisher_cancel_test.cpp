// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <gtest/gtest.h>
#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>
#include <tcp_pubsub/subscriber_session.h>

namespace
{
  using namespace std::chrono_literals;

  template<typename Predicate>
  bool WaitUntil(Predicate predicate)
  {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }
}

TEST(PublisherCancellation, StartupLogsCachedEndpointsOnSingleThreadExecutor)
{
  std::mutex log_mutex;
  std::vector<std::string> connection_logs;
  auto executor = std::make_shared<tcp_pubsub::Executor>(
    1, [&](tcp_pubsub::logger::LogLevel level, const std::string& message)
    {
      if (level == tcp_pubsub::logger::LogLevel::Info
          && message.find(" has connected.") != std::string::npos)
      {
        const std::lock_guard<std::mutex> lock(log_mutex);
        connection_logs.push_back(message);
      }
    });
  tcp_pubsub::Publisher publisher(executor, "127.0.0.1", 0);
  ASSERT_TRUE(publisher.isRunning());

  asio::io_context io;
  asio::ip::tcp::socket peer(io);
  peer.connect({asio::ip::address_v4::loopback(), publisher.getPort()});
  const std::string expected_log = "Publisher 127.0.0.1:" + std::to_string(publisher.getPort())
    + ": Subscriber 127.0.0.1:" + std::to_string(peer.local_endpoint().port()) + " has connected.";

  ASSERT_TRUE(WaitUntil([&]()
  {
    const std::lock_guard<std::mutex> lock(log_mutex);
    return !connection_logs.empty();
  }));
  {
    const std::lock_guard<std::mutex> lock(log_mutex);
    ASSERT_EQ(connection_logs.size(), 1u);
    EXPECT_EQ(connection_logs.front(), expected_log);
  }

  publisher.cancel();
  EXPECT_TRUE(WaitUntil([&]() { return publisher.getSubscriberCount() == 0; }));
}

// Cancels / destroys the publisher while subscribers are connected and data is being sent.
TEST(PublisherCancellation, CancelDuringActiveTransmission)
{
  auto executor = std::make_shared<tcp_pubsub::Executor>(4, [](tcp_pubsub::logger::LogLevel, const std::string&) {});

  for (int iteration = 0; iteration < 50; ++iteration)
  {
    SCOPED_TRACE(iteration);

    auto publisher = std::make_unique<tcp_pubsub::Publisher>(executor, "127.0.0.1", 0);
    ASSERT_TRUE(publisher->isRunning());

    tcp_pubsub::Subscriber subscriber(executor);
    std::vector<std::shared_ptr<tcp_pubsub::SubscriberSession>> sessions;
    // Shared with the callback, which may still run while the subscriber is being destroyed
    const auto received = std::make_shared<std::atomic<int>>(0);
    subscriber.setCallback([received](const tcp_pubsub::CallbackData&) { ++(*received); });
    for (int i = 0; i < 4; ++i)
    {
      sessions.push_back(subscriber.addSession("127.0.0.1", publisher->getPort(), 0));
    }
    ASSERT_TRUE(WaitUntil([&]() { return publisher->getSubscriberCount() == 4; }));

    std::atomic<bool> stop_sending{false};
    std::thread sender([&]()
    {
      const std::string payload(64 * 1024, 'x');
      while (!stop_sending)
        publisher->send(payload.data(), payload.size());
    });

    const bool received_data = WaitUntil([&]() { return received->load() > 0; });
    if (!received_data)
    {
      stop_sending = true;
      sender.join();
      publisher->cancel();
    }
    ASSERT_TRUE(received_data);

    // Concurrent status queries while the sessions are being canceled
    std::thread status_thread([&]()
    {
      while (!stop_sending)
        static_cast<void>(publisher->getSubscriberCount());
    });

    if (iteration % 2 == 0)
      publisher->cancel();
    else
      sessions.front()->cancel();

    stop_sending = true;
    sender.join();
    status_thread.join();

    if (iteration % 2 == 0)
    {
      EXPECT_TRUE(WaitUntil([&]() { return publisher->getSubscriberCount() == 0; }));
    }
    publisher.reset();
  }
}

// Subscribers connect and disconnect repeatedly while the publisher sends.
TEST(PublisherCancellation, SubscriberChurnWhileSending)
{
  auto executor = std::make_shared<tcp_pubsub::Executor>(4, [](tcp_pubsub::logger::LogLevel, const std::string&) {});
  tcp_pubsub::Publisher publisher(executor, "127.0.0.1", 0);

  std::atomic<bool> stop_sending{false};
  std::thread sender([&]()
  {
    const std::string payload(16 * 1024, 'y');
    while (!stop_sending)
      publisher.send(payload.data(), payload.size());
  });

  for (int iteration = 0; iteration < 50; ++iteration)
  {
    tcp_pubsub::Subscriber subscriber(executor);
    auto session = subscriber.addSession("127.0.0.1", publisher.getPort(), 0);
    std::this_thread::sleep_for(2ms);
    session->cancel();
  }

  stop_sending = true;
  sender.join();
  publisher.cancel();
  EXPECT_TRUE(WaitUntil([&]() { return publisher.getSubscriberCount() == 0; }));
}
