// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "ycsb.h"

#include <cmath>

namespace lsmkv::bench {

const char* OpName(OpType op) {
  switch (op) {
    case OpType::kRead: return "read";
    case OpType::kUpdate: return "update";
    case OpType::kInsert: return "insert";
    case OpType::kReadModifyWrite: return "rmw";
    default: return "?";
  }
}

bool GetWorkload(char name, Workload* w) {
  *w = Workload();
  w->name = name;
  switch (name) {
    case 'A':
      w->read = 0.5; w->update = 0.5;
      w->description = "50% read / 50% update, zipfian (session store)";
      return true;
    case 'B':
      w->read = 0.95; w->update = 0.05;
      w->description = "95% read / 5% update, zipfian (photo tagging)";
      return true;
    case 'C':
      w->read = 1.0; w->update = 0.0;
      w->description = "100% read, zipfian (user profile cache)";
      return true;
    case 'D':
      w->read = 0.95; w->update = 0.0; w->insert = 0.05;
      w->default_distribution = "latest";
      w->description = "95% read / 5% insert, read latest (status updates)";
      return true;
    case 'F':
      w->read = 0.5; w->update = 0.0; w->rmw = 0.5;
      w->description = "50% read / 50% read-modify-write, zipfian (user database)";
      return true;
    default:
      return false;
  }
}

uint64_t FnvHash64(uint64_t v) {
  // FNV-1a over the 8 bytes of v, as YCSB's Utils.fnvhash64.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (int i = 0; i < 8; i++) {
    const uint64_t octet = v & 0xff;
    v >>= 8;
    hash ^= octet;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string YcsbKey(uint64_t i) { return "user" + std::to_string(FnvHash64(i)); }

double ZipfianGenerator::Zeta(uint64_t from, uint64_t to, double theta,
                              double initial) {
  double sum = initial;
  for (uint64_t i = from; i < to; i++) {
    sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
  }
  return sum;
}

ZipfianGenerator::ZipfianGenerator(uint64_t n, double theta)
    : n_(n == 0 ? 1 : n), theta_(theta) {
  alpha_ = 1.0 / (1.0 - theta_);
  zeta2_ = Zeta(0, 2, theta_, 0.0);
  zetan_ = Zeta(0, n_, theta_, 0.0);
  eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
         (1.0 - zeta2_ / zetan_);
}

void ZipfianGenerator::Resize(uint64_t n) {
  if (n <= n_) return;
  zetan_ = Zeta(n_, n, theta_, zetan_);
  n_ = n;
  eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
         (1.0 - zeta2_ / zetan_);
}

uint64_t ZipfianGenerator::Next(Random* rnd) {
  const double u = rnd->NextDouble();
  const double uz = u * zetan_;
  if (uz < 1.0) return 0;
  if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
  const auto r = static_cast<uint64_t>(
      static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
  return r >= n_ ? n_ - 1 : r;
}

LatestChooser::LatestChooser(const std::atomic<uint64_t>* inserted, double theta)
    : inserted_(inserted), zipf_(inserted->load(), theta) {}

uint64_t LatestChooser::Next(Random* rnd) {
  const uint64_t n = inserted_->load(std::memory_order_relaxed);
  // Grow lazily; zeta is extended incrementally so this stays O(new items).
  if (n > zipf_.items() + zipf_.items() / 100) zipf_.Resize(n);
  const uint64_t offset = zipf_.Next(rnd);
  return offset >= n ? 0 : n - 1 - offset;
}

std::unique_ptr<KeyChooser> MakeChooser(const std::string& distribution,
                                        uint64_t records,
                                        const std::atomic<uint64_t>* inserted) {
  constexpr double kTheta = 0.99;  // YCSB's default skew
  if (distribution == "uniform") return std::make_unique<UniformChooser>(records);
  if (distribution == "latest") return std::make_unique<LatestChooser>(inserted, kTheta);
  return std::make_unique<ScrambledZipfianChooser>(records, kTheta);
}

}  // namespace lsmkv::bench
