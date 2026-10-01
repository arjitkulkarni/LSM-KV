# Resume material

Every figure below was printed by this repository's harness on the machine
recorded in [`BENCHMARKS.md`](../BENCHMARKS.md), rounded **down**. Rerun
`bench/run_all.sh` before an interview and be ready to reproduce any of them.

## Bullets (pick 3–4)

- **Built LSM-KV, a log-structured key-value storage engine in modern C++17**
  (WAL, lock-free-read skiplist memtable, SSTables with Bloom filters and an
  LRU block cache, leveled compaction, atomic MANIFEST) behind an abstract,
  RAII-only object-oriented API; {{TESTS}} GoogleTest cases including a
  1M-operation differential test against `std::map`.
- **Proved crash safety with a 200-iteration crash matrix** (SIGKILL, torn WAL
  tails, injected fsync failures) under concurrent load:
  **0 of {{ACKED}} acknowledged writes lost**; recovery distinguishes torn
  final writes from mid-log corruption and refuses to drop acked data.
- **Diagnosed and fixed the engine's top write bottleneck with its own
  metrics**: commit-queue histograms showed writers waiting ~41 µs for a
  ~3.5 µs WAL append. An adaptive spin → yield → block hand-off raised write
  throughput **{{FIX}}× at 8 threads** ({{BASE8}} → {{FIX8}} ops/s), with the
  baseline kept reproducible behind a flag.
- **Instrumented the engine for operations at scale**: Prometheus
  `/metrics` (latency histograms, compaction backlog, write stalls, fsync
  latency, group-commit size), structured JSON event logs and a Grafana
  dashboard; reproduced a write-stall regression and caught it on the
  backlog gauge.
- **Built a YCSB-style benchmark harness with open-loop load generation**
  to avoid coordinated omission (HDR histograms, warmup + 60 s steady state,
  median of 3): **{{YCSBA}}+ ops/s on YCSB-A at 8 threads (p99 {{P99A}})**;
  benchmarked side by side against LevelDB 1.23 through the same interface,
  including the workloads where LevelDB wins.
- **Tuned read amplification with a Bloom filter sweep** (4–14 bits/key):
  10 bits/key cut SSTable block reads per missing-key lookup by
  **{{BLOOMSAVE}}%** at a measured {{BLOOMFP}}% false-positive rate.

## LaTeX block

The plan calls for enabling the commented LSM-KV block at the bottom of the
resume `.tex`, placing it **above CIFAR-10**, deleting NyayMitra to pay for
the space, and re-checking that `pdfinfo resume.pdf` still says `Pages: 1`.
If your template uses the common `\resumeProjectHeading` / `\resumeItem`
macros, this drops in:

```latex
\resumeProjectHeading
  {\textbf{LSM-KV} $|$ \emph{C++17, CMake, GoogleTest, Prometheus, Python}}{2026}
  \resumeItemListStart
    \resumeItem{Built a log-structured key-value storage engine (WAL, lock-free-read skiplist memtable, SSTables with Bloom filters, leveled compaction, atomic MANIFEST) behind an abstract RAII C++ API; {{TESTS}} tests incl.\ a 1M-op differential test vs.\ \texttt{std::map}}
    \resumeItem{Crash matrix of 200 SIGKILL / torn-WAL / fsync-failure runs under load: 0 of {{ACKED}} acknowledged writes lost; recovery tells torn tails from mid-log corruption}
    \resumeItem{Found the top write bottleneck with the engine's own metrics (41\,\textmu s queue wait vs.\ 3.5\,\textmu s WAL append); adaptive hand-off raised write throughput {{FIX}}$\times$ at 8 threads}
    \resumeItem{YCSB harness with open-loop load (no coordinated omission): {{YCSBA}}+ ops/s on YCSB-A, p99 {{P99A_TEX}}; compared against LevelDB 1.23 incl.\ where it wins; Prometheus metrics + Grafana}
  \resumeItemListEnd
```

## Rehearse

The three 90-second answers are in [`INTERVIEW.md`](INTERVIEW.md):

1. Draw the skiplist and derive expected O(log n).
2. Walk the write path end to end, naming every lock and its order.
3. Explain coordinated omission and why the open-loop mode exists.
