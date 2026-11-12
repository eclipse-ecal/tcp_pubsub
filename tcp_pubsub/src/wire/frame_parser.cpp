// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "frame_parser.h"

#include <algorithm>

namespace tcp_pubsub::wire
{
  FrameParser::FrameParser(std::size_t max_payload_size, ParserMode mode) noexcept
    : max_payload_size_(max_payload_size), mode_(mode)
  {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t), "Payload sizes must fit in u64");
  }

  ParseResult FrameParser::feed(ConstBytes bytes) noexcept
  {
    if (error_ != ParseError::None)
      return make_result(ParseEvent::ProtocolError, 0);
    if (!bytes.valid())
      return fail(ParseError::InvalidInput, 0);
    if (finished_ && !bytes.empty())
      return fail(ParseError::InputAfterEnd, 0);

    const auto size = bytes.size();
    std::size_t consumed = 0;
    for (;;)
    {
      switch (state_)
      {
      case State::Header:
      {
        while (header_bytes_read_ < 2 && consumed < size)
          header_bytes_[header_bytes_read_++] = bytes[consumed++];
        if (header_bytes_read_ < 2)
          return make_result(ParseEvent::NeedMore, consumed);

        header_.header_size = static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(header_bytes_[0])
          | (static_cast<std::uint16_t>(header_bytes_[1]) << 8));
        if (header_.header_size < 2)
          return fail(ParseError::InvalidHeaderSize, consumed);

        const auto known_size = (std::min)(static_cast<std::size_t>(header_.header_size), kHeaderSize);
        while (header_bytes_read_ < known_size && consumed < size)
          header_bytes_[header_bytes_read_++] = bytes[consumed++];
        if (header_bytes_read_ < known_size)
          return make_result(ParseEvent::NeedMore, consumed);

        header_.type = static_cast<MessageType>(header_bytes_[2]);
        header_.reserved = header_bytes_[3];
        header_.data_size = 0;
        for (unsigned int i = 0; i < 8; ++i)
          header_.data_size |= static_cast<std::uint64_t>(header_bytes_[4 + i]) << (8 * i);

        const bool short_payload_header = header_.type == MessageType::RegularPayload
                                          && header_.header_size < kHeaderSize;
        discarded_ = mode_ == ParserMode::Discard || short_payload_header
                     || (header_.type != MessageType::RegularPayload
                         && header_.type != MessageType::ProtocolHandshake);
        // WP-F2: partial data_size bytes in a short RegularPayload header do not
        // advertise a payload, even when their zero-filled value is nonzero.
        payload_remaining_ = short_payload_header ? 0 : header_.data_size;
        if (mode_ == ParserMode::Receive && payload_remaining_ > max_payload_size_)
          return fail(ParseError::PayloadTooLarge, consumed);

        extension_remaining_ = header_.header_size > kHeaderSize
                               ? header_.header_size - kHeaderSize : 0;
        state_ = State::HeaderExtension;
        break;
      }

      case State::HeaderExtension:
      {
        const auto count = (std::min)(extension_remaining_, size - consumed);
        extension_remaining_ -= count;
        consumed += count;
        if (extension_remaining_ != 0)
          return make_result(ParseEvent::NeedMore, consumed);
        state_ = State::Payload;
        return make_result(ParseEvent::HeaderComplete, consumed);
      }

      case State::Payload:
      {
        if (payload_remaining_ == 0)
        {
          state_ = State::FrameComplete;
          break;
        }
        const auto count = static_cast<std::size_t>((std::min)(
          payload_remaining_, static_cast<std::uint64_t>(size - consumed)));
        if (count == 0)
          return make_result(ParseEvent::NeedMore, consumed);

        const auto payload = bytes.subspan(consumed, count);
        consumed += count;
        payload_remaining_ -= count;
        if (!discarded_)
        {
          auto result = make_result(ParseEvent::PayloadBytes, consumed);
          result.payload = payload;
          return result;
        }
        break;
      }

      case State::FrameComplete:
      {
        auto result = make_result(ParseEvent::FrameComplete, consumed);
        reset_frame();
        return result;
      }
      }
    }
  }

  ParseError FrameParser::finish() noexcept
  {
    if (error_ != ParseError::None)
      return error_;
    finished_ = true;
    if ((state_ == State::Header && header_bytes_read_ == 0)
        || (state_ == State::Payload && payload_remaining_ == 0)
        || state_ == State::FrameComplete)
      return ParseError::None;
    error_ = ParseError::UnexpectedEof;
    return error_;
  }

  ParseResult FrameParser::make_result(ParseEvent event, std::size_t consumed) const noexcept
  {
    return {event, consumed, header_, {}, discarded_, error_};
  }

  ParseResult FrameParser::fail(ParseError error, std::size_t consumed) noexcept
  {
    error_ = error;
    return make_result(ParseEvent::ProtocolError, consumed);
  }

  void FrameParser::reset_frame() noexcept
  {
    state_ = State::Header;
    header_bytes_.fill(0);
    header_bytes_read_ = 0;
    extension_remaining_ = 0;
    payload_remaining_ = 0;
    header_ = {};
    discarded_ = false;
  }
}
