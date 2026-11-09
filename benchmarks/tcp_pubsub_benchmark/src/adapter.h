// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

// Library-independent interface between the benchmark harness and one concrete
// tcp_pubsub implementation. The harness (harness.cpp, main.cpp) only knows
// this header. To benchmark another implementation (e.g. the rewrite), add a
// new adapter_<name>.cpp that implements createContext() and select it in CMake.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bench
{
  struct AdapterOptions
  {
    size_t io_threads       = 2;      // worker threads of the library's execution context (per side)
    bool   inline_delivery  = false;  // true: run the user handler on the I/O side (library's "synchronous" mode)
  };

  // Called with the received message. data is only valid during the call.
  using MessageHandler = std::function<void(const char* data, size_t size)>;

  class IPublisher
  {
  public:
    virtual ~IPublisher() = default;
    virtual uint16_t port() const = 0;
    virtual size_t   subscriberCount() const = 0;
    virtual void     publish(const void* data, size_t size) = 0;
  };

  class ISubscriber
  {
  public:
    virtual ~ISubscriber() = default;
    virtual void connect(const std::string& host, uint16_t port) = 0;
    virtual void setHandler(MessageHandler handler) = 0;
    virtual bool allConnected() const = 0;
  };

  class IContext
  {
  public:
    virtual ~IContext() = default;
    virtual std::unique_ptr<IPublisher>  createPublisher()  = 0;   // listens on 127.0.0.1, OS-chosen port
    virtual std::unique_ptr<ISubscriber> createSubscriber() = 0;
  };

  std::unique_ptr<IContext> createContext(const AdapterOptions& options);
  const char*               adapterName();
}
