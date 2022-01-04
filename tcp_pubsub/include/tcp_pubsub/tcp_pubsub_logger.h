// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.

#pragma once

#include <string>
#include <functional>
#include <iostream>

#include <stdint.h>

#include "tcp_pubsub_version.h"

namespace tcp_pubsub
{
  namespace logger
  {
    enum class LogLevel
    {
      DebugVerbose,
      Debug,
      Info,
      Warning,
      Error,
      Fatal,
    };

    typedef std::function<void(const LogLevel, const std::string&)> logger_t;

    static const logger_t default_logger
          = [](const LogLevel log_level, const std::string& message)
              {
                switch (log_level)
                {
                case LogLevel::DebugVerbose:
                  std::cout << "[TCP ps] [Debug+]  " + message + "\n";
                  break;
                case LogLevel::Debug:
                  std::cout << "[TCP ps] [Debug]   " + message + "\n";
                  break;
                case LogLevel::Info:
                  std::cout << "[TCP ps] [Info]    " + message + "\n";
                  break;
                case LogLevel::Warning:
                  std::cerr << "[TCP ps] [Warning] " + message + "\n";
                  break;
                case LogLevel::Error:
                  std::cerr << "[TCP ps] [Error]   " + message + "\n";
                  break;
                case LogLevel::Fatal:
                  std::cerr << "[TCP ps] [Fatal]   " + message + "\n";
                  break;
                default:
                  break;
                }
              };

    static const logger_t logger_no_verbose_debug
          = [](const LogLevel log_level, const std::string& message)
              {
                switch (log_level)
                {
                case LogLevel::DebugVerbose:
                  break;
                case LogLevel::Debug:
                  std::cout << "[TCP ps] [Debug]   " + message + "\n";
                  break;
                case LogLevel::Info:
                  std::cout << "[TCP ps] [Info]    " + message + "\n";
                  break;
                case LogLevel::Warning:
                  std::cerr << "[TCP ps] [Warning] " + message + "\n";
                  break;
                case LogLevel::Error:
                  std::cerr << "[TCP ps] [Error]   " + message + "\n";
                  break;
                case LogLevel::Fatal:
                  std::cerr << "[TCP ps] [Fatal]   " + message + "\n";
                  break;
                default:
                  break;
                }
              };
  }
}
