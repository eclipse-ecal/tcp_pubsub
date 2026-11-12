// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "wire/frame_builder.h"
#include "wire/frame_parser.h"
#include "wire/handshake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace allocation_tracking
{
  thread_local bool enabled = false;
  thread_local std::size_t count = 0;

  void* allocate(std::size_t size) noexcept
  {
    if (enabled)
      ++count;
    return std::malloc(size == 0 ? 1 : size);
  }

  void* allocate_aligned(std::size_t size, std::size_t alignment) noexcept
  {
    if (enabled)
      ++count;
#if defined(_WIN32)
    return _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    void* memory = nullptr;
    return posix_memalign(&memory, alignment, size == 0 ? 1 : size) == 0 ? memory : nullptr;
#endif
  }

  void free_aligned(void* memory) noexcept
  {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
  }

  class Probe
  {
  public:
    Probe() noexcept
    {
      count = 0;
      enabled = true;
    }

    ~Probe()
    {
      enabled = false;
    }

    std::size_t stop() noexcept
    {
      enabled = false;
      return count;
    }
  };
}

// Keep allocation accounting inside this test executable, not the library.
void* operator new(std::size_t size)
{
  if (auto* memory = allocation_tracking::allocate(size))
    return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
  return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return allocation_tracking::allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
  return ::operator new(size, tag);
}

void operator delete(void* memory) noexcept
{
  std::free(memory);
}

void operator delete[](void* memory) noexcept
{
  ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
  ::operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
  ::operator delete(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
  ::operator delete(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
  ::operator delete(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  if (auto* memory = allocation_tracking::allocate_aligned(size, static_cast<std::size_t>(alignment)))
    return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return allocation_tracking::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
  return ::operator new(size, alignment, tag);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
  allocation_tracking::free_aligned(memory);
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept
{
  ::operator delete(memory, alignment);
}

void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept
{
  ::operator delete(memory, alignment);
}

void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept
{
  ::operator delete(memory, alignment);
}

void operator delete(void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  ::operator delete(memory, alignment);
}

void operator delete[](void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  ::operator delete(memory, alignment);
}

namespace
{
  namespace wire = tcp_pubsub::wire;
  using Bytes = std::vector<std::uint8_t>;

  const Bytes kHandshake{0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const Bytes kHi{0x0C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x69};
  const Bytes kEmpty{0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const Bytes kExtendedHi{0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x48, 0x69};
  const Bytes kUnknown{0x0C, 0x00, 0x07, 0x00, 0x03, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03};
  const Bytes kOversize{0x0C, 0x00, 0x00, 0x00, 0x05, 0x00,
                        0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
  const Bytes kShort{0x04, 0x00, 0x00, 0x00};

  template<class Container>
  void append(Bytes& destination, const Container& source)
  {
    destination.insert(destination.end(), source.begin(), source.end());
  }

  // Independent reference encoding, including deliberately noncanonical headers.
  Bytes raw_header(std::uint16_t size, std::uint8_t type, std::uint64_t data_size,
                   std::uint8_t reserved = 0)
  {
    Bytes bytes((std::max)(std::size_t{2}, static_cast<std::size_t>(size)), 0xA5);
    bytes[0] = static_cast<std::uint8_t>(size % 256);
    bytes[1] = static_cast<std::uint8_t>(size / 256);
    if (size >= 3)
      bytes[2] = type;
    if (size >= 4)
      bytes[3] = reserved;
    for (std::size_t i = 4; i < (std::min)(bytes.size(), wire::kHeaderSize); ++i)
    {
      bytes[i] = static_cast<std::uint8_t>(data_size % 256);
      data_size /= 256;
    }
    return bytes;
  }

  struct ReceivedFrame
  {
    wire::Header header;
    Bytes payload;
  };

  struct Capture
  {
    std::vector<wire::Header> headers;
    std::vector<ReceivedFrame> frames;
    Bytes pending;
    std::size_t discarded = 0;
    wire::ParseError error = wire::ParseError::None;
    wire::ParseError eof_error = wire::ParseError::None;

    void feed(wire::FrameParser& parser, wire::ConstBytes bytes)
    {
      std::size_t offset = 0;
      for (std::size_t step = 0; step < 3 * bytes.size() + 16; ++step)
      {
        const auto result = parser.feed(bytes.subspan(offset));
        ASSERT_LE(result.consumed, bytes.size() - offset);
        offset += result.consumed;
        switch (result.event)
        {
        case wire::ParseEvent::NeedMore:
          EXPECT_EQ(result.error, wire::ParseError::None);
          EXPECT_EQ(offset, bytes.size());
          return;
        case wire::ParseEvent::ProtocolError:
          EXPECT_NE(result.error, wire::ParseError::None);
          error = result.error;
          return;
        case wire::ParseEvent::HeaderComplete:
          headers.push_back(result.header);
          pending.clear();
          break;
        case wire::ParseEvent::PayloadBytes:
          ASSERT_FALSE(result.discarded);
          ASSERT_FALSE(result.payload.empty());
          ASSERT_LE(result.payload.size(), result.consumed);
          ASSERT_EQ(result.payload.data(), bytes.data() + offset - result.payload.size());
          pending.insert(pending.end(), result.payload.begin(), result.payload.end());
          break;
        case wire::ParseEvent::FrameComplete:
          if (result.discarded)
          {
            EXPECT_TRUE(pending.empty());
            ++discarded;
          }
          else
          {
            frames.push_back({result.header, std::move(pending)});
          }
          break;
        }
      }
      FAIL() << "Parser failed to make bounded progress";
    }
  };

  Capture parse(const Bytes& bytes, std::size_t chunk_size = 4096,
                std::size_t limit = wire::kDefaultMaxPayloadSize,
                wire::ParserMode mode = wire::ParserMode::Receive)
  {
    wire::FrameParser parser(limit, mode);
    Capture capture;
    for (std::size_t offset = 0; offset < bytes.size() && capture.error == wire::ParseError::None;)
    {
      const auto count = (std::min)(chunk_size, bytes.size() - offset);
      capture.feed(parser, wire::ConstBytes(bytes).subspan(offset, count));
      offset += count;
    }
    capture.eof_error = parser.finish();
    return capture;
  }

  wire::Header handshake_header(std::uint64_t data_size)
  {
    return {12, wire::MessageType::ProtocolHandshake, 0, data_size};
  }

  TEST(WireBytes, RestrictsConversionsToBorrowedByteStorage)
  {
    static_assert(std::is_trivially_copyable_v<wire::ConstBytes>);
    static_assert(std::is_trivially_copyable_v<wire::MutableBytes>);
    static_assert(std::is_convertible_v<Bytes&, wire::ConstBytes>);
    static_assert(std::is_convertible_v<const Bytes&, wire::ConstBytes>);
    static_assert(std::is_convertible_v<Bytes&, wire::MutableBytes>);
    static_assert(std::is_convertible_v<wire::MutableBytes, wire::ConstBytes>);
    static_assert(!std::is_convertible_v<wire::ConstBytes, wire::MutableBytes>);
    static_assert(!std::is_convertible_v<const Bytes&, wire::MutableBytes>);
    static_assert(!std::is_convertible_v<Bytes&&, wire::ConstBytes>);
    static_assert(!std::is_convertible_v<const Bytes&&, wire::ConstBytes>);
    static_assert(!std::is_convertible_v<Bytes&&, wire::MutableBytes>);
    static_assert(!std::is_convertible_v<std::array<std::uint8_t, 4>&&, wire::ConstBytes>);
    static_assert(!std::is_convertible_v<std::vector<std::uint16_t>&, wire::ConstBytes>);
    static_assert(!std::is_convertible_v<std::vector<wire::ConstBytes>&, wire::ConstBytes>);

    std::uint8_t bytes[]{1, 2, 3};
    wire::MutableBytes output = bytes;
    output[1] = 42;
    const wire::ConstBytes input = output;
    EXPECT_EQ(input.data(), bytes);
    EXPECT_EQ(input.size(), 3u);
    EXPECT_EQ(input[1], 42);

    const std::array<std::uint8_t, 2> array{4, 5};
    EXPECT_EQ(wire::ConstBytes(array)[1], 5);
    Bytes vector{6, 7};
    auto vector_view = wire::MutableBytes(vector);
    vector_view[0] = 8;
    EXPECT_EQ(wire::ConstBytes(vector)[0], 8);
  }

  TEST(WireBytes, PreservesEmptyInvalidAndSubspanSemantics)
  {
    constexpr wire::ConstBytes empty;
    static_assert(empty.valid() && empty.empty() && empty.data() == nullptr);
    static_assert(empty.first(0).empty() && empty.subspan(0).empty());
    static_assert(empty.subspan(0, 0).data() == nullptr);
    static_assert(empty.begin() == empty.end());
    constexpr wire::ConstBytes invalid{nullptr, 1};
    static_assert(!invalid.valid() && !invalid.empty() && invalid.size() == 1);

    std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const wire::ConstBytes view = bytes;
    EXPECT_EQ(view.first(2).size(), 2u);
    EXPECT_EQ(view.first(2).data(), bytes.data());
    EXPECT_EQ(view.subspan(1, 2)[1], 3);
    EXPECT_EQ(view.subspan(2).size(), 2u);
    EXPECT_EQ(view.subspan(4).data(), bytes.data() + bytes.size());
    EXPECT_EQ(view.begin(), bytes.data());
    EXPECT_EQ(view.end(), bytes.data() + bytes.size());
    EXPECT_TRUE(view.subspan(4).empty());
    EXPECT_TRUE(view.subspan(4, 0).empty());
    auto mutable_slice = wire::MutableBytes(bytes).subspan(1).first(1);
    mutable_slice[0] = 9;
    EXPECT_EQ(bytes[1], 9);
  }

  TEST(WireBuilder, EmitsGoldenHandshakeAndData)
  {
    const auto handshake = wire::build_handshake();
    EXPECT_EQ(Bytes(handshake.begin(), handshake.end()), kHandshake);

    std::array<std::uint8_t, 16> output{};
    const std::array<std::uint8_t, 2> payload{0x48, 0x69};
    const auto result = wire::build_frame(output, wire::MessageType::RegularPayload, payload);
    ASSERT_EQ(result.error, wire::BuildError::None);
    ASSERT_EQ(result.size, kHi.size());
    EXPECT_EQ(Bytes(output.begin(), output.begin() + result.size), kHi);
    EXPECT_EQ(output[result.size], 0);
  }

  TEST(WireBuilder, EmitsEmptyFrameAndAcceptsNullEmptyPayload)
  {
    std::array<std::uint8_t, 12> output{};
    const auto result = wire::build_frame(output, wire::MessageType::RegularPayload, {}, 0);
    ASSERT_EQ(result.error, wire::BuildError::None);
    EXPECT_EQ(result.size, kEmpty.size());
    EXPECT_EQ(Bytes(output.begin(), output.end()), kEmpty);
  }

  TEST(WireBuilder, EncodesAllSizeBytesExplicitlyLittleEndian)
  {
    const auto header = wire::encode_header(static_cast<wire::MessageType>(7),
                                            UINT64_C(0x8877665544332211));
    const Bytes expected{0x0C, 0x00, 0x07, 0x00, 0x11, 0x22,
                         0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    EXPECT_EQ(Bytes(header.begin(), header.end()), expected);
  }

  TEST(WireBuilder, RejectsInvalidInputWithoutWriting)
  {
    std::array<std::uint8_t, 16> output{};
    output.fill(0xA5);
    const auto original = output;
    EXPECT_EQ(wire::build_frame({nullptr, 16}, wire::MessageType::RegularPayload, {}).error,
              wire::BuildError::InvalidInput);
    EXPECT_EQ(wire::build_frame({}, wire::MessageType::RegularPayload, {}).error,
              wire::BuildError::InvalidInput);
    const auto result = wire::build_frame(output, wire::MessageType::RegularPayload, {nullptr, 1});
    EXPECT_EQ(result.error, wire::BuildError::InvalidInput);
    EXPECT_EQ(result.size, 0u);
    EXPECT_EQ(output, original);
  }

  TEST(WireBuilder, RejectsLimitOverflowAndInsufficientCapacityBeforeCopy)
  {
    std::array<std::uint8_t, 16> output{};
    output.fill(0xA5);
    const auto original = output;
    const std::uint8_t payload = 42;
    auto result = wire::build_frame(output, wire::MessageType::RegularPayload, {&payload, 2}, 1);
    EXPECT_EQ(result.error, wire::BuildError::PayloadTooLarge);
    EXPECT_EQ(result.size, 0u);
    const auto largest = (std::numeric_limits<std::size_t>::max)();
    result = wire::build_frame({output.data(), largest}, wire::MessageType::RegularPayload,
                              {&payload, largest}, largest);
    EXPECT_EQ(result.error, wire::BuildError::SizeOverflow);
    EXPECT_EQ(result.size, 0u);
    result = wire::build_frame(wire::MutableBytes(output).first(0), wire::MessageType::RegularPayload,
                              {&payload, largest - wire::kHeaderSize}, largest);
    EXPECT_EQ(result.error, wire::BuildError::InsufficientBuffer);
    EXPECT_EQ(result.size, 0u);
    result = wire::build_frame({output.data(), largest}, wire::MessageType::RegularPayload,
                              {&payload, largest - wire::kHeaderSize + 1}, largest);
    EXPECT_EQ(result.error, wire::BuildError::SizeOverflow);
    EXPECT_EQ(result.size, 0u);
    result = wire::build_frame(wire::MutableBytes(output).first(12),
                              wire::MessageType::RegularPayload, {&payload, 1});
    EXPECT_EQ(result.error, wire::BuildError::InsufficientBuffer);
    EXPECT_EQ(result.size, 0u);
    EXPECT_EQ(output, original);
  }

  TEST(WireBuilder, SupportsOverlappingPayloadAndOutput)
  {
    for (const std::size_t source_offset : {0u, 4u, 12u, 13u})
    {
      SCOPED_TRACE(source_offset);
      std::array<std::uint8_t, 18> output{};
      output[source_offset] = 0x48;
      output[source_offset + 1] = 0x69;
      const auto result = wire::build_frame(output, wire::MessageType::RegularPayload,
                                           wire::ConstBytes(output).subspan(source_offset, 2));
      ASSERT_EQ(result.error, wire::BuildError::None);
      EXPECT_EQ(Bytes(output.begin(), output.begin() + result.size), kHi);
    }
  }

  TEST(WireBuilder, AcceptsSixteenMiBExactlyAtTheLimit)
  {
    constexpr std::size_t size = 16 * 1024 * 1024;
    Bytes payload(size, 0x5A);
    Bytes frame(wire::kHeaderSize + size + 1, 0xA5);
    const auto result = wire::build_frame(frame, wire::MessageType::RegularPayload, payload, size);
    ASSERT_EQ(result.error, wire::BuildError::None);
    EXPECT_EQ(result.size, wire::kHeaderSize + size);
    EXPECT_EQ(frame[6], 0);
    EXPECT_EQ(frame[7], 1);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), frame.begin() + wire::kHeaderSize));
    EXPECT_EQ(frame.back(), 0xA5);
  }

  TEST(WireParser, ParsesGoldenVectorsByteByByte)
  {
    for (const auto& bytes : {kHandshake, kHi, kEmpty, kExtendedHi, kUnknown, kShort})
    {
      const auto result = parse(bytes, 1);
      EXPECT_EQ(result.error, wire::ParseError::None);
      EXPECT_EQ(result.eof_error, wire::ParseError::None);
      ASSERT_EQ(result.headers.size(), 1u);
      if (bytes == kUnknown || bytes == kShort)
      {
        EXPECT_TRUE(result.frames.empty());
        EXPECT_EQ(result.discarded, 1u);
      }
      else
      {
        ASSERT_EQ(result.frames.size(), 1u);
        const Bytes expected = bytes == kHandshake ? Bytes{0} : bytes == kEmpty ? Bytes{} : Bytes{0x48, 0x69};
        EXPECT_EQ(result.frames[0].payload, expected);
      }
    }
  }

  TEST(WireParser, HandlesEverySplitAndCoalescedMixedFrames)
  {
    Bytes stream;
    for (const auto& bytes : {kHandshake, kEmpty, kUnknown, kExtendedHi, kShort, kHi, kEmpty})
      append(stream, bytes);
    for (std::size_t split = 0; split <= stream.size(); ++split)
    {
      SCOPED_TRACE(split);
      wire::FrameParser parser;
      Capture capture;
      capture.feed(parser, wire::ConstBytes(stream).first(split));
      capture.feed(parser, wire::ConstBytes(stream).subspan(split));
      EXPECT_EQ(capture.error, wire::ParseError::None);
      EXPECT_EQ(parser.finish(), wire::ParseError::None);
      ASSERT_EQ(capture.frames.size(), 5u);
      EXPECT_EQ(capture.frames[0].payload, Bytes{0});
      EXPECT_EQ(capture.frames[1].payload, Bytes{});
      EXPECT_EQ(capture.frames[2].payload, (Bytes{0x48, 0x69}));
      EXPECT_EQ(capture.frames[3].payload, (Bytes{0x48, 0x69}));
      EXPECT_EQ(capture.frames[4].payload, Bytes{});
      EXPECT_EQ(capture.discarded, 2u);
    }
  }

  TEST(WireParser, AcceptsExtendedHeadersThroughUint16Maximum)
  {
    for (const auto size : std::array<std::uint16_t, 4>{13, 16, 256, 65535})
    {
      SCOPED_TRACE(size);
      auto bytes = raw_header(size, 0, 2);
      append(bytes, Bytes{0x48, 0x69});
      append(bytes, kHi);
      const auto result = parse(bytes, 7);
      EXPECT_EQ(result.error, wire::ParseError::None);
      EXPECT_EQ(result.eof_error, wire::ParseError::None);
      ASSERT_EQ(result.frames.size(), 2u);
      EXPECT_EQ(result.frames[0].header.header_size, size);
      EXPECT_EQ(result.frames[0].payload, (Bytes{0x48, 0x69}));
      EXPECT_EQ(result.frames[1].payload, (Bytes{0x48, 0x69}));
    }
  }

  TEST(WireParser, NeverDeliversShortRegularHeadersEvenWithNonzeroSizeBytes)
  {
    for (std::uint16_t size = 2; size < 12; ++size)
    {
      SCOPED_TRACE(size);
      auto bytes = raw_header(size, 0, UINT64_MAX);
      append(bytes, kEmpty);
      append(bytes, kHi);
      const auto result = parse(bytes, 1, 2);
      EXPECT_EQ(result.error, wire::ParseError::None);
      EXPECT_EQ(result.eof_error, wire::ParseError::None);
      EXPECT_EQ(result.discarded, 1u);
      ASSERT_EQ(result.frames.size(), 2u);
      EXPECT_TRUE(result.frames[0].payload.empty());
      EXPECT_EQ(result.frames[1].payload, (Bytes{0x48, 0x69}));
      ASSERT_EQ(result.headers.size(), 3u);
      EXPECT_EQ(result.headers[0].header_size, size);
      EXPECT_EQ(result.headers[0].type, wire::MessageType::RegularPayload);
      const auto expected_size = size <= 4 ? UINT64_C(0) : (UINT64_C(1) << (8 * (size - 4))) - 1;
      EXPECT_EQ(result.headers[0].data_size, expected_size);
    }
  }

  TEST(WireParser, ZeroFillsMissingHandshakeAndUnknownHeaderBytes)
  {
    auto bytes = raw_header(3, 1, 0);
    append(bytes, raw_header(5, 1, 1));
    append(bytes, Bytes{0});
    append(bytes, raw_header(5, 7, 3));
    append(bytes, Bytes{1, 2, 3});
    append(bytes, kHi);
    const auto result = parse(bytes, 1);
    EXPECT_EQ(result.error, wire::ParseError::None);
    EXPECT_EQ(result.eof_error, wire::ParseError::None);
    ASSERT_EQ(result.frames.size(), 3u);
    EXPECT_EQ(result.frames[0].header.reserved, 0);
    EXPECT_EQ(result.frames[0].header.data_size, 0u);
    EXPECT_TRUE(result.frames[0].payload.empty());
    EXPECT_EQ(result.frames[1].header.data_size, 1u);
    EXPECT_EQ(result.frames[1].payload, Bytes{0});
    EXPECT_EQ(result.discarded, 1u);
    EXPECT_EQ(result.frames[2].payload, (Bytes{0x48, 0x69}));
  }

  TEST(WireParser, IgnoresReservedByte)
  {
    for (unsigned int reserved = 0; reserved <= 255; ++reserved)
    {
      auto bytes = kHi;
      bytes[3] = static_cast<std::uint8_t>(reserved);
      const auto result = parse(bytes, 3);
      ASSERT_EQ(result.frames.size(), 1u);
      EXPECT_EQ(result.frames[0].payload, (Bytes{0x48, 0x69}));
      EXPECT_EQ(result.frames[0].header.reserved, reserved);
    }
  }

  TEST(WireParser, SkipsEveryUnknownTypeIncludingEmptyFrames)
  {
    for (unsigned int type = 2; type <= 255; ++type)
    {
      SCOPED_TRACE(type);
      auto bytes = raw_header(12, static_cast<std::uint8_t>(type), 3);
      append(bytes, Bytes{1, 2, 3});
      append(bytes, raw_header(12, static_cast<std::uint8_t>(type), 0));
      append(bytes, kEmpty);
      append(bytes, kHi);
      const auto result = parse(bytes, 2);
      EXPECT_EQ(result.error, wire::ParseError::None);
      EXPECT_EQ(result.eof_error, wire::ParseError::None);
      EXPECT_EQ(result.discarded, 2u);
      ASSERT_EQ(result.frames.size(), 2u);
      EXPECT_TRUE(result.frames[0].payload.empty());
      EXPECT_EQ(result.frames[1].payload, (Bytes{0x48, 0x69}));
    }
  }

  TEST(WireParser, RejectsZeroAndOneByteHeaderSizesAfterLittleEndianDecode)
  {
    for (const auto size : std::array<std::uint16_t, 2>{0, 1})
    {
      const auto result = parse(raw_header(size, 0, 0), 1);
      EXPECT_EQ(result.error, wire::ParseError::InvalidHeaderSize);
      EXPECT_EQ(result.eof_error, wire::ParseError::InvalidHeaderSize);
      EXPECT_TRUE(result.headers.empty());
      EXPECT_TRUE(result.frames.empty());
    }
    const auto result = parse(raw_header(256, 0, 0), 1);
    EXPECT_EQ(result.error, wire::ParseError::None);
    ASSERT_EQ(result.frames.size(), 1u);
    EXPECT_EQ(result.frames[0].header.header_size, 256);
  }

  TEST(WireParser, RejectsOversizeBeforeHeaderEventOrPayloadRead)
  {
    for (const auto& bytes : {kOversize, raw_header(12, 0, UINT64_MAX),
                             raw_header(12, 1, UINT64_MAX), raw_header(12, 7, UINT64_MAX)})
    {
      wire::FrameParser parser;
      const auto result = parser.feed(bytes);
      EXPECT_EQ(result.event, wire::ParseEvent::ProtocolError);
      EXPECT_EQ(result.error, wire::ParseError::PayloadTooLarge);
      EXPECT_EQ(result.consumed, 12u);
      EXPECT_EQ(result.payload.data(), nullptr);
      EXPECT_TRUE(result.payload.empty());
    }
    const auto header = raw_header(16, 0, 3);
    wire::FrameParser parser(2);
    const auto result = parser.feed(header);
    EXPECT_EQ(result.error, wire::ParseError::PayloadTooLarge);
    EXPECT_EQ(result.consumed, 12u);
  }

  TEST(WireParser, HonorsZeroExactDefaultAndSizeTBoundaryLimits)
  {
    const auto empty = parse(kEmpty, 1, 0);
    ASSERT_EQ(empty.frames.size(), 1u);
    EXPECT_TRUE(empty.frames[0].payload.empty());
    EXPECT_EQ(parse(kHi, 1, 0).error, wire::ParseError::PayloadTooLarge);
    EXPECT_EQ(parse(kHi, 1, 1).error, wire::ParseError::PayloadTooLarge);
    EXPECT_EQ(parse(kHi, 1, 2).error, wire::ParseError::None);

    for (const auto size : {static_cast<std::uint64_t>(wire::kDefaultMaxPayloadSize),
                            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())})
    {
      const auto header = raw_header(12, 0, size);
      wire::FrameParser parser(static_cast<std::size_t>(size));
      const auto result = parser.feed(header);
      EXPECT_EQ(result.event, wire::ParseEvent::HeaderComplete);
      EXPECT_EQ(result.header.data_size, size);
      EXPECT_EQ(parser.finish(), wire::ParseError::UnexpectedEof);
    }
    const auto over_default = raw_header(12, 0, wire::kDefaultMaxPayloadSize + UINT64_C(1));
    EXPECT_EQ(parse(over_default).error, wire::ParseError::PayloadTooLarge);
    const auto platform_limit = (std::numeric_limits<std::size_t>::max)();
    EXPECT_EQ(parse(raw_header(12, 0, static_cast<std::uint64_t>(platform_limit)),
                    4096, platform_limit - 1).error, wire::ParseError::PayloadTooLarge);
  }

  TEST(WireParser, NeverDeliversIncompleteFramesAtEof)
  {
    for (const auto& complete : {kHi, kHandshake, kExtendedHi, kUnknown})
    {
      for (std::size_t size = 0; size < complete.size(); ++size)
      {
        SCOPED_TRACE(size);
        const auto result = parse(Bytes(complete.begin(), complete.begin() + size), 1);
        EXPECT_EQ(result.error, wire::ParseError::None);
        EXPECT_EQ(result.eof_error, size == 0 ? wire::ParseError::None : wire::ParseError::UnexpectedEof);
        EXPECT_TRUE(result.frames.empty());
        EXPECT_EQ(result.discarded, 0u);
      }
    }
    auto bytes = kHi;
    append(bytes, Bytes(kHi.begin(), kHi.end() - 1));
    const auto result = parse(bytes);
    EXPECT_EQ(result.eof_error, wire::ParseError::UnexpectedEof);
    ASSERT_EQ(result.frames.size(), 1u);
    EXPECT_EQ(result.frames[0].payload, (Bytes{0x48, 0x69}));
  }

  TEST(WireParser, LatchesErrorsAndRejectsNullNonemptyInput)
  {
    wire::FrameParser parser;
    EXPECT_EQ(parser.feed({}).event, wire::ParseEvent::NeedMore);
    const auto invalid = parser.feed({nullptr, 1});
    EXPECT_EQ(invalid.error, wire::ParseError::InvalidInput);
    const auto after = parser.feed(kHi);
    EXPECT_EQ(after.event, wire::ParseEvent::ProtocolError);
    EXPECT_EQ(after.error, wire::ParseError::InvalidInput);
    EXPECT_EQ(after.consumed, 0u);
    EXPECT_EQ(parser.finish(), wire::ParseError::InvalidInput);

    wire::FrameParser truncated;
    EXPECT_EQ(truncated.feed(wire::ConstBytes(kHi).first(1)).event, wire::ParseEvent::NeedMore);
    EXPECT_EQ(truncated.finish(), wire::ParseError::UnexpectedEof);
    EXPECT_EQ(truncated.feed(wire::ConstBytes(kHi).subspan(1)).error, wire::ParseError::UnexpectedEof);
  }

  TEST(WireParser, CanDrainCompleteFrameAfterFinishAndRejectsFurtherBytes)
  {
    for (const auto& bytes : {kEmpty, kHi})
    {
      wire::FrameParser parser;
      const auto header = parser.feed(bytes);
      EXPECT_EQ(header.event, wire::ParseEvent::HeaderComplete);
      ASSERT_EQ(header.consumed, wire::kHeaderSize);
      if (bytes.size() > header.consumed)
      {
        const auto payload = parser.feed(wire::ConstBytes(bytes).subspan(header.consumed));
        EXPECT_EQ(payload.event, wire::ParseEvent::PayloadBytes);
        EXPECT_EQ(payload.consumed, bytes.size() - header.consumed);
      }
      EXPECT_EQ(parser.finish(), wire::ParseError::None);
      const auto complete = parser.feed({});
      EXPECT_EQ(complete.event, wire::ParseEvent::FrameComplete);
      EXPECT_FALSE(complete.discarded);
      EXPECT_EQ(complete.header.data_size, bytes.size() - wire::kHeaderSize);
      EXPECT_EQ(parser.feed({}).event, wire::ParseEvent::NeedMore);
      EXPECT_EQ(parser.finish(), wire::ParseError::None);
      EXPECT_EQ(parser.feed(kHi).error, wire::ParseError::InputAfterEnd);
    }
  }

  TEST(WireParser, PublisherStreamingDiscardsAllTypesAndIgnoresPayloadLimit)
  {
    Bytes bytes;
    for (const auto& frame : {kHandshake, kEmpty, kHi, kExtendedHi, kUnknown, kShort})
      append(bytes, frame);
    const auto result = parse(bytes, 1, 0, wire::ParserMode::Discard);
    EXPECT_EQ(result.error, wire::ParseError::None);
    EXPECT_EQ(result.eof_error, wire::ParseError::None);
    EXPECT_TRUE(result.frames.empty());
    EXPECT_EQ(result.discarded, 6u);
    EXPECT_EQ(parse(raw_header(1, 0, 0), 1, 0, wire::ParserMode::Discard).error,
              wire::ParseError::InvalidHeaderSize);
  }

  TEST(WireParser, PublisherStreamingCanSkipFullUint64PayloadLengths)
  {
    const auto bytes = raw_header(12, 0, UINT64_MAX);
    wire::FrameParser parser(0, wire::ParserMode::Discard);
    const auto header = parser.feed(bytes);
    EXPECT_EQ(header.event, wire::ParseEvent::HeaderComplete);
    EXPECT_TRUE(header.discarded);
    EXPECT_EQ(header.header.data_size, UINT64_MAX);
    std::array<std::uint8_t, 32> chunk{};
    for (int i = 0; i < 64; ++i)
    {
      const auto result = parser.feed(chunk);
      EXPECT_EQ(result.event, wire::ParseEvent::NeedMore);
      EXPECT_EQ(result.consumed, chunk.size());
      EXPECT_TRUE(result.payload.empty());
    }
    EXPECT_EQ(parser.finish(), wire::ParseError::UnexpectedEof);
  }

  TEST(WireHandshake, DecodesGoldenRequestAndResponse)
  {
    const auto capture = parse(kHandshake, 1);
    ASSERT_EQ(capture.frames.size(), 1u);
    const auto& frame = capture.frames[0];
    for (const auto role : {wire::HandshakeRole::Publisher, wire::HandshakeRole::Subscriber})
    {
      const auto result = wire::decode_handshake(role, frame.header, frame.payload);
      EXPECT_EQ(result.error, wire::HandshakeError::None);
      EXPECT_EQ(result.version, 0);
    }
  }

  TEST(WireHandshake, PublisherNegotiatesMinimumRequestedAndSupportedVersions)
  {
    for (unsigned int requested = 0; requested <= 255; ++requested)
    {
      const auto version = static_cast<std::uint8_t>(requested);
      const auto result = wire::decode_handshake(wire::HandshakeRole::Publisher,
                                                 handshake_header(1), {&version, 1});
      EXPECT_EQ(result.error, wire::HandshakeError::None);
      EXPECT_EQ(result.version, 0);
      const auto future = wire::decode_handshake(wire::HandshakeRole::Publisher,
                                                 handshake_header(1), {&version, 1}, 5);
      EXPECT_EQ(future.error, wire::HandshakeError::None);
      EXPECT_EQ(future.version, (std::min)(requested, 5u));
      EXPECT_EQ(wire::build_handshake(version).back(), version);
    }
  }

  TEST(WireHandshake, SubscriberRejectsUnsupportedResponseVersion)
  {
    for (unsigned int offered = 0; offered <= 255; ++offered)
    {
      const auto version = static_cast<std::uint8_t>(offered);
      const auto result = wire::decode_handshake(wire::HandshakeRole::Subscriber,
                                                 handshake_header(1), {&version, 1});
      EXPECT_EQ(result.error, offered == 0 ? wire::HandshakeError::None
                                          : wire::HandshakeError::UnsupportedVersion);
      const auto future = wire::decode_handshake(wire::HandshakeRole::Subscriber,
                                                 handshake_header(1), {&version, 1}, 5);
      EXPECT_EQ(future.error, offered <= 5 ? wire::HandshakeError::None
                                          : wire::HandshakeError::UnsupportedVersion);
      if (offered <= 5)
      {
        EXPECT_EQ(future.version, offered);
      }
    }
  }

  TEST(WireHandshake, RejectsNonHandshakeAndEmptyRequests)
  {
    const std::uint8_t version = 0;
    for (unsigned int type = 0; type <= 255; ++type)
    {
      if (type == 1)
        continue;
      auto header = handshake_header(1);
      header.type = static_cast<wire::MessageType>(type);
      EXPECT_EQ(wire::decode_handshake(wire::HandshakeRole::Publisher, header, {&version, 1}).error,
                wire::HandshakeError::ExpectedHandshake);
    }
    EXPECT_EQ(wire::decode_handshake(wire::HandshakeRole::Publisher, handshake_header(0), {}).error,
              wire::HandshakeError::EmptyRequest);
    const auto response = wire::decode_handshake(wire::HandshakeRole::Subscriber,
                                                  handshake_header(0), {});
    EXPECT_EQ(response.error, wire::HandshakeError::None);
    EXPECT_EQ(response.version, 0);
  }

  TEST(WireHandshake, IgnoresExtensionsWithoutRetainingTheWholePayload)
  {
    const std::array<std::uint8_t, 4> payload{0, 0xFF, 0xFF, 0xFF};
    for (const auto role : {wire::HandshakeRole::Publisher, wire::HandshakeRole::Subscriber})
    {
      const auto full = wire::decode_handshake(role, handshake_header(4), payload);
      EXPECT_EQ(full.error, wire::HandshakeError::None);
      EXPECT_EQ(full.version, 0);
      const auto prefix = wire::decode_handshake(role, handshake_header(UINT64_MAX), wire::ConstBytes(payload).first(1));
      EXPECT_EQ(prefix.error, wire::HandshakeError::None);
      EXPECT_EQ(prefix.version, 0);
    }
  }

  TEST(WireHandshake, ReportsInvalidAndMissingPayloadPrefixes)
  {
    const std::uint8_t version = 0;
    for (const auto role : {wire::HandshakeRole::Publisher, wire::HandshakeRole::Subscriber})
    {
      EXPECT_EQ(wire::decode_handshake(role, handshake_header(1), {nullptr, 1}).error,
                wire::HandshakeError::InvalidInput);
      EXPECT_EQ(wire::decode_handshake(role, handshake_header(0), {&version, 1}).error,
                wire::HandshakeError::InvalidInput);
      EXPECT_EQ(wire::decode_handshake(role, handshake_header(1), {}).error,
                wire::HandshakeError::MissingVersion);
    }
  }

  TEST(WireProperties, RandomValidStreamsMatchIndependentReferenceFrames)
  {
    constexpr std::uint32_t seed = 0xC0DEC0DE;
    std::mt19937 random(seed);
    for (int iteration = 0; iteration < 200; ++iteration)
    {
      SCOPED_TRACE(iteration);
      Bytes stream;
      std::vector<ReceivedFrame> expected;
      std::size_t expected_discarded = 0;
      for (int frame = 0; frame < 20; ++frame)
      {
        const auto kind = random() % 6;
        const auto type = kind == 4 ? std::uint8_t{1}
                         : kind == 5 ? static_cast<std::uint8_t>(2 + random() % 254) : std::uint8_t{0};
        const auto reserved = static_cast<std::uint8_t>(random());
        const auto header_size = kind == 2 ? static_cast<std::uint16_t>(2 + random() % 10)
                                 : kind == 3 ? static_cast<std::uint16_t>(13 + random() % 64)
                                 : kind == 4 ? std::uint16_t{5} : std::uint16_t{12};
        const auto payload_size = kind == 1 || kind == 2 ? 0u : random() % 96;
        Bytes payload(payload_size);
        for (auto& byte : payload)
          byte = static_cast<std::uint8_t>(random());
        const auto advertised_size = kind == 2 ? (static_cast<std::uint64_t>(random()) << 32) | random()
                                              : static_cast<std::uint64_t>(payload_size);
        append(stream, raw_header(header_size, type, advertised_size, reserved));
        append(stream, payload);
        if (kind == 2 || kind == 5)
          ++expected_discarded;
        else
          expected.push_back({{header_size, static_cast<wire::MessageType>(type), reserved, payload_size},
                              std::move(payload)});
      }

      wire::FrameParser parser(96);
      Capture capture;
      for (std::size_t offset = 0; offset < stream.size();)
      {
        const auto size = (std::min)(static_cast<std::size_t>(1 + random() % 37), stream.size() - offset);
        capture.feed(parser, wire::ConstBytes(stream).subspan(offset, size));
        offset += size;
      }
      ASSERT_EQ(capture.error, wire::ParseError::None);
      EXPECT_EQ(parser.finish(), wire::ParseError::None);
      EXPECT_EQ(capture.discarded, expected_discarded);
      ASSERT_EQ(capture.frames.size(), expected.size());
      for (std::size_t i = 0; i < expected.size(); ++i)
      {
        EXPECT_EQ(capture.frames[i].header.header_size, expected[i].header.header_size);
        EXPECT_EQ(capture.frames[i].header.type, expected[i].header.type);
        EXPECT_EQ(capture.frames[i].header.reserved, expected[i].header.reserved);
        EXPECT_EQ(capture.frames[i].header.data_size, expected[i].header.data_size);
        EXPECT_EQ(capture.frames[i].payload, expected[i].payload);
      }
    }
  }

  TEST(WireProperties, AllocationProbeCountsNewAndNewArray)
  {
    std::size_t allocations = 0;
    {
      allocation_tracking::Probe probe;
      auto* single = ::operator new(16);
      auto* array = ::operator new[](16);
      ::operator delete(single);
      ::operator delete[](array);
      allocations = probe.stop();
    }
    EXPECT_EQ(allocations, 2u);
  }

  TEST(WireProperties, AllocationProbeCountsAllAlignedForms)
  {
    constexpr auto alignment = std::align_val_t{64};
    std::size_t allocations = 0;
    bool valid = true;
    {
      allocation_tracking::Probe probe;
      for (const std::size_t size : {0u, 65u})
      {
        auto* single = ::operator new(size, alignment);
        auto* array = ::operator new[](size, alignment);
        auto* sized = ::operator new(size, alignment);
        auto* sized_array = ::operator new[](size, alignment);
        auto* nothrow = ::operator new(size, alignment, std::nothrow);
        auto* nothrow_array = ::operator new[](size, alignment, std::nothrow);
        for (auto* memory : {single, array, sized, sized_array, nothrow, nothrow_array})
          valid = valid && memory != nullptr && reinterpret_cast<std::uintptr_t>(memory) % 64 == 0;
        ::operator delete(single, alignment);
        ::operator delete[](array, alignment);
        ::operator delete(sized, size, alignment);
        ::operator delete[](sized_array, size, alignment);
        ::operator delete(nothrow, alignment, std::nothrow);
        ::operator delete[](nothrow_array, alignment, std::nothrow);
      }
      allocations = probe.stop();
    }
    EXPECT_TRUE(valid);
    EXPECT_EQ(allocations, 12u);
  }

  TEST(WireProperties, FuzzHarnessSmoke)
  {
    EXPECT_EQ(LLVMFuzzerTestOneInput(nullptr, 0), 0);
    for (const auto& bytes : {kHandshake, kHi, kEmpty, kExtendedHi, kUnknown, kOversize, kShort,
                             raw_header(0, 0, 0), raw_header(1, 0, 0), raw_header(65535, 0, 0),
                             raw_header(12, 0, UINT64_MAX)})
    {
      EXPECT_EQ(LLVMFuzzerTestOneInput(bytes.data(), bytes.size()), 0);
      EXPECT_EQ(LLVMFuzzerTestOneInput(bytes.data(), bytes.size() - 1), 0);
    }
    Bytes coalesced;
    for (const auto& frame : {kHandshake, kEmpty, kHi, kShort, kUnknown, kExtendedHi})
      append(coalesced, frame);
    EXPECT_EQ(LLVMFuzzerTestOneInput(coalesced.data(), coalesced.size()), 0);

    std::mt19937 random(0xF022);
    std::array<std::uint8_t, 256> bytes{};
    for (int iteration = 0; iteration < 256; ++iteration)
    {
      for (auto& byte : bytes)
        byte = static_cast<std::uint8_t>(random());
      EXPECT_EQ(LLVMFuzzerTestOneInput(bytes.data(), random() % (bytes.size() + 1)), 0);
    }
  }

  TEST(WireProperties, RandomGarbageMakesBoundedProgressWithoutAllocations)
  {
    constexpr std::uint32_t seed = 0xBAD5EED;
    std::mt19937 random(seed);
    std::array<std::uint8_t, 256> bytes{};
    bool valid = true;
    std::size_t allocations = 0;
    {
      allocation_tracking::Probe probe;
      for (int iteration = 0; iteration < 4096 && valid; ++iteration)
      {
        for (auto& byte : bytes)
          byte = static_cast<std::uint8_t>(random());
        if (iteration % 4 == 0)
        {
          bytes[0] = 12;
          bytes[1] = 0;
          bytes[2] = static_cast<std::uint8_t>(iteration % 3);
          std::fill(bytes.begin() + 4, bytes.begin() + 12, 0);
          bytes[4] = static_cast<std::uint8_t>(random() % 128);
        }
        const auto mode = iteration % 2 == 0 ? wire::ParserMode::Receive : wire::ParserMode::Discard;
        wire::FrameParser parser(128, mode);
        bool failed = false;
        for (std::size_t offset = 0; offset < bytes.size() && !failed && valid;)
        {
          const auto chunk = (std::min)(static_cast<std::size_t>(1 + random() % 31), bytes.size() - offset);
          std::size_t consumed = 0;
          bool stopped = false;
          for (std::size_t step = 0; step < 3 * chunk + 16; ++step)
          {
            const auto result = parser.feed(wire::ConstBytes(bytes).subspan(offset + consumed, chunk - consumed));
            if (result.consumed > chunk - consumed)
            {
              valid = false;
              break;
            }
            consumed += result.consumed;
            if (result.event == wire::ParseEvent::PayloadBytes)
              valid = valid && !result.discarded && !result.payload.empty()
                      && result.payload.size() <= result.consumed
                      && result.payload.data() == bytes.data() + offset + consumed - result.payload.size();
            if (result.event == wire::ParseEvent::ProtocolError)
            {
              valid = valid && result.error != wire::ParseError::None;
              failed = true;
              stopped = true;
              break;
            }
            if (result.event == wire::ParseEvent::NeedMore)
            {
              valid = valid && consumed == chunk;
              stopped = true;
              break;
            }
          }
          valid = valid && stopped;
          offset += chunk;
        }
        const auto end = parser.finish();
        valid = valid && (end == wire::ParseError::None || end == wire::ParseError::UnexpectedEof
                          || end == wire::ParseError::InvalidHeaderSize || end == wire::ParseError::PayloadTooLarge);
      }
      allocations = probe.stop();
    }
    EXPECT_TRUE(valid) << "seed = " << seed;
    EXPECT_EQ(allocations, 0u);
  }

  TEST(WireProperties, BuildingAndExtendedHeaderSkippingAllocateNothing)
  {
    std::array<std::uint8_t, 64> output{};
    const std::array<std::uint8_t, 2> payload{0x48, 0x69};
    std::array<std::uint8_t, 12> header{0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::array<std::uint8_t, 256> scratch{};
    bool valid = true;
    std::size_t allocations = 0;
    {
      allocation_tracking::Probe probe;
      const auto built = wire::build_frame(output, wire::MessageType::RegularPayload, payload);
      valid = built.error == wire::BuildError::None && built.size == 14;
      const auto handshake = wire::build_handshake();
      const auto decoded = wire::decode_handshake(wire::HandshakeRole::Publisher, handshake_header(1),
                                                   wire::ConstBytes(handshake).subspan(wire::kHeaderSize));
      valid = valid && decoded.error == wire::HandshakeError::None && decoded.version == 0;
      wire::FrameParser parser;
      const auto first = parser.feed(header);
      valid = valid && first.event == wire::ParseEvent::NeedMore;
      std::size_t remaining = 65535 - wire::kHeaderSize;
      while (remaining != 0)
      {
        const auto size = (std::min)(remaining, scratch.size());
        const auto result = parser.feed(wire::ConstBytes(scratch).first(size));
        remaining -= size;
        valid = valid && result.consumed == size
                && result.event == (remaining == 0 ? wire::ParseEvent::HeaderComplete : wire::ParseEvent::NeedMore);
      }
      valid = valid && parser.feed({}).event == wire::ParseEvent::FrameComplete;
      valid = valid && parser.finish() == wire::ParseError::None;
      allocations = probe.stop();
    }
    EXPECT_TRUE(valid);
    EXPECT_EQ(allocations, 0u);
  }
}
