// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "tcp_header.h"
#include "tcp_pubsub/tcp_pubsub_logger.h"

namespace tcp_pubsub {
class SubscriberSession_Impl : public std::enable_shared_from_this<SubscriberSession_Impl>
{
  //////////////////////////////////////////////
  /// Constructor & Destructor
  //////////////////////////////////////////////
  public:
    SubscriberSession_Impl(const std::shared_ptr<asio::io_service>&                             io_service
                          , const std::vector<std::pair<std::string, uint16_t>>&                publisher_list
                          , int                                                                 max_reconnection_attempts
                          , const std::function<std::shared_ptr<std::vector<char>>()>&          get_buffer_handler
                          , const std::function<void(const std::shared_ptr<SubscriberSession_Impl>&)>& session_closed_handler
                          , const tcp_pubsub::logger::logger_t&                                     log_function);


    // Copy
    SubscriberSession_Impl(const SubscriberSession_Impl&)            = delete;
    SubscriberSession_Impl& operator=(const SubscriberSession_Impl&) = delete;

    // Move
    SubscriberSession_Impl& operator=(SubscriberSession_Impl&&)      = delete;
    SubscriberSession_Impl(SubscriberSession_Impl&&)                 = delete;

    // Destructor
    ~SubscriberSession_Impl();

  //////////////////////////////////////////////
  /// Connect to publisher
  //////////////////////////////////////////////
  public:
    void start();

  private:
    void resolveEndpoint(size_t publisher_list_index);
    void connectToEndpoint(const asio::ip::tcp::resolver::iterator& resolved_endpoints, size_t publisher_list_index);

    void sendProtokolHandshakeRequest();

    void connectionFailedHandler();

  /////////////////////////////////////////////
  // Data receiving
  /////////////////////////////////////////////
  private:
    void readHeaderLength();
    void readHeaderContent(const std::shared_ptr<TcpHeader>& header);
    void discardDataBetweenHeaderAndPayload(const std::shared_ptr<TcpHeader>& header, uint16_t bytes_to_discard);
    void readPayload(const std::shared_ptr<TcpHeader>& header);

  //////////////////////////////////////////////
  /// Public API
  //////////////////////////////////////////////
  public:
    void        setSynchronousCallback(const std::function<void(const std::shared_ptr<std::vector<char>>&, const std::shared_ptr<TcpHeader>&)>& callback);

    void        cancel();
    bool        isConnected() const;

    std::vector<std::pair<std::string, uint16_t>> getPublisherList() const;
    std::pair<std::string, uint16_t>              getConnectedPublisher() const;

    std::string remoteEndpointToString() const;
    std::string localEndpointToString() const;
    std::string endpointToString() const;

  //////////////////////////////////////////////
  /// Member variables
  //////////////////////////////////////////////
  private:
    // Endpoint and resolver given by / constructed by the constructor
    std::vector<std::pair<std::string, uint16_t>> publisher_list_;              ///< The list of endpoints that this session will connect to. The first reachable will be used
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::endpoint endpoint_;

    mutable std::mutex               connected_publisher_endpoint_mutex_;       ///< Mutex for the connected_publisher_endpoint_
    std::pair<std::string, uint16_t> connected_publisher_endpoint_;             ///< The endpoint of the publisher we are connected to

    // Amount of retries left
    int                max_reconnection_attempts_;
    int                retries_left_;
    asio::steady_timer retry_timer_;
    std::atomic<bool>  canceled_;

    // TCP Socket & Queue (protected by the strand!)
    asio::ip::tcp::socket         data_socket_;
    asio::io_service::strand      data_strand_;   // Used for socket operations and the callback. This is done so messages don't queue up in the asio stack. We only start receiving new messages, after we have delivered the current one.

    // Handlers
    const std::function<std::shared_ptr<std::vector<char>>()>                                         get_buffer_handler_;         ///< Function for retrieving / constructing an empty buffer
    const std::function<void(const std::shared_ptr<SubscriberSession_Impl>&)>                         session_closed_handler_;     ///< Handler that is called when the session is closed
    std::function<void(const std::shared_ptr<std::vector<char>>&, const std::shared_ptr<TcpHeader>&)> synchronous_callback_;       ///< [PROTECTED BY data_strand_!] Callback that is called when a complete message has been received. Executed in the asio constext, so this must be cheap!

    // Logger
    const tcp_pubsub::logger::logger_t log_;
  };
  } // namespace tcp_pubsub
