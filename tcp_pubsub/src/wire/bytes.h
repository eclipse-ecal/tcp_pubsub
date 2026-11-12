// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tcp_pubsub::wire
{
  // Non-owning views: storage must outlive the view and any borrowed parser result.
  // Null/nonempty views are invalid and are rejected by the codec, not normalized.
  template<class Byte>
  class BasicBytes
  {
    static_assert(std::is_same_v<Byte, std::uint8_t>
                  || std::is_same_v<Byte, const std::uint8_t>);

  public:
    constexpr BasicBytes() noexcept = default;
    constexpr BasicBytes(Byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template<std::size_t N>
    constexpr BasicBytes(Byte (&bytes)[N]) noexcept : data_(bytes), size_(N) {}

    template<class Container,
             std::enable_if_t<std::is_lvalue_reference_v<Container&&>
                              && std::is_convertible_v<decltype(std::declval<Container&>().data()), Byte*>
                              && std::is_convertible_v<decltype(std::declval<Container&>().size()),
                                                       std::size_t>, int> = 0>
    constexpr BasicBytes(Container&& bytes) noexcept : data_(bytes.data()), size_(bytes.size()) {}

    template<class Other, std::enable_if_t<std::is_convertible_v<Other*, Byte*>, int> = 0>
    constexpr BasicBytes(BasicBytes<Other> bytes) noexcept : data_(bytes.data()), size_(bytes.size()) {}

    [[nodiscard]] constexpr Byte* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool valid() const noexcept { return data_ != nullptr || empty(); }

    [[nodiscard]] constexpr Byte* begin() const noexcept
    {
      assert(valid());
      return data_;
    }

    [[nodiscard]] constexpr Byte* end() const noexcept
    {
      assert(valid());
      return size_ == 0 ? data_ : data_ + size_;
    }

    // Indexing and slicing require a valid view and in-range indices, as for std::span.
    [[nodiscard]] constexpr Byte& operator[](std::size_t index) const noexcept
    {
      assert(valid() && index < size_);
      return data_[index];
    }

    [[nodiscard]] constexpr BasicBytes first(std::size_t count) const noexcept
    {
      assert(valid() && count <= size_);
      return {data_, count};
    }

    [[nodiscard]] constexpr BasicBytes subspan(std::size_t offset) const noexcept
    {
      assert(valid() && offset <= size_);
      return {offset == 0 ? data_ : data_ + offset, size_ - offset};
    }

    [[nodiscard]] constexpr BasicBytes subspan(std::size_t offset, std::size_t count) const noexcept
    {
      assert(valid() && offset <= size_ && count <= size_ - offset);
      return {offset == 0 ? data_ : data_ + offset, count};
    }

  private:
    Byte* data_ = nullptr;
    std::size_t size_ = 0;
  };

  using ConstBytes = BasicBytes<const std::uint8_t>;
  using MutableBytes = BasicBytes<std::uint8_t>;
}
