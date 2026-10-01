# Interview notes

Three answers to rehearse until each takes 90 seconds, then the follow-ups
they invite. Every number quoted here comes from [`BENCHMARKS.md`](../BENCHMARKS.md);
say "on my laptop" and offer to rerun `bench/run_all.sh`.

---

## 1. Draw the skiplist and derive expected O(log n)

**Draw:** four levels. Level 0 links every node; each node is promoted to
the next level with probability p = 1/4 (`kBranching = 4`), up to 12 levels
(`kMaxHeight`). The head node has all 12 levels.

**Search:** start at the top level of the head. Move right while the next
key is smaller than the target; otherwise drop down a level. At level 0 the
next node is the answer (first key ≥ target).

**Derivation (walk the search path backwards, Pugh 1990):** from the node
found at level 0, retrace the path in reverse. At each step, the node we are
on either has another level above it (probability p), so the backward path
goes *up*, or it doesn't (probability 1 − p), so the path came from the
*left*. The expected number of left-steps before each up-step is
(1 − p)/p, so each level costs 1/p steps in expectation. The list has about
L(n) = log₁/ₚ n levels that matter, because the expected number of nodes at
level i is n·pⁱ, which reaches 1 at i = log₁/ₚ n. Total:

```
E[cost] ≈ L(n)/p + (terms for the top levels) = (1/p)·log_{1/p} n + O(1)
        = 4 · log₄ n = 2 · log₂ n          (p = 1/4)
```

So search is O(log n), about as many comparisons as a balanced BST.

**Why p = 1/4 and not 1/2:** p = 1/2 gives the same 2·log₂ n expected
comparisons (2·log₂ n vs 4·log₄ n = 2·log₂ n) but costs 1/(1 − p) = 2
pointers per node instead of 1.33. The micro-benchmark measures ~19 bytes per
node for 8-byte keys (arena, including the key).

**Why 12 levels:** 4¹² ≈ 16.7M entries before the top level saturates. A
4 MiB memtable holds about 28k entries of 148 bytes (the measured bytes per
entry for a 16 B key and 100 B value), far below that.

**Why it matters here:** an insert changes at most 12 forward pointers. The
new node is fully built first, then each level's link is published with a
release-store, so readers following pointers with acquire-loads never see
a half-built node. That's why reads take no lock.

---

## 2. Walk the write path, naming every lock and in what order

"One `Put` from call to ack:"

1. **`Put` builds a one-record `WriteBatch`** and calls `Write`. No lock.
2. **Take the DB mutex (`mu_`), append to `writers_`, release.** If I'm the
   queue head, I'm the leader; otherwise I wait.
3. **Wait with no lock held.** I spin on my own atomic `state` (~200
   `pause`s), then `yield` for up to 100 µs, then sleep on my condvar. The
   sleep is under `mu_`, and the leader publishes state under `mu_`, so a
   wake-up can't be lost. (This was the fix: before it, every follower slept
   immediately, and at 8 threads writers waited ~41 µs for a ~3.5 µs WAL
   append.)
4. **Leader takes `mu_` again**, then:
   * `MakeRoomForWrite`: if the memtable is full, wait for in-flight inserts
     to drain (`pending_inserts_ == 0`), fsync-seal the old WAL, create a new
     WAL, move the memtable to `imm_`, and **take the SuperVersion
     `shared_mutex` exclusively** for a few instructions to publish the new
     {mem, imm, version}. This may also stall: 1 ms if L0 has ≥ 8 files, a
     hard wait if ≥ 12 or if the previous memtable is still flushing.
   * Claim up to 1 MiB of queued batches and assign sequence numbers in
     queue order. Release `mu_`.
5. **WAL append with no lock held**, one record for the whole group, plus
   one `fsync` if any member asked for `sync`. Only the leader touches the
   log, and there is exactly one leader.
6. **Leader takes `mu_`**, publishes `last_sequence`, marks each follower
   `kDone` (reading `blocked` *before* the release-store, because a spinning
   follower may return and destroy its stack-allocated `Writer` right after
   the store), makes the next writer leader, releases `mu_`.
7. **Every writer, in parallel, inserts its own batch** into the memtable,
   taking **only the shard mutex** its key hashes to. Sequence numbers make
   arrival order irrelevant: the skiplist orders by (key, sequence desc).
8. **Decrement `pending_inserts_`** (seq_cst). If it hits zero and a
   memtable switch is waiting, take `mu_` just to notify. Return: that's the
   ack.

**Order:** `mu_` → SuperVersion `shared_mutex` → leaf locks (memtable shard,
cache shard, table cache, metrics). Leaf locks are never held while
acquiring another lock, so no cycle is possible.

**The invariant to volunteer:** a memtable is never switched while a writer
whose record is already in the WAL hasn't finished inserting into it.
Otherwise the write could land in the new memtable while its WAL record sits
in the old log, which gets deleted after the old memtable flushes: a lost
acknowledged write.

---

## 3. Coordinated omission, and why the open-loop mode exists

**The problem:** a closed-loop load generator sends request n+1 only after
request n returns. Suppose the engine stalls for 50 ms (a memtable flush,
say). During those 50 ms each client thread sends nothing, so the requests
it *would* have sent, each of which would have waited up to 50 ms, never
exist, and never enter the histogram. The load generator has coordinated
with the system under test to omit exactly the samples that show the stall.
The p99 you publish describes a server that never stalls.

**The fix (`--rate`):** decide send times in advance, request i at
t₀ + i/rate per thread, and measure each request from its *scheduled*
start, not its actual send. If the engine stalls, the scheduled times keep
passing and the requests queue up. When they finally run, each is charged
its queueing time, which is exactly what a real user arriving at that rate
would have experienced.

**What the harness reports:** both numbers for every op. "Latency" is from
the scheduled start and "service time" is from the actual send; service
time is what a closed-loop tool would show. In `BENCHMARKS.md` the open-loop
p99 at 95% of max throughput is several times the service-time p99 for the
same requests. That gap is the stall time closed-loop tools leave out.

**Follow-up — "why not just use closed loop at max throughput?"** That
answers a different question (capacity). Latency SLOs are about a given
arrival rate, which is open-loop by nature. Real users do not wait for each
other.

---

## Likely follow-ups (short answers)

**Why an LSM tree, not a B-tree?** Random writes become one sequential WAL
append plus an in-memory insert, and the sort is paid later in background
compaction. The costs: read amplification (a miss may probe several tables,
which Bloom filters cut) and write amplification (each byte is rewritten
once per level). The engine reports WA live.

**How do you tell a torn write from corruption?** A crash can only tear the
*last* record in the WAL stream. The reader resyncs at the next 32 KiB block
after any damage. If a checksum-valid record follows, the damage isn't a
torn tail. By default Open then fails rather than drop acknowledged writes.
Only the newest WAL can hold a torn record, because a WAL is fsync'd when
it's retired.

**Why truncate the tail after recovery?** Otherwise new records are
appended after the garbage, and the next recovery sees "damage followed by
valid data", which it correctly treats as corruption.

**What happens when fsync fails?** The write returns an I/O error and the DB
becomes read-only (a background error). Retrying fsync is unsafe: Linux may
already have dropped the dirty pages and marked them clean (PostgreSQL's
"fsyncgate"). The fault-injection test asserts writes stop, reads continue,
and after a restart with a healthy disk every acked write is there.

**How can a reader iterate while compaction deletes files?** It pins a
`shared_ptr<SuperVersion>`, which pins a `Version`, which pins the
`FileMetaData` of every file in it. Garbage collection deletes a file only
when no live `Version` references it (tracked through a list of
`weak_ptr<Version>`). `DBTest.IteratorIsAPointInTimeView` flushes and fully
compacts the DB while an iterator is open, and checks the iterator still
reads exactly the view from its creation.

**How is the MANIFEST atomic?** A flush or compaction is one `VersionEdit`
(its adds and deletes), appended to the MANIFEST as one checksummed log
record and fsync'd. New SSTables are fsync'd before that; old ones are
deleted only after. A torn MANIFEST tail is an edit that never took effect,
and it's ignored safely.

**Why is L0 scored by file count and deeper levels by bytes?** L0 files
overlap, so every read probes every L0 file: the count hurts. Deeper levels
are disjoint, where size is what drives compaction cost.

**When are tombstones dropped?** When compaction's output level is the
deepest level that could hold older data for that key (`IsBaseLevelForKey`
checks the levels below). Dropping earlier would resurrect the old value.

**Memory per key?** The micro-benchmark measures ~148 B per memtable entry
for a 16 B key and 100 B value: 116 B of payload and about 32 B of overhead
(skiplist links, length prefixes, the 8-byte sequence/type tag).

**Why not lock-free writers?** A CAS-based skiplist insert plus safe memory
reclamation is exactly where experienced engineers ship ABA bugs, and a
clean TSan run doesn't prove lock-freedom. Readers are already lock-free.
The profile showed the memtable lock wasn't the bottleneck anyway.

**What's the next bottleneck?** The commit pipeline is still one leader at a
time. Past 8 threads the WAL append itself grows. Next: pipelined writes
(overlap group N+1's append with group N's memtable inserts) and per-core
metric counters. I deliberately stopped at one measured change.

**Where does LevelDB beat you, and why?** Read the "Credibility anchor" table
in `BENCHMARKS.md` and give the ratio honestly. LevelDB's compaction picking
is more mature (seek-triggered compactions, expanding inputs within the
grandparent budget). Mine takes all of L0 into one L0→L1 compaction, which is
simpler but burstier.

**Tell me about a time your measurements were wrong.** Two, both in
`BENCHMARKS.md` → "Measurement lessons". (1) The same config measured 783k
ops/s, then ~340k an hour later. Latency per op barely moved while
throughput halved, which means the threads were being starved of CPU (an IDE
indexer), not the engine regressing. My first before/after sweep had run
each variant as a block, so drift could masquerade as effect. I rewrote the
driver to interleave A/B reps in alternating order. (2) My open-loop
generator drained its backlog after the window, reporting achieved = offered
next to 30-second latencies. The rates had been computed from an earlier,
quieter max. Fix: stop at the window, flag overload, calibrate right before.

**How would you add snapshots?** Keep a list of live snapshot sequence
numbers. Compaction keeps, for each key, the newest entry at or below each
live snapshot instead of only the newest overall. `DBIter` already filters
by sequence number.
