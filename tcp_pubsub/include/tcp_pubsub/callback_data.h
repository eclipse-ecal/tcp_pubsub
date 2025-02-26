// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <memory>
#include <vector>

#include <tcp_pubsub/tcp_pubsub_version.h> // IWYU pragma: keep

namespace tcp_pubsub
{
  struct CallbackData
  {
    // At the moment, this callback data only holds the buffer. But just in case
    // The tcp subsriber would want to return more than that in a later version,
    // we use this struct to improve API stability.

    std::shared_ptr<std::vector<char>> buffer_;
  };
}
