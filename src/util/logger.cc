// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/logger.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "lsmkv/env.h"

namespace lsmkv {

Logger::~Logger() = default;

const char* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "info";
}

void AppendJsonString(std::string* out, const Slice& s) {
  static const char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (size_t i = 0; i < s.size(); i++) {
    const auto c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20 || c >= 0x7f) {
          // Keys are arbitrary bytes; escape anything non-printable so every
          // line stays valid JSON regardless of payload.
          *out += "\\u00";
          out->push_back(kHex[c >> 4]);
          out->push_back(kHex[c & 0xf]);
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void LogFields::AppendKey(const char* key) {
  if (!json_.empty()) json_.push_back(',');
  AppendJsonString(&json_, Slice(key));
  json_.push_back(':');
}

void LogFields::AppendString(const Slice& s) { AppendJsonString(&json_, s); }

void LogFields::AppendDouble(double d) {
  if (!std::isfinite(d)) {
    json_ += "null";
    return;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", d);
  json_ += buf;
}

namespace {

std::string Timestamp() {
  using std::chrono::system_clock;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch())
                          .count() %
                      1000000;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<long>(micros));
  return buf;
}

std::string FormatLine(LogLevel level, const Slice& event,
                       const LogFields& fields) {
  std::string line = "{\"ts\":\"" + Timestamp() + "\",\"level\":\"" +
                     LogLevelName(level) + "\",\"event\":";
  AppendJsonString(&line, event);
  if (!fields.json().empty()) {
    line.push_back(',');
    line += fields.json();
  }
  line += "}\n";
  return line;
}

class JsonFileLogger final : public Logger {
 public:
  explicit JsonFileLogger(std::unique_ptr<WritableFile> file)
      : file_(std::move(file)) {}
  ~JsonFileLogger() override { (void)file_->Close(); }

  void Log(LogLevel level, const Slice& event,
           const LogFields& fields) override {
    const std::string line = FormatLine(level, event, fields);
    std::lock_guard<std::mutex> l(mu_);
    // Logging must never fail the operation being logged.
    (void)file_->Append(line);
    (void)file_->Flush();
  }

 private:
  std::mutex mu_;
  std::unique_ptr<WritableFile> file_;
};

class StderrLogger final : public Logger {
 public:
  explicit StderrLogger(LogLevel min) : min_(min) {}
  void Log(LogLevel level, const Slice& event,
           const LogFields& fields) override {
    if (level < min_) return;
    const std::string line = FormatLine(level, event, fields);
    std::fwrite(line.data(), 1, line.size(), stderr);
  }

 private:
  LogLevel min_;
};

class NullLogger final : public Logger {
 public:
  void Log(LogLevel, const Slice&, const LogFields&) override {}
};

}  // namespace

Status NewJsonFileLogger(Env* env, const std::string& fname,
                         std::shared_ptr<Logger>* result) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (s.ok()) *result = std::make_shared<JsonFileLogger>(std::move(file));
  return s;
}

std::shared_ptr<Logger> NewStderrLogger(LogLevel min_level) {
  return std::make_shared<StderrLogger>(min_level);
}

std::shared_ptr<Logger> NewNullLogger() {
  return std::make_shared<NullLogger>();
}

}  // namespace lsmkv
