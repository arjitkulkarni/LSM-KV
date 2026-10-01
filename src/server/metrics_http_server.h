// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_SERVER_METRICS_HTTP_SERVER_H_
#define LSMKV_SRC_SERVER_METRICS_HTTP_SERVER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "lsmkv/status.h"

namespace lsmkv {

// A deliberately tiny HTTP/1.1 server for Prometheus scraping:
//
//   GET /metrics  -> text/plain; version=0.0.4 (the exposition format)
//   GET /healthz  -> "ok"
//
// One thread, one connection at a time, Connection: close. Scrapes arrive
// every few seconds, so anything fancier would be complexity without a
// customer. Stopping is prompt: the accept loop polls a flag every 100 ms.
class MetricsHttpServer {
 public:
  using BodyFn = std::function<std::string()>;

  // Binds to `bind_address`:`port` (port 0 picks a free port) and starts
  // serving on a background thread.
  static Status Start(const std::string& bind_address, int port, BodyFn metrics,
                      std::unique_ptr<MetricsHttpServer>* server);

  MetricsHttpServer(const MetricsHttpServer&) = delete;
  MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;
  ~MetricsHttpServer();  // stops and joins

  int port() const { return port_; }

 private:
  MetricsHttpServer(intptr_t listen_socket, int port, BodyFn metrics);
  void Serve();
  void HandleConnection(intptr_t client);

  const intptr_t listen_socket_;
  const int port_;
  const BodyFn metrics_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_SERVER_METRICS_HTTP_SERVER_H_
