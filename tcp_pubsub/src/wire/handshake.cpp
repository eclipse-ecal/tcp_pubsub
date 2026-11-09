// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "handshake.h"

#include "frame_builder.h"

#include <algorithm>

namespace tcp_pubsub::wire
{
  HandshakeBytes build_handshake(std::uint8_t version) noexcept
  {
    HandshakeBytes bytes{};
    const auto header = encode_header(MessageType::ProtocolHandshake, 1);
    std::copy(header.begin(), header.end(), bytes.begin());
    bytes[kHeaderSize] = version;
    return bytes;
  }

  HandshakeResult decode_handshake(HandshakeRole role, const Header& header,
                                   const std::uint8_t* prefix, std::size_t prefix_size,
                                   std::uint8_t supported_version) noexcept
  {
    if ((prefix == nullptr && prefix_size != 0) || prefix_size > header.data_size)
      return {0, HandshakeError::InvalidInput};
    if (header.type != MessageType::ProtocolHandshake)
      return {0, HandshakeError::ExpectedHandshake};
    if (role == HandshakeRole::Publisher && header.data_size == 0)
      return {0, HandshakeError::EmptyRequest};
    if (header.data_size != 0 && prefix_size == 0)
      return {0, HandshakeError::MissingVersion};

    const auto version = prefix_size == 0 ? std::uint8_t{0} : prefix[0];
    if (role == HandshakeRole::Publisher)
      return {(std::min)(version, supported_version), HandshakeError::None};
    if (version > supported_version)
      return {0, HandshakeError::UnsupportedVersion};
    return {version, HandshakeError::None};
  }
}
