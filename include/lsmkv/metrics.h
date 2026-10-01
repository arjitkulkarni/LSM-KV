// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// MetricsRegistry: counters, gauges and latency histograms, exported in the
// Prometheus text exposition format (version 0.0.4).

#ifndef LSMKV_INCLUDE_LSMKV_METRICS_H_
#define LSMKV_INCLUDE_LSMKV_METRICS_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lsmkv {

using MetricLabels = std::vector<std::pair<std::string, std::string>>;

// Base class of every time series. Each concrete metric knows how to render
// its own samples; the registry only groups them into families.
class Metric {
 public:
  enum class Type { kCounter, kGauge, kHistogram };

  Metric(std::string name, MetricLabels labels);
  Metric(const Metric&) = delete;
  Metric& operator=(const Metric&) = delete;
  virtual ~Metric();

  virtual Type type() const = 0;
  // Appends this series' sample lines (no HELP/TYPE header).
  virtual void AppendSamples(std::string* out) const = 0;

  const std::string& name() const { return name_; }
  const MetricLabels& labels() const { return labels_; }

 protected:
  // Renders {k="v",...,extra}; `extra` is an already-rendered pair or "".
  std::string RenderLabels(const std::string& extra = "") const;

 private:
  const std::string name_;
  const MetricLabels labels_;
};

// Monotonically increasing count. Lock-free.
class Counter final : public Metric {
 public:
  using Metric::Metric;
  Type type() const override { return Type::kCounter; }
  void Inc(uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
  uint64_t Value() const { return value_.load(std::memory_order_relaxed); }
  void AppendSamples(std::string* out) const override;

 private:
  std::atomic<uint64_t> value_{0};
};

// Point-in-time value. Either set explicitly or computed at scrape time.
class Gauge final : public Metric {
 public:
  Gauge(std::string name, MetricLabels labels,
        std::function<double()> fn = nullptr);
  Type type() const override { return Type::kGauge; }
  void Set(double v) { value_.store(v, std::memory_order_relaxed); }
  double Value() const;
  void AppendSamples(std::string* out) const override;

 private:
  std::atomic<double> value_{0.0};
  const std::function<double()> fn_;
};

// Log-linear ("HDR-style") latency histogram over nanoseconds. Every power
// of two is split into 2^kSubBucketBits linear sub-buckets, so any recorded
// value is reported with <= 1/2^kSubBucketBits relative error (3.1% here)
// in O(1) time and fixed memory. Recording is a handful of relaxed atomic
// adds, spread over kStripes cache-line-separated stripes so concurrent
// writers do not all hammer the same counters.
class Histogram final : public Metric {
 public:
  static constexpr int kSubBucketBits = 5;
  static constexpr int kSubBuckets = 1 << kSubBucketBits;
  static constexpr int kMaxExponent = 42;  // values are clamped to 2^42 ns
  static constexpr int kNumBuckets = (kMaxExponent - kSubBucketBits + 1) *
                                         kSubBuckets +
                                     kSubBuckets;
  static constexpr int kStripes = 8;

  Histogram(std::string name, MetricLabels labels);
  ~Histogram() override;
  Type type() const override { return Type::kHistogram; }

  void Record(uint64_t nanos);

  uint64_t Count() const;
  uint64_t SumNanos() const;
  // Upper bound of the bucket containing the p-th percentile (0 < p <= 100).
  uint64_t PercentileNanos(double p) const;
  void AppendSamples(std::string* out) const override;

  static int BucketFor(uint64_t v);
  static uint64_t BucketUpperBound(int index);

 private:
  struct alignas(64) Stripe {
    std::array<std::atomic<uint64_t>, kNumBuckets> buckets{};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum{0};
  };
  std::vector<uint64_t> Snapshot(uint64_t* count, uint64_t* sum) const;

  std::unique_ptr<Stripe[]> stripes_;
};

// RAII registration of a scrape-time callback gauge; unregisters on
// destruction so the callback can never outlive the object it reads.
class MetricHandle {
 public:
  MetricHandle(const MetricHandle&) = delete;
  MetricHandle& operator=(const MetricHandle&) = delete;
  ~MetricHandle();

 private:
  friend class MetricsRegistry;
  MetricHandle(class MetricsRegistry* registry, std::string name,
               std::string key)
      : registry_(registry), name_(std::move(name)), key_(std::move(key)) {}
  class MetricsRegistry* registry_;
  std::string name_;
  std::string key_;
};

class MetricsRegistry {
 public:
  MetricsRegistry() = default;
  MetricsRegistry(const MetricsRegistry&) = delete;
  MetricsRegistry& operator=(const MetricsRegistry&) = delete;

  // Get-or-create. The returned pointer is owned by the registry and valid
  // for the registry's lifetime.
  Counter* GetCounter(const std::string& name, const std::string& help,
                      const MetricLabels& labels = {});
  Gauge* GetGauge(const std::string& name, const std::string& help,
                  const MetricLabels& labels = {});
  Histogram* GetHistogram(const std::string& name, const std::string& help,
                          const MetricLabels& labels = {});
  [[nodiscard]] std::unique_ptr<MetricHandle> RegisterCallbackGauge(
      const std::string& name, const std::string& help,
      const MetricLabels& labels, std::function<double()> fn);

  // Prometheus text exposition of every registered series.
  std::string ExportPrometheus() const;

  // Current value of a counter or gauge series; false if absent.
  bool GetValue(const std::string& name, const MetricLabels& labels,
                double* value) const;

 private:
  friend class MetricHandle;
  struct Family {
    std::string help;
    Metric::Type type = Metric::Type::kCounter;
    std::map<std::string, std::unique_ptr<Metric>> series;
  };
  template <typename T, typename... Args>
  T* GetOrCreate(const std::string& name, const std::string& help,
                 const MetricLabels& labels, Metric::Type type,
                 Args&&... args);
  void Unregister(const std::string& name, const std::string& key);

  mutable std::mutex mu_;
  std::map<std::string, Family> families_;
};

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_METRICS_H_
