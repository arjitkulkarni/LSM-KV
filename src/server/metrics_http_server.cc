// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "server/metrics_http_server.h"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void CloseSocket(socket_t s) { ::closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void CloseSocket(socket_t s) { ::close(s); }
#endif

namespace lsmkv {

namespace {

#if defined(_WIN32)
// Winsock must be initialized once per process before any socket call.
bool EnsureWinsock() {
  static const bool ok = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
}
#else
bool EnsureWinsock() { return true; }
#endif

void SendAll(socket_t s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = static_cast<int>(::send(s, data.data() + sent,
                                          static_cast<int>(data.size() - sent), 0));
    if (n <= 0) return;
    sent += static_cast<size_t>(n);
  }
}

std::string Response(const char* status, const char* content_type,
                     const std::string& body) {
  std::string r = "HTTP/1.1 ";
  r += status;
  r += "\r\nContent-Type: ";
  r += content_type;
  r += "\r\nContent-Length: " + std::to_string(body.size());
  r += "\r\nConnection: close\r\n\r\n";
  r += body;
  return r;
}

}  // namespace

Status MetricsHttpServer::Start(const std::string& bind_address, int port,
                                BodyFn metrics,
                                std::unique_ptr<MetricsHttpServer>* server) {
  if (!EnsureWinsock()) return Status::IOError("WSAStartup failed");
  const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) return Status::IOError("socket() failed");

  int yes = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
               sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    CloseSocket(s);
    return Status::InvalidArgument("bad bind address", bind_address);
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(s);
    return Status::IOError("bind failed on port", std::to_string(port));
  }
  if (::listen(s, 16) != 0) {
    CloseSocket(s);
    return Status::IOError("listen failed");
  }
  socklen_t len = sizeof(addr);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  const int bound_port = ntohs(addr.sin_port);

  server->reset(new MetricsHttpServer(static_cast<intptr_t>(s), bound_port,
                                      std::move(metrics)));
  return Status::OK();
}

MetricsHttpServer::MetricsHttpServer(intptr_t listen_socket, int port,
                                     BodyFn metrics)
    : listen_socket_(listen_socket), port_(port), metrics_(std::move(metrics)) {
  thread_ = std::thread([this] { Serve(); });
}

MetricsHttpServer::~MetricsHttpServer() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
  CloseSocket(static_cast<socket_t>(listen_socket_));
}

void MetricsHttpServer::Serve() {
  const auto ls = static_cast<socket_t>(listen_socket_);
  while (!stop_.load()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(ls, &readable);
    timeval tv{};
    tv.tv_usec = 100 * 1000;
    const int ready = ::select(static_cast<int>(ls) + 1, &readable, nullptr,
                               nullptr, &tv);
    if (ready <= 0) continue;
    const socket_t client = ::accept(ls, nullptr, nullptr);
    if (client == kInvalidSocket) continue;
    HandleConnection(static_cast<intptr_t>(client));
  }
}

void MetricsHttpServer::HandleConnection(intptr_t raw_client) {
  const auto client = static_cast<socket_t>(raw_client);
  std::string request;
  char buf[2048];
  // Read until the end of the request headers (we ignore any body).
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
    const int n = static_cast<int>(::recv(client, buf, sizeof(buf), 0));
    if (n <= 0) break;
    request.append(buf, static_cast<size_t>(n));
  }

  std::string response;
  if (request.rfind("GET /metrics", 0) == 0) {
    response = Response("200 OK", "text/plain; version=0.0.4; charset=utf-8",
                        metrics_());
  } else if (request.rfind("GET /healthz", 0) == 0) {
    response = Response("200 OK", "text/plain", "ok\n");
  } else {
    response = Response("404 Not Found", "text/plain", "try /metrics\n");
  }
  SendAll(client, response);
  CloseSocket(client);
}

}  // namespace lsmkv
