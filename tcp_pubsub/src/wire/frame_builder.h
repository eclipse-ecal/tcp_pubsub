// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "header.h"

namespace tcp_pubsub::wire
{
  enum class BuildError
  {
    None,
    InvalidInput,
    PayloadTooLarge,
    SizeOverflow,
    InsufficientBuffer,
  };

  struct BuildResult
  {
    std::size_t size = 0;
    BuildError error = BuildError::None;
  };

  HeaderBytes encode_header(MessageType type, std::uint64_t data_size) noexcept;

  // The caller owns the output storage; payload and output may overlap.
  // On error no output bytes are modified.
  [[nodiscard]] BuildResult build_frame(std::uint8_t* output, std::size_t capacity,
                                       MessageType type, const std::uint8_t* payload,
                                       std::size_t payload_size,
                                       std::size_t max_payload_size = kDefaultMaxPayloadSize) noexcept;
}
