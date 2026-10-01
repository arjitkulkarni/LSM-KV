// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/metrics.h"

#include <cmath>
#include <cstdio>

#include "util/bits.h"

namespace lsmkv {

namespace {

std::string FormatDouble(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string EscapeLabelValue(const std::string& v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out += c;
    }
  }
  return out;
}

std::string LabelKey(const MetricLabels& labels) {
  std::string key;
  for (const auto& [k, v] : labels) {
    key += k;
    key += '=';
    key += v;
    key += ',';
  }
  return key;
}

// Stable per-thread stripe index, assigned round-robin on first use.
// The thread_local is constant-initialized and filled in lazily: a
// dynamically-initialized thread_local needs a TLS init wrapper, which some
// toolchains (Clang targeting MinGW) get wrong.
int ThreadStripe() {
  static std::atomic<int> next{0};
  thread_local int stripe = -1;
  if (stripe < 0) {
    stripe = next.fetch_add(1, std::memory_order_relaxed) % Histogram::kStripes;
  }
  return stripe;
}

}  // namespace

// ---- Metric -----------------------------------------------------------------

Metric::Metric(std::string name, MetricLabels labels)
    : name_(std::move(name)), labels_(std::move(labels)) {}

Metric::~Metric() = default;

std::string Metric::RenderLabels(const std::string& extra) const {
  if (labels_.empty() && extra.empty()) return "";
  std::string out = "{";
  bool first = true;
  for (const auto& [k, v] : labels_) {
    if (!first) out += ',';
    first = false;
    out += k;
    out += "=\"";
    out += EscapeLabelValue(v);
    out += '"';
  }
  if (!extra.empty()) {
    if (!first) out += ',';
    out += extra;
  }
  out += '}';
  return out;
}

// ---- Counter / Gauge ----------------------------------------------------------

void Counter::AppendSamples(std::string* out) const {
  *out += name();
  *out += RenderLabels();
  *out += ' ';
  *out += std::to_string(Value());
  *out += '\n';
}

Gauge::Gauge(std::string name, MetricLabels labels, std::function<double()> fn)
    : Metric(std::move(name), std::move(labels)), fn_(std::move(fn)) {}

double Gauge::Value() const {
  return fn_ ? fn_() : value_.load(std::memory_order_relaxed);
}

void Gauge::AppendSamples(std::string* out) const {
  *out += name();
  *out += RenderLabels();
  *out += ' ';
  *out += FormatDouble(Value());
  *out += '\n';
}

// ---- Histogram ------------------------------------------------------------------

Histogram::Histogram(std::string name, MetricLabels labels)
    : Metric(std::move(name), std::move(labels)),
      stripes_(new Stripe[kStripes]) {}

Histogram::~Histogram() = default;

int Histogram::BucketFor(uint64_t v) {
  return LogLinearBucket<kSubBucketBits, kMaxExponent>(v);
}

uint64_t Histogram::BucketUpperBound(int index) {
  return LogLinearUpperBound<kSubBucketBits>(index);
}

void Histogram::Record(uint64_t nanos) {
  Stripe& s = stripes_[static_cast<size_t>(ThreadStripe())];
  s.buckets[static_cast<size_t>(BucketFor(nanos))].fetch_add(
      1, std::memory_order_relaxed);
  s.count.fetch_add(1, std::memory_order_relaxed);
  s.sum.fetch_add(nanos, std::memory_order_relaxed);
}

std::vector<uint64_t> Histogram::Snapshot(uint64_t* count,
                                          uint64_t* sum) const {
  std::vector<uint64_t> merged(kNumBuckets, 0);
  *count = 0;
  *sum = 0;
  for (int i = 0; i < kStripes; i++) {
    const Stripe& s = stripes_[static_cast<size_t>(i)];
    for (int b = 0; b < kNumBuckets; b++) {
      merged[static_cast<size_t>(b)] +=
          s.buckets[static_cast<size_t>(b)].load(std::memory_order_relaxed);
    }
    *count += s.count.load(std::memory_order_relaxed);
    *sum += s.sum.load(std::memory_order_relaxed);
  }
  return merged;
}

uint64_t Histogram::Count() const {
  uint64_t c = 0;
  for (int i = 0; i < kStripes; i++) {
    c += stripes_[static_cast<size_t>(i)].count.load(std::memory_order_relaxed);
  }
  return c;
}

uint64_t Histogram::SumNanos() const {
  uint64_t c = 0;
  for (int i = 0; i < kStripes; i++) {
    c += stripes_[static_cast<size_t>(i)].sum.load(std::memory_order_relaxed);
  }
  return c;
}

uint64_t Histogram::PercentileNanos(double p) const {
  uint64_t count;
  uint64_t sum;
  const std::vector<uint64_t> b = Snapshot(&count, &sum);
  // Bucket totals are read without a global lock, so they may be a few
  // samples ahead of `count`; use the bucket total as the population.
  uint64_t total = 0;
  for (uint64_t v : b) total += v;
  if (total == 0) return 0;
  auto rank = static_cast<uint64_t>(std::ceil(p / 100.0 * total));
  if (rank < 1) rank = 1;
  uint64_t cumulative = 0;
  for (int i = 0; i < kNumBuckets; i++) {
    cumulative += b[static_cast<size_t>(i)];
    if (cumulative >= rank) return BucketUpperBound(i);
  }
  return BucketUpperBound(kNumBuckets - 1);
}

void Histogram::AppendSamples(std::string* out) const {
  // Prometheus histograms need cumulative buckets at fixed boundaries. We
  // publish a 1-2-5 ladder from 1us to 10s (in seconds) computed from the
  // fine-grained internal buckets.
  static const double kBoundsSeconds[] = {
      1e-6, 2e-6, 5e-6, 1e-5, 2e-5, 5e-5, 1e-4, 2e-4, 5e-4, 1e-3, 2e-3,
      5e-3, 1e-2, 2e-2, 5e-2, 0.1,  0.2,  0.5,  1.0,  2.0,  5.0,  10.0};
  uint64_t count;
  uint64_t sum;
  const std::vector<uint64_t> b = Snapshot(&count, &sum);
  uint64_t total = 0;
  for (uint64_t v : b) total += v;

  int bucket = 0;
  uint64_t cumulative = 0;
  for (double bound : kBoundsSeconds) {
    const auto bound_ns = static_cast<uint64_t>(bound * 1e9);
    while (bucket < kNumBuckets && BucketUpperBound(bucket) <= bound_ns) {
      cumulative += b[static_cast<size_t>(bucket)];
      bucket++;
    }
    *out += name() + "_bucket" +
            RenderLabels("le=\"" + FormatDouble(bound) + "\"") + " " +
            std::to_string(cumulative) + "\n";
  }
  *out += name() + "_bucket" + RenderLabels("le=\"+Inf\"") + " " +
          std::to_string(total) + "\n";
  *out += name() + "_sum" + RenderLabels() + " " +
          FormatDouble(static_cast<double>(sum) / 1e9) + "\n";
  *out += name() + "_count" + RenderLabels() + " " + std::to_string(total) +
          "\n";
}

// ---- Registry -------------------------------------------------------------------

MetricHandle::~MetricHandle() { registry_->Unregister(name_, key_); }

template <typename T, typename... Args>
T* MetricsRegistry::GetOrCreate(const std::string& name,
                                const std::string& help,
                                const MetricLabels& labels, Metric::Type type,
                                Args&&... args) {
  std::lock_guard<std::mutex> l(mu_);
  Family& fam = families_[name];
  if (fam.series.empty()) {
    fam.help = help;
    fam.type = type;
  }
  const std::string key = LabelKey(labels);
  auto it = fam.series.find(key);
  if (it != fam.series.end()) {
    // Same name registered with a different type is a programming error;
    // dynamic_cast turns it into a nullptr the caller will trip on in tests.
    return dynamic_cast<T*>(it->second.get());
  }
  auto metric = std::make_unique<T>(name, labels, std::forward<Args>(args)...);
  T* raw = metric.get();
  fam.series.emplace(key, std::move(metric));
  return raw;
}

Counter* MetricsRegistry::GetCounter(const std::string& name,
                                     const std::string& help,
                                     const MetricLabels& labels) {
  return GetOrCreate<Counter>(name, help, labels, Metric::Type::kCounter);
}

Gauge* MetricsRegistry::GetGauge(const std::string& name,
                                 const std::string& help,
                                 const MetricLabels& labels) {
  return GetOrCreate<Gauge>(name, help, labels, Metric::Type::kGauge);
}

Histogram* MetricsRegistry::GetHistogram(const std::string& name,
                                         const std::string& help,
                                         const MetricLabels& labels) {
  return GetOrCreate<Histogram>(name, help, labels, Metric::Type::kHistogram);
}

std::unique_ptr<MetricHandle> MetricsRegistry::RegisterCallbackGauge(
    const std::string& name, const std::string& help,
    const MetricLabels& labels, std::function<double()> fn) {
  {
    std::lock_guard<std::mutex> l(mu_);
    Family& fam = families_[name];
    if (fam.series.empty()) {
      fam.help = help;
      fam.type = Metric::Type::kGauge;
    }
    fam.series[LabelKey(labels)] =
        std::make_unique<Gauge>(name, labels, std::move(fn));
  }
  return std::unique_ptr<MetricHandle>(
      new MetricHandle(this, name, LabelKey(labels)));
}

void MetricsRegistry::Unregister(const std::string& name,
                                 const std::string& key) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = families_.find(name);
  if (it == families_.end()) return;
  it->second.series.erase(key);
  if (it->second.series.empty()) families_.erase(it);
}

std::string MetricsRegistry::ExportPrometheus() const {
  std::lock_guard<std::mutex> l(mu_);
  std::string out;
  for (const auto& [name, fam] : families_) {
    if (fam.series.empty()) continue;
    const char* type = "counter";
    if (fam.type == Metric::Type::kGauge) type = "gauge";
    if (fam.type == Metric::Type::kHistogram) type = "histogram";
    out += "# HELP " + name + " " + fam.help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    for (const auto& [key, metric] : fam.series) {
      metric->AppendSamples(&out);
    }
  }
  return out;
}

bool MetricsRegistry::GetValue(const std::string& name,
                               const MetricLabels& labels,
                               double* value) const {
  std::lock_guard<std::mutex> l(mu_);
  auto fit = families_.find(name);
  if (fit == families_.end()) return false;
  auto sit = fit->second.series.find(LabelKey(labels));
  if (sit == fit->second.series.end()) return false;
  const Metric* m = sit->second.get();
  if (const auto* c = dynamic_cast<const Counter*>(m)) {
    *value = static_cast<double>(c->Value());
    return true;
  }
  if (const auto* g = dynamic_cast<const Gauge*>(m)) {
    *value = g->Value();
    return true;
  }
  if (const auto* h = dynamic_cast<const Histogram*>(m)) {
    *value = static_cast<double>(h->Count());
    return true;
  }
  return false;
}

}  // namespace lsmkv
