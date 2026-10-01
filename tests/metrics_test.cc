// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/metrics.h"

#include <gtest/gtest.h>

#include <sstream>
#include <thread>

#include "server/metrics_http_server.h"

namespace lsmkv {

TEST(MetricsTest, GetOrCreateReturnsTheSameSeries) {
  MetricsRegistry r;
  Counter* a = r.GetCounter("ops_total", "help", {{"op", "put"}});
  Counter* b = r.GetCounter("ops_total", "help", {{"op", "put"}});
  Counter* c = r.GetCounter("ops_total", "help", {{"op", "get"}});
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  a->Inc(3);
  double v = 0;
  ASSERT_TRUE(r.GetValue("ops_total", {{"op", "put"}}, &v));
  EXPECT_EQ(3.0, v);
}

TEST(MetricsTest, PrometheusExpositionFormat) {
  MetricsRegistry r;
  r.GetCounter("lsmkv_ops_total", "Operations by type", {{"op", "put"}})->Inc(7);
  r.GetGauge("lsmkv_memtable_bytes", "Memtable size")->Set(1024);
  Histogram* h = r.GetHistogram("lsmkv_latency_seconds", "Latency");
  h->Record(1500);      // 1.5 us
  h->Record(3000000);   // 3 ms
  const std::string text = r.ExportPrometheus();

  EXPECT_NE(std::string::npos, text.find("# HELP lsmkv_ops_total Operations by type\n"));
  EXPECT_NE(std::string::npos, text.find("# TYPE lsmkv_ops_total counter\n"));
  EXPECT_NE(std::string::npos, text.find("lsmkv_ops_total{op=\"put\"} 7\n"));
  EXPECT_NE(std::string::npos, text.find("# TYPE lsmkv_memtable_bytes gauge\n"));
  EXPECT_NE(std::string::npos, text.find("# TYPE lsmkv_latency_seconds histogram\n"));
  EXPECT_NE(std::string::npos, text.find("lsmkv_latency_seconds_bucket{le=\"+Inf\"} 2\n"));
  EXPECT_NE(std::string::npos, text.find("lsmkv_latency_seconds_count 2\n"));

  // Histogram buckets must be cumulative (non-decreasing).
  std::istringstream in(text);
  std::string line;
  long long prev = -1;
  while (std::getline(in, line)) {
    if (line.rfind("lsmkv_latency_seconds_bucket", 0) != 0) continue;
    const long long v = std::stoll(line.substr(line.rfind(' ') + 1));
    EXPECT_GE(v, prev) << line;
    prev = v;
  }
}

TEST(MetricsTest, LabelValuesAreEscaped) {
  MetricsRegistry r;
  r.GetCounter("x_total", "h", {{"path", "a\"b\\c"}})->Inc();
  EXPECT_NE(std::string::npos,
            r.ExportPrometheus().find("x_total{path=\"a\\\"b\\\\c\"} 1"));
}

TEST(MetricsTest, CallbackGaugeUnregistersWithItsHandle) {
  MetricsRegistry r;
  int calls = 0;
  {
    auto handle = r.RegisterCallbackGauge("cb", "help", {}, [&calls] {
      calls++;
      return 42.0;
    });
    EXPECT_NE(std::string::npos, r.ExportPrometheus().find("cb 42"));
  }
  EXPECT_EQ(std::string::npos, r.ExportPrometheus().find("cb 42"));
  EXPECT_EQ(1, calls);
}

TEST(MetricsTest, CountersAreThreadSafe) {
  MetricsRegistry r;
  Counter* c = r.GetCounter("n_total", "h");
  Histogram* h = r.GetHistogram("lat", "h");
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([c, h] {
      for (int i = 0; i < 100000; i++) {
        c->Inc();
        h->Record(static_cast<uint64_t>(i));
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(800000u, c->Value());
  EXPECT_EQ(800000u, h->Count());
}

TEST(MetricsHttpServerTest, StartsOnAnEphemeralPortAndStops) {
  std::unique_ptr<MetricsHttpServer> server;
  Status s = MetricsHttpServer::Start("127.0.0.1", 0, [] { return std::string("x 1\n"); },
                                      &server);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_GT(server->port(), 0);
  server.reset();  // must join promptly
}

}  // namespace lsmkv
