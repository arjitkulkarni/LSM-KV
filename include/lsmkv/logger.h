// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Logger: structured (JSON lines) event logging.

#ifndef LSMKV_INCLUDE_LSMKV_LOGGER_H_
#define LSMKV_INCLUDE_LSMKV_LOGGER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

class Env;

enum class LogLevel : unsigned char { kDebug = 0, kInfo, kWarn, kError };

const char* LogLevelName(LogLevel level);

// Builder for the key/value payload of one event. Values are JSON-encoded
// as they are added, so the logger itself never re-parses anything:
//
//   logger->Log(LogLevel::kInfo, "flush_finished",
//               LogFields().Add("file", number).Add("bytes", size));
class LogFields {
 public:
  template <typename T>
  LogFields& Add(const char* key, const T& value) {
    AppendKey(key);
    if constexpr (std::is_same_v<T, bool>) {
      json_ += value ? "true" : "false";
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
      json_ += std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_integral_v<T>) {
      json_ += std::to_string(static_cast<unsigned long long>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
      AppendDouble(static_cast<double>(value));
    } else {
      AppendString(Slice(value));
    }
    return *this;
  }

  // Comma-separated "key":value pairs, without surrounding braces.
  const std::string& json() const { return json_; }

 private:
  void AppendKey(const char* key);
  void AppendString(const Slice& s);
  void AppendDouble(double d);

  std::string json_;
};

// Appends `s` to *out as a quoted, escaped JSON string.
void AppendJsonString(std::string* out, const Slice& s);

class Logger {
 public:
  Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  virtual ~Logger();

  // Emits {"ts":...,"level":...,"event":...,<fields>} as one line.
  virtual void Log(LogLevel level, const Slice& event,
                   const LogFields& fields) = 0;
};

// Appends JSON lines to `fname` (created/truncated).
Status NewJsonFileLogger(Env* env, const std::string& fname,
                         std::shared_ptr<Logger>* result);
// Writes JSON lines at or above `min_level` to stderr.
std::shared_ptr<Logger> NewStderrLogger(LogLevel min_level);
// Discards everything.
std::shared_ptr<Logger> NewNullLogger();

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_LOGGER_H_
