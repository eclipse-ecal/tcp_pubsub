// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "frame_builder.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace tcp_pubsub::wire
{
  HeaderBytes encode_header(MessageType type, std::uint64_t data_size) noexcept
  {
    HeaderBytes bytes{};
    bytes[0] = static_cast<std::uint8_t>(kHeaderSize);
    bytes[2] = static_cast<std::uint8_t>(type);
    for (unsigned int i = 0; i < 8; ++i)
      bytes[4 + i] = static_cast<std::uint8_t>(data_size >> (8 * i));
    return bytes;
  }

  BuildResult build_frame(MutableBytes output, MessageType type, ConstBytes payload,
                          std::size_t max_payload_size) noexcept
  {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t), "Payload sizes must fit in u64");

    if (output.data() == nullptr || !payload.valid())
      return {0, BuildError::InvalidInput};
    const auto payload_size = payload.size();
    if (payload_size > max_payload_size)
      return {0, BuildError::PayloadTooLarge};
    if (payload_size > (std::numeric_limits<std::size_t>::max)() - kHeaderSize)
      return {0, BuildError::SizeOverflow};

    const auto frame_size = kHeaderSize + payload_size;
    if (output.size() < frame_size)
      return {0, BuildError::InsufficientBuffer};

    if (payload_size != 0)
      std::memmove(output.data() + kHeaderSize, payload.data(), payload_size);
    const auto header = encode_header(type, static_cast<std::uint64_t>(payload_size));
    std::copy(header.begin(), header.end(), output.data());
    return {frame_size, BuildError::None};
  }
}
