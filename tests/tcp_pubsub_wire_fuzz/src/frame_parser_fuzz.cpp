// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "wire/frame_parser.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace
{
  namespace wire = tcp_pubsub::wire;

  void require(bool condition)
  {
    if (!condition)
      std::abort();
  }

  struct Summary
  {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    std::size_t consumed = 0;
    std::size_t headers = 0;
    std::size_t frames = 0;
    wire::ParseError error = wire::ParseError::None;

    void mix(std::uint8_t byte)
    {
      hash = (hash ^ byte) * UINT64_C(1099511628211);
    }

    void mix_header(const wire::Header& header)
    {
      mix(static_cast<std::uint8_t>(header.header_size));
      mix(static_cast<std::uint8_t>(header.header_size >> 8));
      mix(static_cast<std::uint8_t>(header.type));
      mix(header.reserved);
      for (unsigned int i = 0; i < 8; ++i)
        mix(static_cast<std::uint8_t>(header.data_size >> (8 * i)));
    }
  };

  Summary parse(wire::ConstBytes input, std::size_t chunk_size,
                wire::ParserMode mode, std::size_t limit)
  {
    wire::FrameParser parser(limit, mode);
    Summary summary;
    bool failed = false;
    bool in_frame = false;
    wire::Header header;
    std::uint64_t payload_size = 0;
    for (std::size_t offset = 0; offset < input.size() && !failed;)
    {
      auto remaining = input.subspan(offset).first((std::min)(chunk_size, input.size() - offset));
      offset += remaining.size();
      unsigned int zero_progress_events = 0;
      for (;;)
      {
        const auto result = parser.feed(remaining);
        require(result.consumed <= remaining.size());
        summary.consumed += result.consumed;
        require(result.payload.valid());
        if (result.event == wire::ParseEvent::PayloadBytes)
        {
          require(in_frame && !result.discarded && !result.payload.empty());
          require(result.payload.size() <= result.consumed);
          require(result.payload.data() == remaining.data() + result.consumed - result.payload.size());
          require(payload_size <= header.data_size
                  && result.payload.size() <= header.data_size - payload_size);
          payload_size += result.payload.size();
          for (const auto byte : result.payload)
            summary.mix(byte);
        }
        else
        {
          require(result.payload.empty());
        }
        remaining = remaining.subspan(result.consumed);
        if (result.event == wire::ParseEvent::ProtocolError)
        {
          require(result.error != wire::ParseError::None);
          failed = true;
          break;
        }
        require(result.error == wire::ParseError::None);
        if (result.event == wire::ParseEvent::NeedMore)
        {
          require(remaining.empty());
          break;
        }
        if (result.event == wire::ParseEvent::HeaderComplete)
        {
          require(!in_frame);
          in_frame = true;
          header = result.header;
          payload_size = 0;
          ++summary.headers;
          summary.mix_header(header);
          summary.mix(result.discarded ? 1 : 0);
        }
        if (result.event == wire::ParseEvent::FrameComplete)
        {
          require(in_frame);
          require(result.header.header_size == header.header_size
                  && result.header.type == header.type && result.header.reserved == header.reserved
                  && result.header.data_size == header.data_size);
          require(result.discarded || payload_size == header.data_size);
          in_frame = false;
          ++summary.frames;
          summary.mix(0xFF);
        }
        zero_progress_events = result.consumed == 0 ? zero_progress_events + 1 : 0;
        require(zero_progress_events <= 3);
      }
    }
    summary.error = parser.finish();
    if (summary.error == wire::ParseError::None)
    {
      require(!in_frame);
      require(parser.feed({}).event == wire::ParseEvent::NeedMore);
      const std::uint8_t extra[]{0};
      require(parser.feed(extra).error == wire::ParseError::InputAfterEnd);
    }
    else
    {
      const auto latched = parser.feed({});
      require(latched.event == wire::ParseEvent::ProtocolError
              && latched.error == summary.error && latched.consumed == 0);
    }
    return summary;
  }

  void compare(const Summary& left, const Summary& right)
  {
    require(left.hash == right.hash && left.consumed == right.consumed
            && left.headers == right.headers && left.frames == right.frames && left.error == right.error);
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  const wire::ConstBytes input{data, size};
  for (const auto mode : {wire::ParserMode::Receive, wire::ParserMode::Discard})
  {
    for (const auto limit : {std::size_t{0}, std::size_t{4096},
                             (std::numeric_limits<std::size_t>::max)()})
    {
      const auto whole = parse(input, (std::max)(std::size_t{1}, size), mode, limit);
      compare(whole, parse(input, 1, mode, limit));
      compare(whole, parse(input, input.empty() ? 1 : 1 + input[0] % 31, mode, limit));
    }
  }
  return 0;
}
