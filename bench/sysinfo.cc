// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "sysinfo.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef LSMKV_BUILD_FLAGS
#define LSMKV_BUILD_FLAGS "unknown"
#endif

namespace lsmkv::bench {

namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' ||
                        s.back() == '\0')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') i++;
  return s.substr(i);
}

#if defined(_WIN32)
std::string RegistryString(HKEY root, const char* path, const char* value) {
  char buf[256];
  DWORD size = sizeof(buf);
  if (::RegGetValueA(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) ==
      ERROR_SUCCESS) {
    return Trim(std::string(buf));
  }
  return "";
}
#endif

}  // namespace

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
  }
  return out;
}

SystemInfo CollectSystemInfo(int argc, char** argv) {
  SystemInfo info;
  info.hardware_threads = std::thread::hardware_concurrency();
  info.build_flags = LSMKV_BUILD_FLAGS;
#if defined(__clang__)
  info.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
  info.compiler = "gcc " __VERSION__;
#elif defined(_MSC_VER)
  info.compiler = "msvc " + std::to_string(_MSC_VER);
#endif

#if defined(_WIN32)
  info.cpu_model = RegistryString(
      HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
      "ProcessorNameString");
  const std::string product = RegistryString(
      HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
  const std::string build = RegistryString(
      HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild");
  const std::string display = RegistryString(
      HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
  // Windows 11 still reports "Windows 10" in ProductName; the build number
  // (>= 22000) is the reliable discriminator.
  std::string name = product;
  if (!build.empty() && std::stoi(build) >= 22000 && name.rfind("Windows 10", 0) == 0) {
    name.replace(0, 10, "Windows 11");
  }
  info.os = name + " " + display + " (build " + build + ")";
  MEMORYSTATUSEX mem;
  mem.dwLength = sizeof(mem);
  if (::GlobalMemoryStatusEx(&mem)) info.memory_bytes = mem.ullTotalPhys;
  info.cpu_governor = "n/a (Windows power plan)";
#else
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      info.cpu_model = Trim(line.substr(line.find(':') + 1));
      break;
    }
  }
  struct utsname u {};
  if (uname(&u) == 0) info.os = std::string(u.sysname) + " " + u.release + " " + u.machine;
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    info.memory_bytes = static_cast<unsigned long long>(pages) *
                        static_cast<unsigned long long>(page_size);
  }
  std::ifstream gov("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  if (std::getline(gov, line)) {
    info.cpu_governor = Trim(line);
  } else {
    info.cpu_governor = "unknown";
  }
#endif

  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  info.timestamp_utc = buf;

  for (int i = 0; i < argc; i++) {
    if (i > 0) info.command_line += ' ';
    info.command_line += argv[i];
  }
  return info;
}

void KeepSystemAwake() {
#if defined(_WIN32)
  ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
}

std::string ToJson(const SystemInfo& i) {
  char mem[32];
  std::snprintf(mem, sizeof(mem), "%.1f", i.memory_bytes / 1073741824.0);
  return std::string("{") + "\"cpu_model\":\"" + JsonEscape(i.cpu_model) + "\"," +
         "\"hardware_threads\":" + std::to_string(i.hardware_threads) + "," +
         "\"memory_gib\":" + mem + "," + "\"os\":\"" + JsonEscape(i.os) + "\"," +
         "\"cpu_governor\":\"" + JsonEscape(i.cpu_governor) + "\"," +
         "\"compiler\":\"" + JsonEscape(i.compiler) + "\"," + "\"build_flags\":\"" +
         JsonEscape(i.build_flags) + "\"," + "\"timestamp_utc\":\"" + i.timestamp_utc +
         "\"," + "\"command_line\":\"" + JsonEscape(i.command_line) + "\"}";
}

}  // namespace lsmkv::bench
