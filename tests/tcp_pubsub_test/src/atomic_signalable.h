// Copyright (c) Continental. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

template <typename T>
class atomic_signalable
{
public:
  atomic_signalable(T initial_value) : value(initial_value) {}

  atomic_signalable<T>& operator=(const T new_value)
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value = new_value;
    cv.notify_all();
    return *this;
  }

  T operator++()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    T newValue = ++value;
    cv.notify_all();
    return newValue;
  }

  T operator++(T) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    T oldValue = value++;
    cv.notify_all();
    return oldValue;
  }

  T operator--()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    T newValue = --value;
    cv.notify_all();
    return newValue;
  }

  T operator--(T) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    T oldValue = value--;
    cv.notify_all();
    return oldValue;
  }

  T operator+=(const T& other) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value += other;
    cv.notify_all();
    return value;
  }

  T operator-=(const T& other) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value -= other;
    cv.notify_all();
    return value;
  }

  T operator*=(const T& other) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value *= other;
    cv.notify_all();
    return value;
  }

  T operator/=(const T& other) 
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value /= other;
    cv.notify_all();
    return value;
  }

  T operator%=(const T& other)
  {
    const std::lock_guard<std::mutex> lock(mutex);
    value %= other;
    cv.notify_all();
    return value;
  }

  template <typename Predicate>
  bool wait_for(Predicate predicate, std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [&]() { return predicate(value); });
  }

  T get() const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value;
  }

  bool operator==(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value == other;
  }

  bool operator==(const atomic_signalable<T>& other) const
  {
    std::lock_guard<std::mutex> lock_this(mutex);
    std::lock_guard<std::mutex> lock_other(other.mutex);
    return value == other.value;
  }

  bool operator!=(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value != other;
  }

  bool operator<(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value < other;
  }

  bool operator<=(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value <= other;
  }

  bool operator>(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value > other;
  }

  bool operator>=(T other) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return value >= other;
  }

private:
  T value;
  std::condition_variable cv;
  mutable std::mutex mutex;
};


template <typename T>
bool operator==(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic == other;
}

template <typename T>
bool operator!=(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic != other;
}

template <typename T>
bool operator<(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic > other;
}

template <typename T>
bool operator<=(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic >= other;
}

template <typename T>
bool operator>(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic < other;
}

template <typename T>
bool operator>=(const T& other, const atomic_signalable<T>& atomic)
{
  return atomic <= other;
}
