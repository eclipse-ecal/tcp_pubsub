// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include <iostream>
#include <thread>

#include <tcp_pubsub/publisher.h>

int main()
{
  {
    std::shared_ptr<tcp_pubsub::Executor> executor = std::make_shared<tcp_pubsub::Executor>(6);
    tcp_pubsub::Publisher hello_world_publisher(executor, 1588);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  return 0;
}
