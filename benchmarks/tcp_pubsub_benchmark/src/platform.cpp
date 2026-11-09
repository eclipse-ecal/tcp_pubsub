// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include "platform.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <psapi.h>
  #include <tlhelp32.h>
  #include <mmsystem.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <sys/resource.h>
  #include <sys/sysctl.h>
  #include <sys/utsname.h>
#else
  #include <sys/utsname.h>
  #include <time.h>
#endif

namespace bench
{
#if defined(_WIN32)

  namespace
  {
    double fileTimeSeconds(const FILETIME& ft)
    {
      ULARGE_INTEGER value;
      value.LowPart  = ft.dwLowDateTime;
      value.HighPart = ft.dwHighDateTime;
      return static_cast<double>(value.QuadPart) * 1e-7;   // 100 ns units
    }
  }

  ProcessSnapshot sampleProcess(bool include_threads)
  {
    ProcessSnapshot snapshot;

    FILETIME creation, exit_time, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
    {
      snapshot.cpu_seconds = fileTimeSeconds(kernel) + fileTimeSeconds(user);
    }

    PROCESS_MEMORY_COUNTERS counters;
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
      snapshot.rss_bytes      = counters.WorkingSetSize;
      snapshot.peak_rss_bytes = counters.PeakWorkingSetSize;
    }

    // Enumerates all threads of the whole system: takes tens of milliseconds
    const HANDLE threads_snapshot = include_threads ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
    if (threads_snapshot != INVALID_HANDLE_VALUE)
    {
      THREADENTRY32 entry;
      entry.dwSize = sizeof(entry);
      int count = 0;
      const DWORD pid = GetCurrentProcessId();
      if (Thread32First(threads_snapshot, &entry))
      {
        do
        {
          if (entry.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(threads_snapshot, &entry));
      }
      CloseHandle(threads_snapshot);
      snapshot.threads = count;
    }

    return snapshot;
  }

  std::string osDescription()
  {
    return "Windows";
  }

  std::string cpuDescription()
  {
    const char* value = std::getenv("PROCESSOR_IDENTIFIER");
    return value ? value : "unknown";
  }

  void raiseTimerResolution()
  {
    timeBeginPeriod(1);
  }

#elif defined(__APPLE__)

  ProcessSnapshot sampleProcess(bool include_threads)
  {
    ProcessSnapshot snapshot;

    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
      snapshot.cpu_seconds = static_cast<double>(usage.ru_utime.tv_sec)  + static_cast<double>(usage.ru_utime.tv_usec) * 1e-6
                           + static_cast<double>(usage.ru_stime.tv_sec)  + static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
    }

    mach_task_basic_info_data_t info;
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    {
      snapshot.rss_bytes      = info.resident_size;
      snapshot.peak_rss_bytes = info.resident_size_max;
    }

    thread_act_array_t     list = nullptr;
    mach_msg_type_number_t n    = 0;
    if (include_threads && task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS)
    {
      for (mach_msg_type_number_t i = 0; i < n; ++i) mach_port_deallocate(mach_task_self(), list[i]);
      vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_t));
      snapshot.threads = static_cast<int>(n);
    }

    return snapshot;
  }

  std::string osDescription()
  {
    utsname name;
    if (uname(&name) == 0) return std::string("macOS (Darwin ") + name.release + ")";
    return "macOS";
  }

  std::string cpuDescription()
  {
    char   buffer[256];
    size_t size = sizeof(buffer);
    if (sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) return buffer;
    return "unknown";
  }

  void raiseTimerResolution() {}

#else

  namespace
  {
    // Reads "<key>   <value> kB" lines of /proc/self/status
    size_t procStatusKiB(const std::string& key)
    {
      std::ifstream file("/proc/self/status");
      std::string   line;
      while (std::getline(file, line))
      {
        if (line.compare(0, key.size(), key) == 0)
        {
          return static_cast<size_t>(std::strtoull(line.c_str() + key.size(), nullptr, 10));
        }
      }
      return 0;
    }
  }

  ProcessSnapshot sampleProcess(bool include_threads)
  {
    ProcessSnapshot snapshot;

    timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
    {
      snapshot.cpu_seconds = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    snapshot.rss_bytes      = procStatusKiB("VmRSS:") * 1024;
    snapshot.peak_rss_bytes = procStatusKiB("VmHWM:") * 1024;
    if (include_threads) snapshot.threads = static_cast<int>(procStatusKiB("Threads:"));

    return snapshot;
  }

  std::string osDescription()
  {
    utsname name;
    if (uname(&name) == 0) return std::string(name.sysname) + " " + name.release;
    return "Linux";
  }

  std::string cpuDescription()
  {
    std::ifstream file("/proc/cpuinfo");
    std::string   line;
    while (std::getline(file, line))
    {
      if (line.compare(0, 10, "model name") == 0)
      {
        const auto colon = line.find(':');
        if (colon != std::string::npos) return line.substr(colon + 2);
      }
    }
    return "unknown";
  }

  void raiseTimerResolution() {}

#endif

  unsigned hardwareThreads()
  {
    return std::thread::hardware_concurrency();
  }
}
