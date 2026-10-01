# Benchmarks

Every number on this page was printed by the project's own harness and pasted
here by `bench/report.py`; nothing is typed by hand. The raw output (JSON per
run, including the exact command line) is in [`results/`](results/). To
regenerate all of it from scratch:

```bash
bench/run_all.sh            # full methodology, ~2.5 h on a laptop
QUICK=1 bench/run_all.sh    # 10-minute smoke version
```

## How the numbers were measured

This is the part that has to survive questioning.

| Rule | Why |
|---|---|
| **Fresh load per repetition**, then `WaitForCompactions()` | Every rep starts from the same settled LSM shape instead of inheriting the previous rep's compaction debt. |
| **10 s warmup, then 60 s fixed-duration steady state** | Warmup fills the block cache and lets compaction reach equilibrium. Fixed *duration*, not fixed op count, so a slow configuration cannot finish early and look fast. |
| **Per-thread HDR-style histograms** (7 sub-bucket bits: ≤ 0.78% error, reported value is the bucket's upper edge) | p50/p99/p99.9 without storing samples, and without cross-thread contention in the load generator itself. |
| **Open-loop mode** (`--rate`) alongside closed loop | Avoids *coordinated omission*: latency is measured from when a request was *scheduled*, not when the generator got around to sending it. See [the open-loop section](#coordinated-omission-closed-vs-open-loop). |
| **3 repetitions, median of each metric** | One outlier rep (a background compaction landing in the window) cannot move the headline. |
| **Every comparison is interleaved** (`bench/ab.py`) | Before/after, 1 vs 8 shards and LSM-KV vs LevelDB run one rep at a time in alternating order (A B, B A, A B), so slow drift in the machine hits both sides equally. See [measurement lessons](#measurement-lessons). |
| **Environment logged with every result** | CPU model, core count, OS build, compiler, flags, UTC time and the literal command line are embedded in each JSON file. |
| **CPU governor / page cache** | On Linux, `run_all.sh` pins the governor to `performance` and drops the page cache when it has `sudo`. The results below come from a Windows laptop, where neither applies: the power plan was *Balanced*, and all runs are warm-cache (a fresh DB is written, then read back while its files are still cached). |
| **The machine is kept awake** | Benchmark processes call `SetThreadExecutionState` so a laptop cannot suspend mid-measurement. |

Workloads follow YCSB's core definitions: **A** 50/50 read/update, **B** 95/5
read/update, **C** read-only, **D** 95/5 read/insert with reads skewed to the
newest keys, **F** 50/50 read / read-modify-write. Keys are `"user" +
FNV-64(i)`; the Zipfian generator is YCSB's (θ = 0.99, scrambled so hot keys
are spread over the key space). Values are 100 bytes. Unless stated
otherwise: 1,000,000 records, 8 client threads, 4 MiB memtable, 8 memtable
shards, 10-bit Bloom filters, 8 MiB block cache, `sync=false` (acked once the
WAL record is in the OS page cache).

### Environment

<!-- BEGIN:env -->
_run `bench/run_all.sh`_
<!-- END:env -->

---

## YCSB at 8 threads

<!-- BEGIN:ycsb -->
_run `bench/run_all.sh`_
<!-- END:ycsb -->

The *Reps min–max* column is there on purpose. This is a laptop on a
Balanced power plan with turbo boost and thermal limits, so individual
repetitions of the same configuration vary noticeably. That is why the
headline is the median of three and not the best run.

Reads are served from the memtable, the block cache or one positional read
per SSTable touched; updates cost one WAL append (amortized over the commit
group) and one memtable insert. The update tail (p99.9) is where background
flushes and compactions show up.

**Why read-only C is slower than read-mostly B:** in A, B and F the
Zipfian-hot keys keep being rewritten, so their newest versions sit in the
memtable and a read never leaves memory. In C nothing is written after the
load, so every read goes to the SSTables: Bloom filter, index, then the 8 MiB
block cache, which holds only a slice of the ~124 MiB dataset, or a
positional read plus CRC check. This is the LSM trade-off in miniature:
recently written data is the cheapest to read. The uniform rows show the same
effect harder, because nothing is hot enough to stay cached.

## Component baselines (Day 1)

Measured before any tuning, so later improvement claims have a baseline.

<!-- BEGIN:micro -->
_run `bench/run_all.sh`_
<!-- END:micro -->

## Bloom filter sweep

What each extra bit of filter per key buys. Filter-level false positives are
measured on 1,000,000 keys that were never inserted. The engine-level columns
come from a DB with a realistic multi-level shape and the **block cache
disabled**, so every data-block fetch is a real file read counted by
`lsmkv_sst_block_reads_total`.

<!-- BEGIN:bloom -->
_run `bench/run_all.sh`_
<!-- END:bloom -->

Without a filter, a lookup for an absent key pays one block read for every
SSTable whose key range covers it (every L0 file plus one per deeper level).
With 10 bits/key that drops to roughly `tables × FP rate`. 10 bits/key is
the default because going from 10 to 14 bits costs 40% more filter memory to
save a fraction of a read per miss.

## Performance pass: finding and fixing the top bottleneck

### The symptom

A first scaling run showed write throughput **falling** as client threads
were added, while memtable sharding (1 vs 8 locks) changed almost nothing.
So the memtable mutex, the obvious suspect, was not the bottleneck.

### The diagnosis (from the engine's own metrics)

Two histograms were added to the write path: time a writer waits in the
commit queue, and time the group leader spends appending to the WAL. The
harness output that decided the fix is kept verbatim in
[`results/perf/00-diagnosis-before-fix.txt`](results/perf/00-diagnosis-before-fix.txt):

```
load, 8 threads:  147,725 ops/s | mean group 3.44 writers
                  queue wait p50 41.0 us | WAL append p50 3.5 us
```

The serialized work per commit group (the WAL append) took ~3.5 µs, but a
group cycle took 3.44 writers ÷ 147.7k ops/s ≈ **23 µs**. The other ~20 µs
was hand-off: every follower and the next leader slept on a condition
variable, paid an OS wake-up, then re-acquired the DB mutex one by one.

### The fix (exactly one change)

Queued writers now wait on an atomic per-writer state with a three-phase
strategy: spin ~200 `pause` instructions, then `yield` for up to 100 µs, then
block on the condition variable. Leadership and completion are published
with a release-store under the DB mutex, so a sleeping writer can never miss
its wake-up. `Options::adaptive_write_wait = false` restores the old
behaviour, which is how the "before" column is reproduced.

A use-after-free was caught while writing this: the leader originally read
`writer->blocked` *after* publishing `kDone`. A spinning follower can
observe `kDone`, return and destroy its stack-allocated `Writer` in that
window. The leader now reads `blocked` first; the reasoning is in
`DBImpl::SetWriterState`.

### Result: write-only load

<!-- BEGIN:scaling-load -->
_run `bench/run_all.sh`_
<!-- END:scaling-load -->

### Result: YCSB A (50% updates)

<!-- BEGIN:scaling-A -->
_run `bench/run_all.sh`_
<!-- END:scaling-A -->

### Memtable sharding, for the record

<!-- BEGIN:shards -->
_run `bench/run_all.sh`_
<!-- END:shards -->

### What would be next

The fix leaves the commit pipeline serialized on one leader at a time.
Past 8 threads, WAL-append time itself grows (more bytes per group, plus
cache-line traffic on shared counters). The next steps would be pipelined
writes (next group's WAL append overlaps this group's memtable inserts) and
per-core metric counters. Per the one-change rule they are not done here.
On Linux, `bench/profile.sh` records `perf` + a FlameGraph for workload A at
8 threads, to confirm with a sampling profiler what these metrics suggest.

## Coordinated omission: closed vs open loop

A closed-loop generator sends the next request only after the previous one
returns. When the engine stalls for 50 ms, that thread sends nothing for
50 ms, so the requests a real client *would* have sent during the stall,
each of which would have waited, never get recorded. The histogram ends up
describing a server that never stalled. Open-loop mode fixes the send times
in advance (`--rate`) and measures each request from its scheduled start, so
queueing behind a stall is charged to the requests that queued.

The rows below come from workload A. The first row is a closed-loop
calibration run made immediately before the open-loop runs, and the offered
rates are percentages of it. "Service time" is measured from the actual
send, i.e. what a closed-loop harness would have reported for the same
requests. The 110% row is an overload on purpose: service time still looks
healthy there, while real latency is a queue that grows for the whole
window.

<!-- BEGIN:openloop -->
_run `bench/run_all.sh`_
<!-- END:openloop -->

## Observability: reproducing a write-stall regression

The memtable was deliberately shrunk from 4 MiB to 64 KiB. Each flush then
produces a tiny L0 file, L0 hits the slowdown and stop triggers, and writers
stall while compaction catches up. The harness sampled the engine's metrics
once per second (`--timeseries`); the backlog and stall series are plotted on
the project site and can be watched live in the Grafana dashboard
(`observability/`).

<!-- BEGIN:stall -->
_run `bench/run_all.sh`_
<!-- END:stall -->

## Crash matrix

`tools/crash_matrix.py` runs `lsmkv-stress` under continuous load, logs
every write the engine *acknowledged*, and kills the process at a random
moment: SIGKILL (TerminateProcess on Windows), SIGKILL plus a torn WAL tail
(partial record, garbage or zero-fill), or an injected fsync failure. After
each crash it reopens the DB and checks that every acknowledged write is
present with a value at least as new as the last ack, that every stored
value passes its embedded checksum, and that a full scan is sorted and
clean. The DB directory persists across iterations, so later crashes hit a
DB with many SSTables and compactions in flight.

<!-- BEGIN:crash -->
_run `bench/run_all.sh`_
<!-- END:crash -->

SIGKILL tests *process-crash* durability: the killed process loses its
user-space buffers but the OS page cache survives. *Power-loss* durability
(writes acknowledged with `sync=true` surviving the loss of everything not
fsync'd) is tested separately by `FaultInjectionTest.SyncedWritesSurvivePowerLoss`,
which truncates every file to its last-synced length and reopens, across
flushes and compactions.

## Measurement lessons

Two things went wrong while producing these numbers. Both are kept here
because the fixes are part of the methodology.

**1. The machine drifted, so sequential A/B runs were unfair.** The same
configuration (YCSB-A, Zipfian, 8 threads, fix enabled) measured
**783,000 ops/s** in the first hour of the campaign, with three reps within
0.3% of each other, and **~340,000 ops/s** an hour later, with reps spread
by ±15%. The engine's own write-path metrics showed per-operation latency
barely moving (p50 8.5 → 9–11 µs) while throughput halved, and one rep's
load phase ran 9× slower than normal. That pattern is the benchmark threads
being starved by something else on the laptop (an IDE indexer was the
largest CPU consumer at the time), not the engine slowing down. The first
scaling sweep had run each variant as a block, minutes apart, so it could
not tell drift from effect. Every comparison on this page was therefore
re-measured with `bench/ab.py`, which interleaves the variants rep by rep in
alternating order. Absolute numbers from different sections were measured at
different times and should not be compared with each other; the
within-experiment ratios are the claims.

**2. The open-loop generator hid overload.** The first open-loop runs set
their rates from the throughput measured an hour earlier, when the machine
was quiet. By the time they ran, even "50%" exceeded the machine's capacity.
The generator then kept draining its backlog after the measurement window
closed, so it reported *achieved = offered* next to latencies of 6–70
**seconds**. The latencies were honest (a queue growing without bound); the
throughput was not. The fix: stop issuing at the end of the window, count
only operations that complete inside it, flag `overloaded` when completions
fall below 95% of the offered rate, and calibrate the maximum with a
closed-loop run immediately before the open-loop runs. A deliberate 110% row
now shows what overload looks like.

## Credibility anchor: LevelDB 1.23, same harness, same box

LevelDB is built from source (`-DLSMKV_WITH_LEVELDB=ON`) and driven through
the same `lsmkv::DB` interface (`bench/leveldb_adapter.h`), with matching
settings: 4 MiB write buffer, 10-bit Bloom filters, 8 MiB block cache,
compression off (LSM-KV does not compress).

<!-- BEGIN:leveldb -->
_run `bench/run_all.sh`_
<!-- END:leveldb -->

Where LSM-KV wins and why:

* **Write-heavy workloads with many client threads**: LSM-KV's queued writers
  spin briefly instead of sleeping, and they insert into the memtable in
  parallel after group commit. LevelDB's leader applies the whole group to its
  single-writer skiplist while every follower sleeps.

Where it loses and why:

* **Maturity of compaction.** LevelDB picks compaction inputs more carefully
  (seek-triggered compactions, expanding inputs within the grandparent
  budget). LSM-KV always takes all of L0 into an L0→L1 compaction, which
  means larger, burstier compactions.
* **Read path allocation.** LevelDB's read path avoids a few allocations
  that LSM-KV still makes (the `std::function` callback into the table and a
  `shared_ptr` copy of the SuperVersion per `Get`).

The ratio column is the honest summary. The exact figures depend on this
machine and are reproducible with `bench/run_all.sh`.
