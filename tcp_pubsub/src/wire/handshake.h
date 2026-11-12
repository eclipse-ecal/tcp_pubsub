// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "bytes.h"
#include "header.h"

namespace tcp_pubsub::wire
{
  using HandshakeBytes = std::array<std::uint8_t, kHeaderSize + 1>;

  enum class HandshakeRole
  {
    Publisher,
    Subscriber,
  };

  enum class HandshakeError
  {
    None,
    InvalidInput,
    ExpectedHandshake,
    EmptyRequest,
    MissingVersion,
    UnsupportedVersion,
  };

  struct HandshakeResult
  {
    std::uint8_t version = 0;
    HandshakeError error = HandshakeError::None;
  };

  [[nodiscard]] HandshakeBytes build_handshake(std::uint8_t version = kProtocolVersion) noexcept;

  // Invoke after FrameComplete. Retain only the first payload byte; the parser
  // consumes extensions, so prefix need not contain the whole handshake payload.
  [[nodiscard]] HandshakeResult decode_handshake(
    HandshakeRole role, const Header& header, ConstBytes prefix,
    std::uint8_t supported_version = kProtocolVersion) noexcept;
}
