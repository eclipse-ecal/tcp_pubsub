// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "header.h"

namespace tcp_pubsub::wire
{
  enum class ParserMode
  {
    Receive,
    // WP-R1/R2: publisher streaming reads discard even oversized frames.
    Discard,
  };

  enum class ParseEvent
  {
    NeedMore,
    HeaderComplete,
    PayloadBytes,
    FrameComplete,
    ProtocolError,
  };

  enum class ParseError
  {
    None,
    InvalidInput,
    InvalidHeaderSize,
    PayloadTooLarge,
    UnexpectedEof,
    InputAfterEnd,
  };

  struct ParseResult
  {
    ParseEvent event = ParseEvent::NeedMore;
    std::size_t consumed = 0;
    Header header;
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    bool discarded = false;
    ParseError error = ParseError::None;
  };

  class FrameParser
  {
  public:
    explicit FrameParser(std::size_t max_payload_size = kDefaultMaxPayloadSize,
                         ParserMode mode = ParserMode::Receive) noexcept;

    // One event per call. Advance by consumed, then feed the unconsumed suffix,
    // including an empty suffix, until NeedMore or ProtocolError is returned.
    // PayloadBytes borrows the input; discarded frames never emit PayloadBytes.
    [[nodiscard]] ParseResult feed(const std::uint8_t* bytes, std::size_t size) noexcept;
    [[nodiscard]] ParseError finish() noexcept;

  private:
    enum class State
    {
      Header,
      HeaderExtension,
      Payload,
      FrameComplete,
    };

    ParseResult make_result(ParseEvent event, std::size_t consumed) const noexcept;
    ParseResult fail(ParseError error, std::size_t consumed) noexcept;
    void reset_frame() noexcept;

    std::size_t max_payload_size_;
    ParserMode mode_;
    State state_ = State::Header;
    HeaderBytes header_bytes_{};
    std::size_t header_bytes_read_ = 0;
    std::size_t extension_remaining_ = 0;
    std::uint64_t payload_remaining_ = 0;
    Header header_;
    bool discarded_ = false;
    bool finished_ = false;
    ParseError error_ = ParseError::None;
  };
}
