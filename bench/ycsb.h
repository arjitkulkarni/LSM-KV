// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// YCSB workload definitions and key-choice distributions, following the
// reference implementation (brianfrankcooper/YCSB, core workloads).

#ifndef LSMKV_BENCH_YCSB_H_
#define LSMKV_BENCH_YCSB_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "util/random.h"

namespace lsmkv::bench {

enum class OpType { kRead = 0, kUpdate, kInsert, kReadModifyWrite, kCount };
const char* OpName(OpType op);

struct Workload {
  char name = 'A';
  double read = 0.5;
  double update = 0.5;
  double insert = 0.0;
  double rmw = 0.0;
  std::string default_distribution = "zipfian";
  std::string description;
};

// A: 50/50 read/update ("session store")
// B: 95/5 read/update ("photo tagging")
// C: 100% read ("user profile cache")
// D: 95/5 read/insert, reads skewed to the newest keys ("status updates")
// F: 50/50 read / read-modify-write ("user database")
bool GetWorkload(char name, Workload* w);

// The YCSB key for record number i: "user" + a 64-bit FNV hash, so that
// records inserted in order are scattered across the key space.
std::string YcsbKey(uint64_t i);
uint64_t FnvHash64(uint64_t v);

// Chooses a record number in [0, n). Not thread-safe; one per thread.
class KeyChooser {
 public:
  virtual ~KeyChooser() = default;
  virtual uint64_t Next(Random* rnd) = 0;
};

class UniformChooser final : public KeyChooser {
 public:
  explicit UniformChooser(uint64_t n) : n_(n) {}
  uint64_t Next(Random* rnd) override { return rnd->Uniform(n_); }

 private:
  uint64_t n_;
};

// Gray et al., "Quickly Generating Billion-Record Synthetic Databases"
// (SIGMOD '94), as used by YCSB's ZipfianGenerator. Item 0 is the most
// popular; P(i) ~ 1 / (i+1)^theta.
class ZipfianGenerator {
 public:
  ZipfianGenerator(uint64_t n, double theta);
  uint64_t Next(Random* rnd);
  // Grow the item count (workload D inserts), updating zeta incrementally.
  void Resize(uint64_t n);
  uint64_t items() const { return n_; }

 private:
  static double Zeta(uint64_t from, uint64_t to, double theta, double initial);
  uint64_t n_;
  double theta_;
  double alpha_;
  double zeta2_;
  double zetan_;
  double eta_;
};

// Zipfian popularity, but the popular items are scattered over the key
// space by hashing (YCSB's ScrambledZipfianGenerator): hot keys do not all
// sit in one SSTable block.
class ScrambledZipfianChooser final : public KeyChooser {
 public:
  ScrambledZipfianChooser(uint64_t n, double theta) : n_(n), zipf_(n, theta) {}
  uint64_t Next(Random* rnd) override { return FnvHash64(zipf_.Next(rnd)) % n_; }

 private:
  uint64_t n_;
  ZipfianGenerator zipf_;
};

// Skewed toward the most recently inserted record (workload D).
class LatestChooser final : public KeyChooser {
 public:
  LatestChooser(const std::atomic<uint64_t>* inserted, double theta);
  uint64_t Next(Random* rnd) override;

 private:
  const std::atomic<uint64_t>* inserted_;
  ZipfianGenerator zipf_;
};

std::unique_ptr<KeyChooser> MakeChooser(const std::string& distribution,
                                        uint64_t records,
                                        const std::atomic<uint64_t>* inserted);

}  // namespace lsmkv::bench

#endif  // LSMKV_BENCH_YCSB_H_
