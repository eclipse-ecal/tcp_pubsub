// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "bytes.h"
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

  [[nodiscard]] HeaderBytes encode_header(MessageType type, std::uint64_t data_size) noexcept;

  // The caller owns the output storage; payload and output may overlap.
  // On error no output bytes are modified.
  [[nodiscard]] BuildResult build_frame(MutableBytes output, MessageType type, ConstBytes payload,
                                       std::size_t max_payload_size = kDefaultMaxPayloadSize) noexcept;
}
