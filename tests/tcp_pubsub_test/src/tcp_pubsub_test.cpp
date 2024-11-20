// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <chrono>
#include <functional>
#include <memory>

#include <tcp_pubsub/executor.h>
#include <tcp_pubsub/publisher.h>
#include <tcp_pubsub/subscriber.h>

#include "atomic_signalable.h"

#include <gtest/gtest.h>

TEST(tcp_pubsub, basic_test)
{
  atomic_signalable<int> num_messages_received(0);

  // Create executor
  std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(1);

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
    if (hello_world_subscriber.getSessions().at(0)->isConnected())
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Check that the subscriber is connected
  EXPECT_TRUE(hello_world_subscriber.getSessions().at(0)->isConnected());
  
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
