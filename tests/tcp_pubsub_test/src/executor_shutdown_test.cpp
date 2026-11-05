// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>

#include "executor_impl.h"
#include "publisher_session.h"
#include "subscriber_session_impl.h"

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
                     const std::shared_ptr<tcp_pubsub::Executor>& callback_keepalive = nullptr,
                     const std::function<void()>& callback_action = {})
      : publisher (executor, "127.0.0.1", 0)
      , subscriber(executor)
    {
      // The callback may hold an additional reference to the executor
      subscriber.setCallback([received_counter, callback_keepalive, callback_action](const tcp_pubsub::CallbackData&)
                             {
                               ++(*received_counter);
                               if (callback_action) callback_action();
                             }, true);
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

TEST(ExecutorShutdown, LastReferenceIsReleasedOnWorkerThread)
{
  for (const size_t thread_count : {1u, 2u})
  {
    SCOPED_TRACE(thread_count);
    struct Destruction
    {
      std::thread::id thread;
      std::atomic<bool> finished{false};
    };
    const auto destruction = std::make_shared<Destruction>();
    const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
    const auto received = std::make_shared<std::atomic<int>>(0);
    const auto entered = std::make_shared<std::promise<std::thread::id>>();
    auto entered_future = entered->get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto executor = std::shared_ptr<tcp_pubsub::Executor>(
        new tcp_pubsub::Executor(thread_count, makeCountingLogger(log_counter)),
        [destruction](tcp_pubsub::Executor* pointer)
        {
          destruction->thread = std::this_thread::get_id();
          delete pointer;
          destruction->finished = true;
        });
    auto traffic = std::make_unique<Traffic>(executor, received, executor,
        [entered, release_future]()
        {
          entered->set_value(std::this_thread::get_id());
          release_future.wait();
        });
    const bool exchanged = traffic->exchangeMessage(received);
    if (!exchanged) release.set_value();
    ASSERT_TRUE(exchanged);
    const auto worker_thread = entered_future.get();

    traffic.reset();
    executor.reset();
    EXPECT_FALSE(destruction->finished.load());
    release.set_value();

    ASSERT_TRUE(WaitUntil([&]() { return destruction->finished.load(); }));
    EXPECT_EQ(destruction->thread, worker_thread);
    EXPECT_TRUE(WaitUntil([&]() { return log_counter.use_count() == 1; }));
  }
}

template<typename Handle>
class ExecutorHandleOwnership : public ::testing::Test
{
  static_assert(!std::is_copy_constructible<Handle>::value, "Publisher/Subscriber must not be copy constructible");
  static_assert(!std::is_copy_assignable<Handle>::value, "Publisher/Subscriber must not be copy assignable");
  static_assert(std::is_nothrow_move_constructible<Handle>::value, "Publisher/Subscriber must be nothrow move constructible");
  static_assert(std::is_nothrow_move_assignable<Handle>::value, "Publisher/Subscriber must be nothrow move assignable");
};

using ExecutorHandleTypes = ::testing::Types<tcp_pubsub::Publisher, tcp_pubsub::Subscriber>;
TYPED_TEST_SUITE(ExecutorHandleOwnership, ExecutorHandleTypes);

TYPED_TEST(ExecutorHandleOwnership, MovingHandleTransfersOwnership)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  std::weak_ptr<tcp_pubsub::Executor> weak;
  {
    auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
    weak = executor;
    auto original = std::make_unique<TypeParam>(executor);
    TypeParam moved(std::move(*original));
    original.reset();
    executor.reset();
    EXPECT_FALSE(weak.expired());
  }
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(log_counter.use_count(), 1);
}

TYPED_TEST(ExecutorHandleOwnership, MoveAssignmentTearsDownOldOperations)
{
  const LogCounter old_logs = std::make_shared<std::atomic<int>>(0);
  const LogCounter new_logs = std::make_shared<std::atomic<int>>(0);
  auto old_executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(old_logs));
  auto new_executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(new_logs));
  const std::weak_ptr<tcp_pubsub::Executor> old_weak = old_executor;
  const std::weak_ptr<tcp_pubsub::Executor> new_weak = new_executor;
  auto target = std::make_unique<TypeParam>(old_executor);
  auto source = std::make_unique<TypeParam>(new_executor);
  old_executor.reset();
  new_executor.reset();

  *target = std::move(*source);
  EXPECT_TRUE(old_weak.expired());
  EXPECT_EQ(old_logs.use_count(), 1);
  source.reset();
  EXPECT_FALSE(new_weak.expired());
  target.reset();
  EXPECT_TRUE(new_weak.expired());
  EXPECT_EQ(new_logs.use_count(), 1);
}

TYPED_TEST(ExecutorHandleOwnership, SelfMoveAssignmentPreservesOwnership)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
  const std::weak_ptr<tcp_pubsub::Executor> weak = executor;
  auto handle = std::make_unique<TypeParam>(executor);
  executor.reset();
  auto& alias = *handle;
  *handle = std::move(alias);
  EXPECT_FALSE(weak.expired());
  handle.reset();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, MovedSubscriberPreservesSessionsAndCallback)
{
  const auto received = std::make_shared<std::atomic<int>>(0);
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
  {
    Traffic traffic(executor, received);
    ASSERT_TRUE(traffic.exchangeMessage(received));
    auto moved = std::make_unique<tcp_pubsub::Subscriber>(std::move(traffic.subscriber));
    ASSERT_EQ(moved->getSessions().size(), 1);
    ASSERT_TRUE(traffic.publisher.send("after construction", 18));
    ASSERT_TRUE(WaitUntil([&]() { return received->load() == 2; }));

    tcp_pubsub::Subscriber assigned(executor);
    assigned = std::move(*moved);
    moved.reset();
    ASSERT_EQ(assigned.getSessions().size(), 1);
    ASSERT_TRUE(traffic.publisher.send("after assignment", 16));
    EXPECT_TRUE(WaitUntil([&]() { return received->load() == 3; }));
  }
  executor.reset();
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, MovedPublisherPreservesConnections)
{
  const auto received = std::make_shared<std::atomic<int>>(0);
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
  {
    Traffic traffic(executor, received);
    ASSERT_TRUE(traffic.exchangeMessage(received));
    const auto port = traffic.publisher.getPort();
    auto moved = std::make_unique<tcp_pubsub::Publisher>(std::move(traffic.publisher));
    EXPECT_EQ(moved->getPort(), port);
    ASSERT_EQ(moved->getSubscriberCount(), 1);
    ASSERT_TRUE(moved->send("after construction", 18));
    ASSERT_TRUE(WaitUntil([&]() { return received->load() == 2; }));

    tcp_pubsub::Publisher assigned(executor, "127.0.0.1", 0);
    assigned = std::move(*moved);
    moved.reset();
    EXPECT_EQ(assigned.getPort(), port);
    ASSERT_EQ(assigned.getSubscriberCount(), 1);
    ASSERT_TRUE(assigned.send("after assignment", 16));
    EXPECT_TRUE(WaitUntil([&]() { return received->load() == 3; }));
  }
  executor.reset();
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, RetainedCancelledSessionKeepsLoggerButNotWorkersAlive)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto received = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<tcp_pubsub::SubscriberSession> session;
  std::weak_ptr<tcp_pubsub::Executor> weak;
  {
    auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
    weak = executor;
    {
      Traffic traffic(executor, received);
      ASSERT_TRUE(traffic.exchangeMessage(received));
      session = traffic.subscriber.getSessions().at(0);
    }
    executor.reset();
  }
  EXPECT_TRUE(weak.expired());
  EXPECT_FALSE(session->isConnected());
  EXPECT_EQ(session->getPublisherList().size(), 1);
  EXPECT_GT(log_counter.use_count(), 1);
  const int calls_after_shutdown = log_counter->load();
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(log_counter->load(), calls_after_shutdown);
  session.reset();
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, PendingWorkLongerThanFiveSecondsIsDrained)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto completed = std::make_shared<std::atomic<bool>>(false);
  const auto failed = std::make_shared<std::atomic<bool>>(false);
  auto executor = std::make_unique<tcp_pubsub::Executor_Impl>(makeCountingLogger(log_counter));
  auto context = executor->ioService();
  auto session = std::make_shared<tcp_pubsub::PublisherSession>(
      context, [](const std::shared_ptr<tcp_pubsub::PublisherSession>&) {}, makeCountingLogger(log_counter));
  auto timer = std::make_shared<asio::steady_timer>(*context, 5500ms);
  timer->async_wait([session, timer, completed, failed](const asio::error_code& ec)
                   {
                     *failed = bool(ec);
                     *completed = true;
                   });
  session.reset();
  timer.reset();
  context.reset();
  executor->start(1);

  const auto before = std::chrono::steady_clock::now();
  executor.reset();
  EXPECT_GE(std::chrono::steady_clock::now() - before, 5s);
  EXPECT_TRUE(completed->load());
  EXPECT_FALSE(failed->load());
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, SubscriberSessionContextOutlivesItsAsioMembers)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  auto context = std::make_shared<asio::io_context>();
  const std::weak_ptr<asio::io_context> weak = context;
  auto session = std::make_shared<tcp_pubsub::SubscriberSession_Impl>(
      context, std::vector<std::pair<std::string, uint16_t>>{{"127.0.0.1", 1}}, 0,
      []() { return std::make_shared<std::vector<char>>(); },
      [](const std::shared_ptr<tcp_pubsub::SubscriberSession_Impl>&) {},
      makeCountingLogger(log_counter));
  context.reset();
  EXPECT_FALSE(weak.expired());
  session.reset();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, RunningCallbackFinishesBeforeShutdownReturns)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto received = std::make_shared<std::atomic<int>>(0);
  const auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  std::promise<void> release;
  const auto release_future = release.get_future().share();
  auto executor = std::make_shared<tcp_pubsub::Executor>(1, makeCountingLogger(log_counter));
  auto traffic = std::make_unique<Traffic>(executor, received, nullptr,
      [entered, release_future]()
      {
        entered->set_value();
        release_future.wait();
      });
  const bool exchanged = traffic->exchangeMessage(received);
  if (!exchanged) release.set_value();
  ASSERT_TRUE(exchanged);
  entered_future.wait();

  std::promise<void> started;
  auto started_future = started.get_future();
  std::promise<void> finished;
  auto finished_future = finished.get_future();
  std::thread shutdown([executor = std::move(executor), traffic = std::move(traffic), &started, &finished]() mutable
                      {
                        started.set_value();
                        traffic.reset();
                        executor.reset();
                        finished.set_value();
                      });
  started_future.wait();
  EXPECT_EQ(finished_future.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  shutdown.join();
  EXPECT_EQ(finished_future.wait_for(0s), std::future_status::ready);
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, AcceptedConnectionCannotEscapeCancellation)
{
  const auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  std::promise<void> release;
  const auto release_future = release.get_future().share();
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  auto executor = std::make_shared<tcp_pubsub::Executor>(1,
      [entered, release_future, log_counter](tcp_pubsub::logger::LogLevel, const std::string& message)
      {
        ++(*log_counter);
        if (message.find(" has connected.") != std::string::npos)
        {
          entered->set_value();
          release_future.wait();
        }
      });
  auto publisher = std::make_unique<tcp_pubsub::Publisher>(executor, "127.0.0.1", 0);
  asio::io_context client_context;
  asio::ip::tcp::socket client(client_context);
  client.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), publisher->getPort()));
  const auto entered_status = entered_future.wait_for(5s);
  if (entered_status != std::future_status::ready) release.set_value();
  ASSERT_EQ(entered_status, std::future_status::ready);

  publisher->cancel();
  release.set_value();
  client.non_blocking(true);
  const bool disconnected = WaitUntil([&]()
                                     {
                                       char data;
                                       asio::error_code ec;
                                       client.read_some(asio::buffer(&data, 1), ec);
                                       return ec == asio::error::eof || ec == asio::error::connection_reset;
                                     });
  client.close();
  EXPECT_TRUE(disconnected);
  EXPECT_EQ(publisher->getSubscriberCount(), 0);
  publisher.reset();
  executor.reset();
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, CancelledPublisherSessionCannotBeRestarted)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  const auto closed = std::make_shared<std::atomic<int>>(0);
  auto context = std::make_shared<asio::io_context>();
  auto session = std::make_shared<tcp_pubsub::PublisherSession>(
      context, [closed](const std::shared_ptr<tcp_pubsub::PublisherSession>&) { ++(*closed); },
      makeCountingLogger(log_counter));
  session->cancel();
  session->start();
  EXPECT_EQ(context->run(), 0);
  EXPECT_EQ(closed->load(), 1);
  session.reset();
  context.reset();
  EXPECT_EQ(log_counter.use_count(), 1);
}

TEST(ExecutorShutdown, ZeroWorkerThreadsAreRejected)
{
  const LogCounter log_counter = std::make_shared<std::atomic<int>>(0);
  EXPECT_THROW(tcp_pubsub::Executor(0, makeCountingLogger(log_counter)), std::invalid_argument);
  EXPECT_EQ(log_counter.use_count(), 1);
}

// Executors are shared via std::shared_ptr, rather than copied or moved.
static_assert(!std::is_copy_constructible<tcp_pubsub::Executor>::value,  "Executor must not be copy constructible");
static_assert(!std::is_copy_assignable<tcp_pubsub::Executor>::value,     "Executor must not be copy assignable");
static_assert(!std::is_move_constructible<tcp_pubsub::Executor>::value,  "Executor must not be move constructible");
static_assert(!std::is_move_assignable<tcp_pubsub::Executor>::value,     "Executor must not be move assignable");
