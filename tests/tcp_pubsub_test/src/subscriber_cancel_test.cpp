// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <gtest/gtest.h>
#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/subscriber.h>
#include <tcp_pubsub/subscriber_session.h>

#include "portable_endian.h"
#include "tcp_header.h"

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

  class SubscriberCancellationTest : public testing::Test
  {
  protected:
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(
      4, [](tcp_pubsub::logger::LogLevel, const std::string&) {});

    void SetUp() override
    {
      acceptor.non_blocking(true);
    }

    bool Accept(asio::ip::tcp::socket& socket)
    {
      return WaitUntil([&]()
      {
        asio::error_code ec;
        acceptor.accept(socket, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) return false;
        if (ec) throw asio::system_error(ec);
        return true;
      });
    }
  };
}

// Cancels or destroys sessions with incomplete reads as peers close concurrently.
TEST_F(SubscriberCancellationTest, PendingReadAndDestruction)
{
  for (int iteration = 0; iteration < 100; ++iteration)
  {
    SCOPED_TRACE(iteration);
    auto subscriber = std::make_unique<tcp_pubsub::Subscriber>(executor);
    std::vector<std::shared_ptr<tcp_pubsub::SubscriberSession>> sessions;
    std::vector<asio::ip::tcp::socket> sockets;
    for (int i = 0; i < 4; ++i)
    {
      sessions.push_back(subscriber->addSession("127.0.0.1", acceptor.local_endpoint().port(), 0));
      sockets.emplace_back(io);
      ASSERT_TRUE(Accept(sockets.back()));
      ASSERT_TRUE(WaitUntil([&]() { return sockets.back().available() > 0; }));

      // Leave the subscriber's composed header read incomplete.
      const char partial_header = sizeof(tcp_pubsub::TcpHeader);
      asio::write(sockets.back(), asio::buffer(&partial_header, 1));
    }

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    int ready_threads = 0;
    bool start_cleanup = false;
    const auto wait_for_cleanup = [&]()
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      ++ready_threads;
      gate_condition.notify_all();
      gate_condition.wait(lock, [&]() { return start_cleanup; });
    };

    std::thread cancellation_thread([&]()
    {
      wait_for_cleanup();
      if (iteration % 2 == 0)
        subscriber->cancel();
      else
        subscriber.reset();
    });
    std::thread peer_close_thread([&]()
    {
      wait_for_cleanup();
      for (auto& socket : sockets)
      {
        asio::error_code ec;
        socket.close(ec);
      }
    });

    bool gate_ready = false;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      gate_ready = gate_condition.wait_for(lock, 5s, [&]() { return ready_threads == 2; });
      start_cleanup = true;
    }
    gate_condition.notify_all();
    cancellation_thread.join();
    peer_close_thread.join();
    ASSERT_TRUE(gate_ready);

    subscriber.reset();
    for (const auto& session : sessions)
    {
      ASSERT_TRUE(WaitUntil([&]() { return session->getConnectedPublisher().first.empty(); }));
      EXPECT_FALSE(session->isConnected());
      session->cancel();
    }
  }
}

// Stresses concurrent socket status queries and cancellation for ThreadSanitizer.
TEST_F(SubscriberCancellationTest, ConcurrentStatusQueriesAndCancel)
{
  for (int iteration = 0; iteration < 50; ++iteration)
  {
    SCOPED_TRACE(iteration);
    tcp_pubsub::Subscriber subscriber(executor);
    auto session = subscriber.addSession("127.0.0.1", acceptor.local_endpoint().port(), 0);
    asio::ip::tcp::socket socket(io);
    ASSERT_TRUE(Accept(socket));
    ASSERT_TRUE(WaitUntil([&]() { return socket.available() > 0; }));
    ASSERT_TRUE(WaitUntil([&]() { return session->isConnected(); }));

    std::atomic<bool> stop_queries{false};
    std::atomic<int> query_count{0};
    std::thread status_thread([&]()
    {
      while (!stop_queries.load())
      {
        static_cast<void>(session->isConnected());
        ++query_count;
      }
    });

    const bool queries_started = WaitUntil([&]() { return query_count.load() >= 100; });
    if (!queries_started)
    {
      stop_queries = true;
      status_thread.join();
      FAIL() << "Status-query thread did not start";
    }

    session->cancel();
    stop_queries = true;
    status_thread.join();

    EXPECT_FALSE(session->isConnected());
    EXPECT_TRUE(session->getConnectedPublisher().first.empty());
    EXPECT_TRUE(WaitUntil([&]() { return subscriber.getSessions().empty(); }));
  }
}

// Repeats cancellation during startup and verifies that sessions are removed.
TEST_F(SubscriberCancellationTest, CancelBeforeResolveCompletes)
{
  tcp_pubsub::Subscriber subscriber(executor);
  for (int iteration = 0; iteration < 200; ++iteration)
  {
    auto session = subscriber.addSession("127.0.0.1", acceptor.local_endpoint().port(), 0);
    session->cancel();
    session->cancel();
    ASSERT_TRUE(WaitUntil([&]() { return subscriber.getSessions().empty(); }));
    EXPECT_FALSE(session->isConnected());
  }
}

// Cancels after a failed connection and verifies that the session is removed.
TEST_F(SubscriberCancellationTest, CancelPendingRetry)
{
  std::atomic<int> failures{0};
  auto retry_executor = std::make_shared<tcp_pubsub::Executor>(
    4, [&failures](tcp_pubsub::logger::LogLevel level, const std::string& message)
    {
      if (level == tcp_pubsub::logger::LogLevel::Warning
          && message.find("Failed connecting") != std::string::npos) ++failures;
    });

  // Reserve a local port without listening, so connecting fails without DNS/network dependencies.
  asio::ip::tcp::acceptor reserved(io);
  reserved.open(asio::ip::tcp::v4());
  reserved.bind({asio::ip::address_v4::loopback(), 0});
  tcp_pubsub::Subscriber subscriber(retry_executor);
  auto session = subscriber.addSession("127.0.0.1", reserved.local_endpoint().port());
  ASSERT_TRUE(WaitUntil([&]() { return failures.load() > 0; }));
  session->cancel();
  ASSERT_TRUE(WaitUntil([&]() { return subscriber.getSessions().empty(); }));
  EXPECT_FALSE(session->isConnected());
}

// Verifies that a synchronous callback can cancel its own session.
TEST_F(SubscriberCancellationTest, CancelFromSynchronousCallback)
{
  tcp_pubsub::Subscriber subscriber(executor);
  auto session = subscriber.addSession("127.0.0.1", acceptor.local_endpoint().port(), 0);
  std::atomic<int> callbacks{0};
  subscriber.setCallback([session, &callbacks](const tcp_pubsub::CallbackData&)
  {
    session->cancel();
    ++callbacks;
  }, true);

  asio::ip::tcp::socket socket(io);
  ASSERT_TRUE(Accept(socket));
  ASSERT_TRUE(WaitUntil([&]() { return socket.available() > 0; }));
  tcp_pubsub::TcpHeader header;
  header.header_size = htole16(sizeof(header));
  header.data_size = htole64(1);
  const char payload = 'x';
  const std::vector<asio::const_buffer> buffers{asio::buffer(&header, sizeof(header)), asio::buffer(&payload, 1)};
  asio::write(socket, buffers);

  ASSERT_TRUE(WaitUntil([&]() { return subscriber.getSessions().empty(); }));
  EXPECT_EQ(callbacks.load(), 1);
  EXPECT_FALSE(session->isConnected());
}

// Requests cancellation from the test thread while a synchronous callback is blocked.
TEST_F(SubscriberCancellationTest, CancelWhileSynchronousCallbackIsActive)
{
  tcp_pubsub::Subscriber subscriber(executor);
  auto session = subscriber.addSession("127.0.0.1", acceptor.local_endpoint().port(), 0);
  std::mutex callback_mutex;
  std::condition_variable callback_condition;
  bool callback_entered = false;
  bool release_callback = false;
  std::atomic<int> callbacks{0};

  subscriber.setCallback([&](const tcp_pubsub::CallbackData&)
  {
    std::unique_lock<std::mutex> lock(callback_mutex);
    callback_entered = true;
    callback_condition.notify_one();
    callback_condition.wait(lock, [&]() { return release_callback; });
    ++callbacks;
  }, true);

  asio::ip::tcp::socket socket(io);
  ASSERT_TRUE(Accept(socket));
  ASSERT_TRUE(WaitUntil([&]() { return socket.available() > 0; }));
  tcp_pubsub::TcpHeader header;
  header.header_size = htole16(sizeof(header));
  header.data_size = htole64(1);
  const char payload = 'x';
  const std::vector<asio::const_buffer> buffers{asio::buffer(&header, sizeof(header)), asio::buffer(&payload, 1)};
  asio::write(socket, buffers);

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(callback_mutex);
    entered = callback_condition.wait_for(lock, 5s, [&]() { return callback_entered; });
  }

  session->cancel();
  if (entered)
  {
    EXPECT_FALSE(session->isConnected());
    EXPECT_TRUE(session->getConnectedPublisher().first.empty());
  }
  {
    const std::lock_guard<std::mutex> lock(callback_mutex);
    release_callback = true;
  }
  callback_condition.notify_one();

  ASSERT_TRUE(entered);
  ASSERT_TRUE(WaitUntil([&]() { return subscriber.getSessions().empty(); }));
  EXPECT_EQ(callbacks.load(), 1);
  EXPECT_FALSE(session->isConnected());
}
