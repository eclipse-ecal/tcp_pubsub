// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#include "subscriber_session_impl.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "portable_endian.h"
#include "protocol_handshake_message.h"
#include "tcp_header.h"
#include "tcp_pubsub/tcp_pubsub_logger.h"
#include "tcp_pubsub_logger_abstraction.h"

namespace tcp_pubsub
{
  //////////////////////////////////////////////
  /// Constructor & Destructor
  //////////////////////////////////////////////
  
  SubscriberSession_Impl::SubscriberSession_Impl(const std::shared_ptr<asio::io_context>&                             io_context
                                                , const std::vector<std::pair<std::string, uint16_t>>&                publisher_list
                                                , int                                                                 max_reconnection_attempts
                                                , const std::function<std::shared_ptr<std::vector<char>>()>&          get_buffer_handler
                                                , const std::function<void(const std::shared_ptr<SubscriberSession_Impl>&)>& session_closed_handler
                                                , const tcp_pubsub::logger::logger_t&                                      log_function)
    : publisher_list_         (publisher_list)
    , resolver_               (*io_context)
    , max_reconnection_attempts_(max_reconnection_attempts)
    , retries_left_           (max_reconnection_attempts)
    , retry_timer_            (*io_context, std::chrono::seconds(1))
    , canceled_               (false)
    , data_socket_            (*io_context)
    , data_strand_            (*io_context)
    , get_buffer_handler_     (get_buffer_handler)
    , session_closed_handler_ (session_closed_handler)
    , log_                    (log_function)
  {
    // Throw an exception if the publisher list is empty
    if (publisher_list_.empty())
    {
      throw std::invalid_argument("SubscriberSession_Impl: Publisher list is empty.");
    }
  }

  // Destructor
  SubscriberSession_Impl::~SubscriberSession_Impl()
  {
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
    std::stringstream ss;
    ss << std::this_thread::get_id();
    const std::string thread_id = ss.str();
    log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Deleting from thread " + thread_id + "...");
#endif

    cancel();

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "SubscriberSession " + endpointToString() + ": Deleted.");
#endif
  }

  //////////////////////////////////////////////
  /// Connect to publisher
  //////////////////////////////////////////////
  
  void SubscriberSession_Impl::start()
  {
    if (canceled_) return;

    // Start resolving the endpoint given in the constructor
    resolveEndpoint(0);
  }

  void SubscriberSession_Impl::resolveEndpoint(size_t publisher_list_index)
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

    resolver_.async_resolve(publisher_list_[publisher_list_index].first
                            , std::to_string(publisher_list_[publisher_list_index].second)
                            , [me = shared_from_this(), publisher_list_index](asio::error_code ec, const asio::ip::tcp::resolver::results_type& resolved_endpoints)
                              {
                                if (ec)
                                {
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
                                  {
                                    const std::string message = "Failed resolving endpoint [" + me->publisher_list_[publisher_list_index].first + ":" + std::to_string(me->publisher_list_[publisher_list_index].second) + "]: " + ec.message();
                                    me->log_(logger::LogLevel::Debug, "SubscriberSession " + me->endpointToString() + ": " + message);
                                  }
#endif // 
                                  if (publisher_list_index + 1 < me->publisher_list_.size())
                                  {
                                    // Try next possible endpoint
                                    me->resolveEndpoint(publisher_list_index + 1);
                                  }
                                  else
                                  {
                                    // Log warning
                                    std::string message = "Failed resolving any endpoint: ";
                                    for (size_t i = 0; i < me->publisher_list_.size(); i++)
                                    {
                                      message += me->publisher_list_[i].first + ":" + std::to_string(me->publisher_list_[i].second);
                                      if (i + 1 < me->publisher_list_.size())
                                        message += ", ";
                                    }
                                    me->log_(logger::LogLevel::Warning, "SubscriberSession " + me->endpointToString() + ": " + message);

                                    // Execute connection Failed handler
                                    me->connectionFailedHandler();
                                    return;
                                  }
                                }
                                else
                                {
                                  me->connectToEndpoint(resolved_endpoints, publisher_list_index);
                                }
                              });
  }

  void SubscriberSession_Impl::connectToEndpoint(const asio::ip::tcp::resolver::results_type& resolved_endpoints, size_t publisher_list_index)
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

    // Convert the resolved_endpoints iterator to an endpoint sequence
    // (i.e. a vector of endpoints)
    auto endpoint_sequence = std::make_shared<std::vector<asio::ip::tcp::endpoint>>();
    for (const auto& endpoint : resolved_endpoints)
    {
      endpoint_sequence->push_back(endpoint);
    }

    asio::async_connect(data_socket_
                        , *endpoint_sequence
                        , [me = shared_from_this(), publisher_list_index](asio::error_code ec, const asio::ip::tcp::endpoint& /*endpoint*/)
                          {
                            if (ec)
                            {
                              me->log_(logger::LogLevel::Warning, "SubscriberSession " + me->localEndpointToString() + ": Failed connecting to publisher " + me->publisher_list_[publisher_list_index].first + ":" + std::to_string(me->publisher_list_[publisher_list_index].second) + ": " + ec.message());
                              me->connectionFailedHandler();
                              return;
                            }
                            else
                            {
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
                              me->log_(logger::LogLevel::Debug, "SubscriberSession " + me->endpointToString() + ": Successfully connected to publisher " + me->publisher_list_[publisher_list_index].first + ":" + std::to_string(me->publisher_list_[publisher_list_index].second));
#endif

#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
                              me->log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + me->endpointToString() + ": Setting tcp::no_delay option.");
#endif
                              // Disable Nagle's algorithm. Nagles Algorithm will otherwise cause the
                              // Socket to wait for more data, if it encounters a frame that can still
                              // fit more data. Obviously, this is an awfull default behaviour, if we
                              // want to transmit our data in a timely fashion.
                              {
                                asio::error_code nodelay_ec;
                                me->data_socket_.set_option(asio::ip::tcp::no_delay(true), nodelay_ec);
                                if (nodelay_ec) me->log_(logger::LogLevel::Warning, "SubscriberSession " + me->endpointToString() + ": Failed setting tcp::no_delay option. The performance may suffer.");
                              }

                              // Store the connected publisher endpoint
                              {
                                const std::lock_guard<std::mutex> lock(me->connected_publisher_endpoint_mutex_);
                                me->connected_publisher_endpoint_ = me->publisher_list_[publisher_list_index];
                              }

                              // Start reading a package by reading the header length. Everything will
                              // unfold from there automatically.
                              me->sendProtokolHandshakeRequest();
                            }
                          });
  }

  void SubscriberSession_Impl::sendProtokolHandshakeRequest()
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug,  "SubscriberSession " + endpointToString() + ": Sending ProtocolHandshakeRequest.");
#endif

    const std::shared_ptr<std::vector<char>> buffer = std::make_shared<std::vector<char>>();
    buffer->resize(sizeof(TcpHeader) + sizeof(ProtocolHandshakeMessage));

    TcpHeader* header   = reinterpret_cast<TcpHeader*>(buffer->data());
    header->header_size = htole16(sizeof(TcpHeader));
    header->type        = MessageContentType::ProtocolHandshake;
    header->reserved    = 0;
    header->data_size   = htole64(sizeof(ProtocolHandshakeMessage));

    ProtocolHandshakeMessage* handshake_message = reinterpret_cast<ProtocolHandshakeMessage*>(&(buffer->operator[](sizeof(TcpHeader))));
    handshake_message->protocol_version         = 0; // At the moment, we only support Version 0. 

    asio::async_write(data_socket_
                , asio::buffer(*buffer)
                , asio::bind_executor(data_strand_,
                  [me = shared_from_this(), buffer](asio::error_code ec, std::size_t /*bytes_to_transfer*/)
                  {
                    if (ec)
                    {
                      me->log_(logger::LogLevel::Warning, "SubscriberSession " + me->endpointToString() + ": Failed sending ProtocolHandshakeRequest: " + ec.message());
                      me->connectionFailedHandler();
                      return;
                    }
                    me->readHeaderLength();
                  }));
  }


  void SubscriberSession_Impl::connectionFailedHandler()
  {
    // Reset the connected publisher endpoint
    {
      const std::lock_guard<std::mutex> lock(connected_publisher_endpoint_mutex_);
      connected_publisher_endpoint_ = std::make_pair("", 0);
    }

    {
      asio::error_code ec;
      data_socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    }

    {
      asio::error_code ec;
      data_socket_.close(ec); // Even if ec indicates an error, the socket is closed now (according to the documentation)
    }

    if (!canceled_ && (retries_left_ < 0 || retries_left_ > 0))
    {
      // Decrement the number of retries we have left
      if (retries_left_ > 0)
        retries_left_--;

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
      log_(logger::LogLevel::Debug, "SubscriberSession " + endpointToString() + ": Waiting and retrying to connect");
#endif

      // Retry connection after a short time
      retry_timer_.async_wait([me = shared_from_this()](asio::error_code ec)
                              {
                                if (ec)
                                {
                                  me->log_(logger::LogLevel::Warning, "SubscriberSession " + me->endpointToString() + ": Waiting to reconnect failed: " + ec.message());
                                  me->session_closed_handler_(me);
                                  return;
                                }
                                me->resolveEndpoint(0);
                              });
    }
    else
    {
      session_closed_handler_(shared_from_this());
    }
  }
  
  /////////////////////////////////////////////
  // Data receiving
  /////////////////////////////////////////////
  
  void SubscriberSession_Impl::readHeaderLength()
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
    log_(logger::LogLevel::DebugVerbose,  "SubscriberSession " + endpointToString() + ": Waiting for data...");
#endif

    const std::shared_ptr<TcpHeader> header = std::make_shared<TcpHeader>();

    asio::async_read(data_socket_
                     , asio::buffer(&(header->header_size), sizeof(header->header_size))
                     , asio::transfer_at_least(sizeof(header->header_size))
                     , asio::bind_executor(data_strand_, [me = shared_from_this(), header](asio::error_code ec, std::size_t /*length*/)
                                        {
                                          if (ec)
                                          {
                                            me->log_(logger::LogLevel::Error,  "SubscriberSession " + me->endpointToString() + ": Error reading header length: " + ec.message());
                                            me->connectionFailedHandler();;
                                            return;
                                          }
                                          me->readHeaderContent(header);
                                        }));
  }

  void SubscriberSession_Impl::readHeaderContent(const std::shared_ptr<TcpHeader>& header)
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

    if (header->header_size < sizeof(header->header_size))
    {
      log_(logger::LogLevel::Error,  "SubscriberSession " + endpointToString() + ": Received header length of " + std::to_string(header->header_size) + ", which is less than the minimal header size.");
      connectionFailedHandler();
      return;
    }

    const uint16_t remote_header_size = le16toh(header->header_size);
    const uint16_t my_header_size     = sizeof(*header);

    const uint16_t bytes_to_read_from_socket    = std::min(remote_header_size, my_header_size) - sizeof(header->header_size);
    const uint16_t bytes_to_discard_from_socket = (remote_header_size > my_header_size ? (remote_header_size - my_header_size) : 0);

    asio::async_read(data_socket_
                    , asio::buffer(&reinterpret_cast<char*>(header.get())[sizeof(header->header_size)], bytes_to_read_from_socket)
                    , asio::transfer_at_least(bytes_to_read_from_socket)
                    , asio::bind_executor(data_strand_, [me = shared_from_this(), header, bytes_to_discard_from_socket](asio::error_code ec, std::size_t /*length*/)
                                        {
                                          if (ec)
                                          {
                                            me->log_(logger::LogLevel::Error,  "SubscriberSession " + me->endpointToString() + ": Error reading header content: " + ec.message());
                                            me->connectionFailedHandler();;
                                            return;
                                          }
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
                                          me->log_(logger::LogLevel::DebugVerbose,  "SubscriberSession " + me->endpointToString()
                                            + ": Received header content: "
                                            + "data_size: "       + std::to_string(le64toh(header->data_size)));
#endif

                                          if (bytes_to_discard_from_socket > 0)
                                          {
                                            me->discardDataBetweenHeaderAndPayload(header, bytes_to_discard_from_socket);
                                          }
                                          else
                                          {
                                            me->readPayload(header);
                                          }
                                        }));
  }

  void SubscriberSession_Impl::discardDataBetweenHeaderAndPayload(const std::shared_ptr<TcpHeader>& header, uint16_t bytes_to_discard)
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

    std::vector<char> data_to_discard;
    data_to_discard.resize(bytes_to_discard);

#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
    log_(logger::LogLevel::DebugVerbose,  "SubscriberSession " + endpointToString() + ": Discarding " + std::to_string(bytes_to_discard) + " bytes after the header.");
#endif

    asio::async_read(data_socket_
                    , asio::buffer(data_to_discard.data(), bytes_to_discard)
                    , asio::transfer_at_least(bytes_to_discard)
                    , asio::bind_executor(data_strand_, [me = shared_from_this(), header](asio::error_code ec, std::size_t /*length*/)
                                        {
                                          if (ec)
                                          {
                                            me->log_(logger::LogLevel::Error,  "SubscriberSession " + me->endpointToString() + ": Error discarding bytes after header: " + ec.message());
                                            me->connectionFailedHandler();;
                                            return;
                                          }
                                          me->readPayload(header);
                                        }));
  }

  void SubscriberSession_Impl::readPayload(const std::shared_ptr<TcpHeader>& header)
  {
    if (canceled_)
    {
      connectionFailedHandler();
      return;
    }

    if (header->data_size == 0)
    {
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
        log_(logger::LogLevel::Debug,  "SubscriberSession " + endpointToString() + ": Received data size of 0.");
#endif
      readHeaderLength();
      return;
    }

    // Get a buffer. This may be a used or a new one.
    const std::shared_ptr<std::vector<char>> data_buffer = get_buffer_handler_();

    if (data_buffer->capacity() < le64toh(header->data_size))
    {
      // Reserve 10% extra memory
      data_buffer->reserve(static_cast<size_t>(le64toh(header->data_size) * 1.1));
    }

    // Resize the buffer to the required size
    data_buffer->resize(le64toh(header->data_size));

    asio::async_read(data_socket_
                , asio::buffer(data_buffer->data(), le64toh(header->data_size))
                , asio::transfer_at_least(le64toh(header->data_size))
                , asio::bind_executor(data_strand_, [me = shared_from_this(), header, data_buffer](asio::error_code ec, std::size_t /*length*/)
                                    {
                                      if (ec)
                                      {
                                        me->log_(logger::LogLevel::Error,  "SubscriberSession " + me->endpointToString() + ": Error reading payload: " + ec.message());
                                        me->connectionFailedHandler();;
                                        return;
                                      }

                                      // Reset the max amount of reconnects
                                      me->retries_left_ = me->max_reconnection_attempts_;

                                      if (header->type == MessageContentType::ProtocolHandshake)
                                      {
                                        ProtocolHandshakeMessage handshake_message;
                                        const size_t bytes_to_copy = std::min(data_buffer->size(), sizeof(ProtocolHandshakeMessage));
                                        std::memcpy(&handshake_message, data_buffer->data(), bytes_to_copy);
#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
                                        me->log_(logger::LogLevel::Debug,  "SubscriberSession " + me->endpointToString() + ": Received Handshake message. Using Protocol version v" + std::to_string(handshake_message.protocol_version));
#endif
                                        if (handshake_message.protocol_version > 0)
                                        {
                                          me->log_(logger::LogLevel::Error,  "SubscriberSession " + me->endpointToString() + ": Publisher set protocol version to v" + std::to_string(handshake_message.protocol_version) + ". This protocol is not supported.");
                                          me->connectionFailedHandler();
                                          return;
                                        }
                                      }
                                      else if (header->type == MessageContentType::RegularPayload)
                                      {
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
                                        me->log_(logger::LogLevel::DebugVerbose,  "SubscriberSession " + me->endpointToString() + ": Received message of type \"RegularPayload\"");
#endif
                                        // Call the callback first, ...
                                        asio::post(me->data_strand_, [me, data_buffer, header]()
                                                              {
                                                                if (me->canceled_)
                                                                {
                                                                  me->connectionFailedHandler();
                                                                  return;
                                                                }
                                                                me->synchronous_callback_(data_buffer, header);
                                                              });

                                      }
                                      else
                                      {
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
                                        me->log_(logger::LogLevel::DebugVerbose,  "SubscriberSession " + me->endpointToString() + ": Received message has unknow type: " + std::to_string(static_cast<int>(header->type)));
#endif
                                      }

                                      // ... then start reading the next message
                                      asio::post(me->data_strand_, [me]() {
                                        me->readHeaderLength();
                                      });
                                    }));
  }

  //////////////////////////////////////////////
  /// Public API
  //////////////////////////////////////////////
  
  void SubscriberSession_Impl::setSynchronousCallback(const std::function<void(const std::shared_ptr<std::vector<char>>&, const std::shared_ptr<TcpHeader>&)>& callback)
  {
    if (canceled_) return;

    // We let asio set the callback for the following reasons:
    //   - We can protect the variable with the data_strand => If the callback is currently running, the new callback will be applied afterwards
    //   - We don't need an additional mutex, so a synchronous callback should actually be able to set another callback that gets activated once the current callback call ends
    //   - Reading the next message will start once the callback call is finished. Therefore, read and callback are synchronized and the callback calls don't start stacking up
    asio::post(data_strand_, [me = shared_from_this(), callback]()
                      {
                        me->synchronous_callback_ = callback;
                      });
  }

  std::vector<std::pair<std::string, uint16_t>> SubscriberSession_Impl::getPublisherList() const
  {
    return publisher_list_;
  }

  std::pair<std::string, uint16_t> SubscriberSession_Impl::getConnectedPublisher() const
  {
    const std::lock_guard<std::mutex> lock(connected_publisher_endpoint_mutex_);
    return connected_publisher_endpoint_;
  }

  void SubscriberSession_Impl::cancel()
  {
    const bool already_canceled = canceled_.exchange(true);

    if (already_canceled) return;

#if (TCP_PUBSUB_LOG_DEBUG_ENABLED)
    log_(logger::LogLevel::Debug, "SubscriberSession " + endpointToString() + ": Cancelling...");
#endif
    
    {
      asio::error_code ec;
      data_socket_.close(ec);
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
      if (ec)
        log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Failed closing socket: " + ec.message());
      else
        log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Successfully closed socket.");
#endif
    }

    {
      asio::error_code ec;
      data_socket_.cancel(ec); // Even if ec indicates an error, the socket is closed now (according to the documentation)
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
      if (ec)
        log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Failed cancelling socket: " + ec.message());
      else
        log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Successfully canceled socket.");
#endif
    }

    {
    try {
      static_cast<void>(retry_timer_.cancel());
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
      log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Successfully canceled retry timer.");
#endif
    } catch (asio::system_error& err){
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
        log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Failed canceling retry timer: " + err.what());
#endif
  }
    }

    resolver_.cancel();
#if (TCP_PUBSUB_LOG_DEBUG_VERBOSE_ENABLED)
    log_(logger::LogLevel::DebugVerbose, "SubscriberSession " + endpointToString() + ": Successfully canceled resovler.");
#endif
  }

  bool SubscriberSession_Impl::isConnected() const
  {
    asio::error_code ec;
    data_socket_.remote_endpoint(ec);

    // If we can get the remote endpoint, we consider the socket as connected.
    // Otherwise it is not connected.

    return !static_cast<bool>(ec);
  }

  std::string SubscriberSession_Impl::remoteEndpointToString() const
  {
    const std::lock_guard<std::mutex> lock(connected_publisher_endpoint_mutex_);
    return (connected_publisher_endpoint_.first.empty() ? "?" : connected_publisher_endpoint_.first)
            + ":"
            + std::to_string(connected_publisher_endpoint_.second);
  }

  std::string SubscriberSession_Impl::localEndpointToString() const
  {
    asio::error_code ec;
    auto local_endpoint = data_socket_.local_endpoint(ec);
    if (ec)
      return "?";
    else
      return local_endpoint.address().to_string() + ":" + std::to_string(local_endpoint.port());
  }

  std::string SubscriberSession_Impl::endpointToString() const
  {
    return localEndpointToString() + "->" + remoteEndpointToString();
  }
}
