# LSM-KV

**A crash-tested, observable LSM-tree key-value storage engine in C++17.**

Write-ahead log with CRC32C and torn-write detection · lock-free-read
skiplist memtable, hash-sharded · SSTables with prefix-compressed blocks,
Bloom filters and an O(1) LRU block cache · leveled compaction with an atomic
MANIFEST · group commit · Prometheus metrics and JSON event logs · a YCSB
harness with open-loop load generation · a SIGKILL crash matrix.

```cpp
#include "lsmkv/db.h"

lsmkv::Options options;                        // every field has a default
std::unique_ptr<lsmkv::DB> db;
lsmkv::Status s = lsmkv::DB::Open(options, "/tmp/demo", &db);

s = db->Put(lsmkv::WriteOptions(), "user:42", "ada");
std::string value;
s = db->Get(lsmkv::ReadOptions(), "user:42", &value);   // "ada"

auto it = db->NewIterator(lsmkv::ReadOptions());        // std::unique_ptr
for (it->Seek("user:"); it->Valid() && it->key().starts_with("user:"); it->Next()) { /* ... */ }
```

No call site ever writes `new` or `delete`: `DB::Open` hands back a
`std::unique_ptr<DB>`, iterators come back as `std::unique_ptr<Iterator>`, and
everything they read (memtables, table files, cached blocks) is pinned by
`shared_ptr`. That is why an iterator stays valid while compaction deletes the
files underneath it.

## Numbers

Every figure below is printed by the project's own harness
([`BENCHMARKS.md`](BENCHMARKS.md) has the methodology, the raw JSON is in
[`results/`](results/)). Headline claims are rounded down.

<!-- BEGIN:headline -->
_run `bench/run_all.sh`_
<!-- END:headline -->

<p>
  <img src="results/plots/scaling-load.svg" alt="Write throughput vs client threads, before and after the commit-queue fix" width="49%">
  <img src="results/plots/bloom-sweep.svg" alt="SSTable block reads per negative lookup by Bloom bits per key" width="49%">
</p>

The project site ([`site/index.html`](site/index.html)) turns the same
results into an interactive tour: real `lsmkv-dump` output, step-through
flows of the write, read and crash paths, and every chart.

## Architecture

```
                  Put / Delete / Write(batch)                   Get / NewIterator
                            │                                          │
                            ▼                                          ▼
                 ┌──────────────────────┐                 ┌───────────────────────┐
  writers ──────▶│  commit queue (mu_)  │                 │ SuperVersion snapshot │
                 │  leader builds group │                 │ (shared_mutex, 1 copy)│
                 └──────────┬───────────┘                 └───────────┬───────────┘
                            │ 1 WAL record / group                    │ no lock held below
                            ▼                                         ▼
  ┌───────────────────────────────────────────┐        ┌───────────────────────────────┐
  │ WAL  000123.log                           │        │ active memtable  → imm memtable│
  │ 32 KiB blocks · CRC32C · FULL/FIRST/MID/LAST│       │ → L0 (newest file first)       │
  └───────────────────────────────────────────┘        │ → L1 … L6 (one file per level) │
                            │ then, in parallel        │ per table: Bloom → index → block│
                            ▼                          └───────────────────────────────┘
  ┌──────────────────────────────────────────────────────┐
  │ MemTable: 8 hash shards × (skiplist + arena + mutex)  │   lock-free reads
  └──────────────────────────┬───────────────────────────┘
          4 MiB full? ──────▶ │ switch: seal WAL (fsync), new WAL, imm = mem
                             ▼
             background thread: flush imm → L0 SSTable, then leveled compaction
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ L0: overlapping files (≤ 4 before compaction, 8 slows writes, 12 stops)   │
  │ L1: 10 MiB ─ L2: 100 MiB ─ L3: 1 GiB … sorted, non-overlapping files      │
  │ MANIFEST: log of VersionEdits (add/remove files) — one fsync'd record per │
  │ flush or compaction, so the live-file set changes atomically              │
  └──────────────────────────────────────────────────────────────────────────┘
```

### The object-oriented design

The public contract lives in [`include/lsmkv/`](include/lsmkv/) and was
written before any implementation:

| Type | Role | Pattern |
|---|---|---|
| `DB` | abstract engine: `Put/Get/Delete/Write/NewIterator` | interface; `DBImpl` and the LevelDB adapter both implement it, so the benchmark drives either engine through one code path |
| `Iterator` | forward cursor | one interface for memtable, block, two-level, k-way merge and user-facing cursors, so flush, compaction and scans are written once |
| `Comparator`, `FilterPolicy`, `Cache` | key order, SSTable filter, block cache | Strategy objects, swappable per DB |
| `Env`, `WritableFile`, `RandomAccessFile` | the only door to the OS | POSIX and Win32 implementations; `FaultInjectionEnv` is a Decorator that fails fsyncs or drops unsynced data for tests |
| `InternalKeyComparator`, `InternalFilterPolicy` | versioned keys | Decorators over the user's comparator / filter |
| `Status`, `Slice`, `Options`, `WriteBatch` | values | rule of zero; `Status` is `[[nodiscard]]`, so ignoring an I/O error is a compiler warning |
| `Logger`, `MetricsRegistry`, `Metric` → `Counter`/`Gauge`/`Histogram` | observability | small class hierarchy; callback gauges are unregistered by an RAII `MetricHandle` |

### Write path

1. The writer takes the DB mutex, joins the commit queue and waits
   (spin → yield → block) until it is either the queue head (leader) or
   finished by a leader.
2. **Leader**: makes room (may switch memtable + WAL, may stall on L0 or on a
   pending flush), claims up to 1 MiB of queued batches, assigns sequence
   numbers in queue order, releases the mutex.
3. **Leader, no lock held**: appends one WAL record for the whole group,
   plus one `fsync` if any member asked for `sync`.
4. **Leader**: re-takes the mutex, publishes the last sequence, marks
   followers done, hands leadership to the next queued writer.
5. **Every writer in parallel**: inserts its own batch into the memtable,
   taking only the owning shard's mutex, then returns (the ack).

Locks, in acquisition order: **DB mutex → SuperVersion `shared_mutex` → leaf
locks** (memtable shard, cache shard, table cache, metrics registry; never
held while taking another lock). The invariant that makes step 5 safe: a
memtable is never switched while a writer whose record is already in the
WAL has not finished inserting into it. The switch waits for
`pending_inserts_ == 0`, and a Dekker-style `seq_cst` handshake guarantees
the last inserter wakes it.

### Read path

`Get` takes the `shared_mutex` in shared mode just long enough to copy one
`shared_ptr<SuperVersion>` holding the active memtable, the immutable
memtable and the current `Version`. Everything after that runs without DB
locks: active memtable → immutable memtable → L0 files newest-first → one
binary-searched file per deeper level. Per SSTable: Bloom filter → binary
search of the pinned index block → one positional read of a 4 KiB block
(or a cache hit) → binary search over restart points → a scan of at most 16
prefix-compressed entries.

### Durability and recovery

* **Ack semantics.** With `sync=false`, a write is acked once its WAL record
  is in the OS page cache, so it survives a process crash. With `sync=true`,
  the WAL is fsync'd first, so it survives power loss. Group commit amortizes
  one fsync across every writer in the group.
* **Torn tail vs. corruption.** The reader tells a torn final write (the
  file just ends mid-record: un-acked, safe to drop) from real damage (a CRC
  mismatch or an impossible header). After damage it resynchronizes at the
  next 32 KiB block and keeps reading. If a *valid* record follows the damage,
  the damage cannot be a torn tail: Open refuses rather than silently
  dropping acknowledged writes (`kTolerateCorruptedTail`, the default). The
  alternatives are `kPointInTimeRecovery` (open with the consistent prefix)
  and `kAbsoluteConsistency` (reject any damage).
* **Why only the newest WAL can be torn.** A WAL is fsync'd when it is
  retired at a memtable switch. So damage in an older WAL is always
  corruption, never a crash artifact.
* **Tail truncation.** Recovery cuts the torn bytes off the newest WAL
  before appending to it again. Otherwise new records would land after the
  garbage, and the next recovery would correctly flag "damage followed by
  valid data".
* **Atomic metadata.** New SSTables are fsync'd and re-opened (verified)
  before the MANIFEST record that references them is appended and
  fsync'd. Compaction inputs are deleted only after that record is durable.
  `CURRENT` is replaced by write-temp → fsync → rename → fsync-dir.
* **Failed fsync is fatal.** After an fsync error the engine stops acking
  writes (reads keep working). Retrying the fsync is unsafe: the kernel may
  already have dropped the dirty pages (PostgreSQL's "fsyncgate").

### Compaction

Leveled: L0 is scored by file count (every L0 file costs every read a
probe), deeper levels by bytes against a 10× geometric target. A single
file with nothing to merge below it is moved by editing metadata only. The
k-way merge runs over a binary min-heap of child iterators. Shadowed
versions are dropped (there are no snapshots), and a tombstone is dropped
only when no deeper level can hold an older value for its key. A pending
memtable flush preempts a running compaction, because writers may be
stalled on it. Write amplification is tracked continuously
(`lsmkv.write-amplification`, `lsmkv_write_amplification`).

### Observability

`GetProperty("lsmkv.prometheus")` or a tiny built-in HTTP server
(`/metrics`, text exposition format 0.0.4) exports:
ops/s by type, latency histograms, block-cache hit ratio, compaction
backlog (estimated pending bytes), write-stall count and duration by cause,
WAL fsync latency, group-commit size, commit-queue wait vs. WAL-append time,
bytes read/written per level, files and bytes per level, Bloom filter
usefulness, and recovery time and bytes replayed. Every flush, compaction,
stall, recovery decision and background error is also a structured JSON line
in `<db>/LOG`. A ready-made Grafana dashboard is in
[`observability/`](observability/) (`docker compose up`).

## Design decisions (and what they cost)

**LSM vs. B-tree.** An LSM tree turns random writes into sequential ones
(WAL append + memtable insert) at the cost of *read amplification* (a miss
may probe several tables, which Bloom filters mitigate) and *write
amplification* (compaction rewrites data once per level, roughly
`multiplier × levels` in the worst case). This engine tracks WA live
instead of guessing it.

**Skiplist over a red-black tree.** An insert changes at most 12 forward
pointers, each published with one release-store after the node is fully
built. So readers follow pointers with acquire-loads and take no lock at
all. A red-black tree rebalances with rotations that touch several nodes at
once, which rules out lock-free readers this cheaply. With p = 1/4 the
expected search is `(1/p)·log_{1/p} n = 2 log₂ n` comparisons, about what a
balanced BST does, with only 1.33 pointers per node.

**Sharded locks over lock-free.** Lock-free *writers* (CAS-linked skiplist
inserts, epoch reclamation) are where experienced engineers get ABA and
memory reclamation wrong, and a clean TSan run does not prove
lock-freedom. Readers are already lock-free here; writers take one
fine-grained shard lock. The benchmark shows the memtable lock was never the
bottleneck anyway: the commit-queue hand-off was, and it was fixed without
removing a lock.

**`shared_ptr` versioning over manual reference counts.** `Version`,
`MemTable`, `Table` and cached blocks are reference-counted by
`shared_ptr`. A reader pins exactly what it needs, and a retired file is
deleted only once no live `Version` references it.

## Testing

* **124 GoogleTest cases**, including:
  * a **1,000,000-operation differential test** of the memtable against
    `std::map` (Put/Get/Delete/Scan), with 1 and 8 shards;
  * an end-to-end DB differential test with reopens, flushes and full
    compactions interleaved;
  * concurrency tests with writers, readers and scanners racing flushes and
    compactions;
  * **recovery tests** for every damage type: truncated record, torn
    fragmented record, zero-filled tail, garbage tail, corruption followed by
    valid data (refused), point-in-time and absolute-consistency modes, a
    corrupt SSTable block (an error, never a wrong value), a missing
    SSTable, a torn MANIFEST, and repeated crash-recover cycles;
  * **power-loss simulation** (`FaultInjectionEnv` drops every unsynced byte:
    all `sync=true` writes survive, across flushes and compactions);
  * **fsync failure** (the DB goes read-only instead of acking into the void).
* **Crash matrix**: 200 SIGKILL / torn-tail / fsync-failure iterations under
  load, with every acknowledged write verified after restart. It runs
  nightly in CI.
* **Sanitizers**: ASan + UBSan and a separate ThreadSanitizer job in CI
  (gcc-14 and clang-18). Locally: GCC 14 `-Wall -Wextra -Wpedantic
  -Wshadow -Wold-style-cast …` with zero warnings, Clang 19 `-Werror`, and
  UBSan in trap mode.

## Build and run

Requirements: CMake ≥ 3.20, a C++17 compiler (GCC ≥ 10, Clang ≥ 12, or
MinGW-w64 GCC), Ninja recommended. GoogleTest is fetched automatically.

```bash
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/rel
ctest --test-dir build/rel --output-on-failure -j 8

# a quick benchmark
./build/rel/lsmkv-bench --workload=A --threads=8 --records=1000000 --reps=3

# look inside the files
./build/rel/lsmkv-dump db  /tmp/lsmkv-demo
./build/rel/lsmkv-dump sst /tmp/lsmkv-demo/000012.sst
./build/rel/lsmkv-dump wal /tmp/lsmkv-demo/000011.log

# crash it 200 times
python3 tools/crash_matrix.py --build build/rel --iterations 200

# regenerate every number in BENCHMARKS.md and results/
bench/run_all.sh
```

## Repository layout

```
include/lsmkv/   public API: db, options, status, slice, iterator, comparator,
                 filter_policy, cache, env, write_batch, logger, metrics
src/db/          DBImpl (write/read path, recovery, compaction), Version/VersionSet,
                 MANIFEST edits, internal keys, DB iterator, table cache
src/memtable/    lock-free-read skiplist, sharded memtable
src/wal/         log format, writer, reader (torn-tail vs corruption)
src/table/       block builder/reader, SSTable builder/reader, two-level and
                 k-way merging iterators
src/util/        arena, CRC32C, Bloom, LRU cache, histograms, metrics, JSON logger,
                 POSIX / Win32 Env, FaultInjectionEnv
src/server/      /metrics HTTP endpoint
tools/           lsmkv-dump, lsmkv-stress, crash_matrix.py
bench/           YCSB harness, micro-benchmarks, Bloom sweep, LevelDB adapter,
                 run_all.sh, report.py, profile.sh
tests/           GoogleTest suite
observability/   Prometheus + Grafana (dashboard JSON, docker compose)
site/            the project website (static; reads site/data/results.js)
docs/            interview notes
```

## Limitations

Deliberate, and documented rather than hidden:

* **Single node.** No replication or consensus.
* **No MVCC snapshots or transactions.** Iterators get a point-in-time view,
  but there is no user-facing snapshot handle. Because of that, compaction
  keeps only the newest version of each key.
* **Forward-only iterators.** No `Prev`/`SeekToLast`.
* **No range deletes, no compression, no column families.**
* **One background thread** runs flushes and compactions (flushes preempt
  compactions). There are no parallel subcompactions.
* **L0→L1 compaction takes every L0 file**, which is simple and clears L0 in
  one go but makes those compactions large.
* **Platforms**: Linux (primary; CI with sanitizers) and Windows (MinGW-w64,
  the machine the published numbers came from). macOS should work through
  the POSIX Env but is not in CI.
* **Crash testing** covers process crashes (SIGKILL) and simulated power loss
  via fault injection. It does not cover real power pulls or lying disks.

## License

MIT
