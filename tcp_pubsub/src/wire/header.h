// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tcp_pubsub::wire
{
  inline constexpr std::size_t kHeaderSize = 12;
  inline constexpr std::size_t kDefaultMaxPayloadSize = 256 * 1024 * 1024;
  inline constexpr std::uint8_t kProtocolVersion = 0;

  enum class MessageType : std::uint8_t
  {
    RegularPayload = 0,
    ProtocolHandshake = 1,
  };

  struct Header
  {
    std::uint16_t header_size = 0;
    MessageType type = MessageType::RegularPayload;
    std::uint8_t reserved = 0;
    std::uint64_t data_size = 0;
  };

  using HeaderBytes = std::array<std::uint8_t, kHeaderSize>;
}
