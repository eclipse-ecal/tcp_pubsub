// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>

#include "atomic_signalable.h"

#include <gtest/gtest.h>

// Basic test that sends two messages from a publisher to a subscriber
TEST(tcp_pubsub, basic_test)
{
  atomic_signalable<int> num_messages_received(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1, tcp_pubsub::logger::logger_no_verbose_debug);

  // Create publisher
  tcp_pubsub::Publisher hello_world_publisher(executor, 1588);

  // Create subscriber
  tcp_pubsub::Subscriber hello_world_subscriber(executor);

  // Subscribe to localhost on port 1588
  hello_world_subscriber.addSession("127.0.0.1", 1588);

  std::string received_message;

  // Create a callback that will be called when a message is received
  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function =
            [&received_message, &num_messages_received](const tcp_pubsub::CallbackData& callback_data)
            {
              received_message = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
              ++num_messages_received;
            };

  // Register the callback
  hello_world_subscriber.setCallback(callback_function);

  // Wait up to 1 second for the subscriber to connect
  for (int i = 0; i < 10; ++i)
  {
    if (hello_world_subscriber.getSessions().at(0)->isConnected()
      && hello_world_publisher.getSubscriberCount() >= 1)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(0)->isConnected());
  EXPECT_EQ(hello_world_publisher.getSubscriberCount(), 1);
  
  // Publish "Hello World 1"
  {
    const std::string message = "Hello World 1";
    hello_world_publisher.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, "Hello World 1");
  EXPECT_EQ(num_messages_received.get(), 1);

  // Publish "Hello World 2"
  {
    const std::string message = "Hello World 2";
    hello_world_publisher.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 1; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, "Hello World 2");
  EXPECT_EQ(num_messages_received.get(), 2);
}

// Test that sends a very large message from a publisher to a subscriber
TEST(tcp_pubsub, large_message_test)
{
  constexpr size_t message_size = 1024 * 1024 * 16;

  atomic_signalable<int> num_messages_received(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1, tcp_pubsub::logger::logger_no_verbose_debug);

  // Create publisher
  tcp_pubsub::Publisher hello_world_publisher(executor, 1588);

  // Create subscriber
  tcp_pubsub::Subscriber hello_world_subscriber(executor);

  // Subscribe to localhost on port 1588
  hello_world_subscriber.addSession("127.0.0.1", 1588);

  std::string received_message;

  // Create a callback that will be called when a message is received
  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function =
    [&received_message, &num_messages_received](const tcp_pubsub::CallbackData& callback_data)
    {
      received_message = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
      ++num_messages_received;
    };

  // Register the callback
  hello_world_subscriber.setCallback(callback_function);

  // Wait up to 1 second for the subscriber to connect
  for (int i = 0; i < 10; ++i)
  {
    if (hello_world_subscriber.getSessions().at(0)->isConnected()
      && hello_world_publisher.getSubscriberCount() >= 1)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(0)->isConnected());
  EXPECT_EQ(hello_world_publisher.getSubscriberCount(), 1);

  // Create a large message consisting of random bytes
  std::string message;
  message.resize(message_size);
  for (size_t i = 0; i < message_size; ++i)
  {
    message[i] = static_cast<char>(rand() % 256);
  }

  // Publish the large message
  hello_world_publisher.send(message.data(), message.size());

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, message);
  EXPECT_EQ(num_messages_received.get(), 1);
}

// Test that sends messages from 2 publishers to a single subscriber
TEST(tcp_pubsub, multiple_publishers_test)
{
  atomic_signalable<int> num_messages_received(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1, tcp_pubsub::logger::logger_no_verbose_debug);

  // Create publisher 1
  tcp_pubsub::Publisher hello_world_publisher1(executor, 1588);

  // Create publisher 2
  tcp_pubsub::Publisher hello_world_publisher2(executor, 1589);

  // Create subscriber
  tcp_pubsub::Subscriber hello_world_subscriber(executor);

  // Subscribe to localhost on port 1588
  hello_world_subscriber.addSession("127.0.0.1", 1588);
  hello_world_subscriber.addSession("127.0.0.1", 1589);

  std::string received_message;

  // Create a callback that will be called when a message is received
  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function =
    [&received_message, &num_messages_received](const tcp_pubsub::CallbackData& callback_data)
    {
      received_message = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
      ++num_messages_received;
    };

  // Register the callback
  hello_world_subscriber.setCallback(callback_function);

  // Wait up to 1 second for the subscriber to connect
  for (int i = 0; i < 10; ++i)
  {
    if (hello_world_subscriber.getSessions().at(0)->isConnected()
      && hello_world_subscriber.getSessions().at(1)->isConnected()
      && hello_world_publisher1.getSubscriberCount() >= 1
      && hello_world_publisher2.getSubscriberCount() >= 1)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(0)->isConnected());
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(1)->isConnected());
  EXPECT_EQ(hello_world_publisher1.getSubscriberCount(), 1);
  EXPECT_EQ(hello_world_publisher2.getSubscriberCount(), 1);

  // Publish "Hello World 1"
  {
    const std::string message = "Hello World 1";
    hello_world_publisher1.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, "Hello World 1");
  EXPECT_EQ(num_messages_received.get(), 1);

  // Publish "Hello World 2"
  {
    const std::string message = "Hello World 2";
    hello_world_publisher2.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 1; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, "Hello World 2");
  EXPECT_EQ(num_messages_received.get(), 2);
}

// Test that sends messages from a single publisher to 2 subscribers
TEST(tcp_pubsub, multiple_subscribers_test)
{
  atomic_signalable<int> num_messages_received1(0);
  atomic_signalable<int> num_messages_received2(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1, tcp_pubsub::logger::logger_no_verbose_debug);

  // Create publisher
  tcp_pubsub::Publisher hello_world_publisher(executor, 1588);

  // Create subscriber 1
  tcp_pubsub::Subscriber hello_world_subscriber1(executor);

  // Create subscriber 2
  tcp_pubsub::Subscriber hello_world_subscriber2(executor);

  // Subscribe to localhost on port 1588
  hello_world_subscriber1.addSession("127.0.0.1", 1588);
  hello_world_subscriber2.addSession("127.0.0.1", 1588);

  std::string received_message1;
  std::string received_message2;

  // Create a callback that will be called when a message is received
  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function1 =
    [&received_message1, &num_messages_received1](const tcp_pubsub::CallbackData& callback_data)
    {
      received_message1 = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
      ++num_messages_received1;
    };

  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function2 =
    [&received_message2, &num_messages_received2](const tcp_pubsub::CallbackData& callback_data)
    {
      received_message2 = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
      ++num_messages_received2;
    };

  // Register the callback
  hello_world_subscriber1.setCallback(callback_function1);
  hello_world_subscriber2.setCallback(callback_function2);

  // Wait up to 1 second for the subscriber to connect
  for (int i = 0; i < 10; ++i)
  {
    if (hello_world_subscriber1.getSessions().at(0)->isConnected()
      && hello_world_subscriber2.getSessions().at(0)->isConnected()
      && hello_world_publisher.getSubscriberCount() >= 2)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber1.getSessions().at(0)->isConnected());
  EXPECT_TRUE(hello_world_subscriber2.getSessions().at(0)->isConnected());
  EXPECT_EQ(hello_world_publisher.getSubscriberCount(), 2);

  // Publish "Hello World 1"
  {
    const std::string message = "Hello World 1";
    hello_world_publisher.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received1.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));
  num_messages_received2.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message1, "Hello World 1");
  EXPECT_EQ(num_messages_received1.get(), 1);
  EXPECT_EQ(received_message2, "Hello World 1");
  EXPECT_EQ(num_messages_received2.get(), 1);
}

// Test connecting to a list of possible publishers. The subscriber will connect to the first available publisher.
TEST(tcp_pubsub, publisher_list_test)
{
  atomic_signalable<int> num_messages_received(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1, tcp_pubsub::logger::logger_no_verbose_debug);

  // Create publisher
  tcp_pubsub::Publisher hello_world_publisher(executor, 1588);

  // Create subscriber
  tcp_pubsub::Subscriber hello_world_subscriber(executor);

  // Subscribe to localhost on port 1588
  std::vector<std::pair<std::string, uint16_t>> publishers = { { "NonExistentPublisher", 1800 }, { "localhost", 1588 } };
  hello_world_subscriber.addSession(publishers);

  std::string received_message;

  // Create a callback that will be called when a message is received
  std::function<void(const tcp_pubsub::CallbackData& callback_data)> callback_function =
            [&received_message, &num_messages_received](const tcp_pubsub::CallbackData& callback_data)
            {
              received_message = std::string(callback_data.buffer_->data(), callback_data.buffer_->size());
              ++num_messages_received;
            };

  // Register the callback
  hello_world_subscriber.setCallback(callback_function);

  // Wait up to 1 second for the subscriber to connect
  for (int i = 0; i < 10; ++i)
  {
    if (hello_world_subscriber.getSessions().at(0)->isConnected()
      && hello_world_publisher.getSubscriberCount() >= 1)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(0)->isConnected());
  EXPECT_EQ(hello_world_publisher.getSubscriberCount(), 1);

  // Check the address and port hat the subscriber connected to
  EXPECT_EQ(hello_world_subscriber.getSessions().at(0)->getConnectedPublisher().first,  "localhost");
  EXPECT_EQ(hello_world_subscriber.getSessions().at(0)->getConnectedPublisher().second, 1588);

  // Publish "Hello World 1"
  {
    const std::string message = "Hello World 1";
    hello_world_publisher.send(message.data(), message.size());
  }

  // wait for message to be received
  num_messages_received.wait_for([](int value) { return value > 0; }, std::chrono::seconds(1));

  // Check that the message was received
  EXPECT_EQ(received_message, "Hello World 1");
  EXPECT_EQ(num_messages_received.get(), 1);
}
