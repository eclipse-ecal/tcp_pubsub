// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>

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

  // The Executor copies its logger into everything it creates (publishers,
  // subscribers, sessions). The shared counter captured by the logger therefore
  // tells us how many of those objects are still alive: use_count() == 1 means
  // that the test is the only owner left, i.e. everything has been destroyed.
  using LogCounter = std::shared_ptr<std::atomic<int>>;

  tcp_pubsub::logger::logger_t makeCountingLogger(const LogCounter& counter)
  {
    return [counter](tcp_pubsub::logger::LogLevel, const std::string&) { ++(*counter); };
  }

  // Sends a message from a publisher to a subscriber, so that there are
  // sessions and pending asynchronous operations on the executor.
  struct Traffic
  {
    explicit Traffic(const std::shared_ptr<tcp_pubsub::Executor>& executor,
                     const std::shared_ptr<std::atomic<int>>&     received_counter,
                     const std::shared_ptr<tcp_pubsub::Executor>& callback_keepalive = nullptr)
      : publisher (executor, "127.0.0.1", 0)
      , subscriber(executor)
    {
      // The callback may hold an additional reference to the executor
      subscriber.setCallback([received_counter, callback_keepalive](const tcp_pubsub::CallbackData&)
                             { ++(*received_counter); }, true);
      subscriber.addSession("127.0.0.1", publisher.getPort(), 0);
    }

    bool exchangeMessage(const std::shared_ptr<std::atomic<int>>& received_counter)
    {
      if (!WaitUntil([&]() { return publisher.getSubscriberCount() == 1; })) return false;
      const std::string message = "Hello World";
      publisher.send(message.data(), message.size());
      return WaitUntil([&]() { return received_counter->load() > 0; });
    }

    tcp_pubsub::Publisher  publisher;
    tcp_pubsub::Subscriber subscriber;
  };
}

// When the user destroys the executor and everything that was created from it,
// the shutdown must be complete once the last destructor has returned.
// Nothing may keep running in the background, and no logger call may happen
// afterwards, as the logger may capture objects that are gone by then.
TEST(ExecutorShutdown, DestructorReturnsOnlyAfterEverythingIsTornDown)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto       received    = std::make_shared<std::atomic<int>>(0);

  {
    auto executor = std::make_shared<tcp_pubsub::Executor>(2, makeCountingLogger(log_counter));
    Traffic traffic(executor, received);
    ASSERT_TRUE(traffic.exchangeMessage(received));
    // Destruction order: traffic (subscriber, publisher), then the executor
  }

  // No wait here: the shutdown has to be finished already.
  EXPECT_EQ(log_counter.use_count(), 1) << "Objects created by the executor are still alive after the executor has been destroyed.";

  const int calls_after_shutdown = log_counter->load();
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(log_counter->load(), calls_after_shutdown) << "The logger has been called after the executor has been destroyed.";
}

// Same as above, but the executor is destroyed while sessions are still
// connected, i.e. publisher and subscriber are destroyed after the executor
// handle of the user is gone.
TEST(ExecutorShutdown, UserReleasesExecutorHandleBeforePublisherAndSubscriber)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto       received    = std::make_shared<std::atomic<int>>(0);

  {
    std::unique_ptr<Traffic> traffic;
    {
      auto executor = std::make_shared<tcp_pubsub::Executor>(2, makeCountingLogger(log_counter));
      traffic       = std::make_unique<Traffic>(executor, received);
      ASSERT_TRUE(traffic->exchangeMessage(received));
      // The user's handle to the executor goes away first. The publisher and
      // subscriber keep the executor running.
    }
    EXPECT_GT(log_counter.use_count(), 1);
    traffic.reset();
  }

  EXPECT_EQ(log_counter.use_count(), 1) << "Objects created by the executor are still alive after everything has been destroyed.";
}

// The last reference to the executor is dropped by one of the executor's own
// worker threads (here: the copy that is stored inside the subscriber's
// callback is released when the session gets destroyed). Destroying an
// executor from its own worker thread must neither deadlock nor crash.
TEST(ExecutorShutdown, LastReferenceIsReleasedOnWorkerThread)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto       received    = std::make_shared<std::atomic<int>>(0);

  {
    auto executor = std::make_shared<tcp_pubsub::Executor>(2, makeCountingLogger(log_counter));
    Traffic traffic(executor, received, executor);
    ASSERT_TRUE(traffic.exchangeMessage(received));
  }

  // The worker thread releases the last reference asynchronously, so we have to wait here.
  EXPECT_TRUE(WaitUntil([&]() { return log_counter.use_count() == 1; })) << "The executor has not been torn down.";
}

// A moved-from executor must be safe to destroy.
TEST(ExecutorShutdown, DestroyingMovedFromExecutorIsSafe)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);

  {
    auto first  = std::make_unique<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
    tcp_pubsub::Executor second(std::move(*first));
    first.reset();

    // The moved-to executor still owns the logger
    EXPECT_GT(log_counter.use_count(), 1);
  }

  EXPECT_EQ(log_counter.use_count(), 1);
}

// Move-assigning to an executor must shut down the executor that gets replaced.
TEST(ExecutorShutdown, MoveAssignmentShutsDownReplacedExecutor)
{
  const LogCounter replaced_counter = std::make_shared<std::atomic<int>>(0);
  const LogCounter other_counter    = std::make_shared<std::atomic<int>>(0);

  {
    tcp_pubsub::Executor replaced(1, makeCountingLogger(replaced_counter));
    tcp_pubsub::Executor other   (1, makeCountingLogger(other_counter));
    EXPECT_GT(replaced_counter.use_count(), 1);

    replaced = std::move(other);

    EXPECT_EQ(replaced_counter.use_count(), 1) << "The replaced executor has not been shut down.";
    EXPECT_GT(other_counter.use_count(), 1);
  }

  EXPECT_EQ(other_counter.use_count(), 1);
}
