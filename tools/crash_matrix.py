#!/usr/bin/env python3
"""Crash matrix for LSM-KV.

Runs `lsmkv-stress write` under continuous load, records every write the
engine *acknowledged* (the child prints an ack line only after Put() returned
OK) into an append-only client log, kills the process at a random moment with
SIGKILL (TerminateProcess on Windows), restarts, and asserts with
`lsmkv-stress verify` that:

  * every acknowledged write is readable, with a value at least as new as the
    last acknowledged one for that key, and
  * nothing is corrupt: every stored value passes its embedded checksum and a
    full scan is sorted and clean.

Fault modes, mixed across iterations:

  kill        SIGKILL at a random offset, async (sync=false) writes
  kill-sync   SIGKILL at a random offset, fsync'd writes through group commit
  torn-tail   SIGKILL, then damage the newest WAL's tail the way a torn
              in-flight write would (partial record, garbage, or zero-fill)
  fsync-fail  inject an fsync failure; the engine must fail the write, stop
              acknowledging writes, and lose nothing it already acked

The DB directory persists across iterations, so later iterations recover a
DB with many SSTables, levels and compactions in flight.

What SIGKILL tests and what it does not: a killed process loses its user-space
buffers but not the OS page cache, so this proves process-crash durability
for every acknowledged write. Power-loss durability (sync=true writes
surviving loss of un-fsync'd data) is covered by the FaultInjectionEnv tests
in tests/fault_injection_test.cc.

Usage:
  python tools/crash_matrix.py --build build/rel --iterations 200 --out results/crash_matrix.json
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MODES = ["kill", "kill-sync", "torn-tail", "fsync-fail"]
MODE_WEIGHTS = [0.40, 0.20, 0.25, 0.15]


def find_binary(build: Path, name: str) -> Path:
    for candidate in (build / name, build / f"{name}.exe"):
        if candidate.exists():
            return candidate
    sys.exit(f"cannot find {name} in {build} -- build the project first")


def newest_wal(db: Path) -> Path | None:
    logs = sorted(p for p in db.glob("*.log"))
    return logs[-1] if logs else None


def damage_wal_tail(db: Path, rng: random.Random) -> str:
    """Appends bytes that look like a write torn mid-flight."""
    wal = newest_wal(db)
    if wal is None:
        return "no-wal"
    kind = rng.choice(["partial-record", "garbage", "zero-fill"])
    with open(wal, "ab") as f:
        if kind == "partial-record":
            # A plausible header (crc, length=500, type=FULL) with only part of
            # the payload: exactly what a crash during an append leaves.
            header = rng.randbytes(4) + (500).to_bytes(2, "little") + b"\x01"
            f.write(header + rng.randbytes(rng.randint(0, 400)))
        elif kind == "garbage":
            f.write(rng.randbytes(rng.randint(1, 3000)))
        else:
            f.write(b"\x00" * rng.randint(1, 5000))
    return kind


class Child:
    """Runs a writer and streams its ack lines into the client log."""

    def __init__(self, cmd: list[str], acked_log: Path):
        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.acks = 0
        self.errors: list[str] = []
        self._log = open(acked_log, "a", buffering=1)
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            if line.startswith("A "):
                self._log.write(line)  # the external client's record of acks
                self.acks += 1
            elif line.startswith("E "):
                self.errors.append(line.strip())

    def kill(self) -> None:
        self.proc.kill()  # SIGKILL / TerminateProcess: no cleanup runs

    def wait(self, timeout: float) -> int | None:
        try:
            rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
        self._thread.join(timeout=10)
        self._log.flush()
        self._log.close()
        return rc


def keep_system_awake() -> None:
    """Stop Windows idle-sleep for the life of this process (no-op elsewhere)."""
    if os.name == "nt":
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)


def run(args: argparse.Namespace) -> int:
    keep_system_awake()
    build = Path(args.build)
    stress = find_binary(build, "lsmkv-stress")
    rng = random.Random(args.seed)

    work = Path(args.workdir) if args.workdir else Path(tempfile.mkdtemp(prefix="lsmkv-crash-"))
    db = work / "db"
    acked_log = work / "acked.log"
    if db.exists():
        shutil.rmtree(db)
    db.mkdir(parents=True, exist_ok=True)
    acked_log.write_text("")

    common = [
        f"--db={db}",
        f"--keys={args.keys}",
        f"--value_size={args.value_size}",
        f"--write_buffer={args.write_buffer}",
    ]

    records = []
    failures = 0
    started = time.time()
    for i in range(args.iterations):
        mode = rng.choices(MODES, weights=MODE_WEIGHTS)[0]
        threads = rng.choice([1, 2, 4, 8])
        cmd = [
            str(stress), "write", *common,
            f"--threads={threads}",
            f"--start={i * 1_000_000_000 + 1}",
            f"--seed={rng.randrange(1 << 30)}",
            f"--sync={'1' if mode in ('kill-sync', 'fsync-fail') else '0'}",
        ]
        fail_after = 0
        if mode == "fsync-fail":
            fail_after = rng.randint(2, 200)
            cmd.append(f"--fail_sync_after={fail_after}")

        child = Child(cmd, acked_log)
        delay_ms = 0
        if mode == "fsync-fail":
            rc = child.wait(timeout=60)
            expected_exit = rc == 3 and bool(child.errors)
        else:
            delay_ms = rng.randint(args.min_kill_ms, args.max_kill_ms)
            time.sleep(delay_ms / 1000.0)
            child.kill()
            rc = child.wait(timeout=30)
            expected_exit = True

        damage = ""
        if mode == "torn-tail":
            damage = damage_wal_tail(db, rng)

        verify = subprocess.run(
            [str(stress), "verify", *common, f"--acked={acked_log}"],
            capture_output=True, text=True, timeout=300,
        )
        try:
            result = json.loads(verify.stdout.strip().splitlines()[-1])
        except (IndexError, json.JSONDecodeError):
            result = {"ok": False, "error": verify.stdout + verify.stderr}

        ok = bool(result.get("ok")) and expected_exit
        if not ok:
            failures += 1
        rec = {
            "iteration": i,
            "mode": mode,
            "threads": threads,
            "kill_after_ms": delay_ms,
            "fail_sync_after": fail_after,
            "damage": damage,
            "child_exit": rc,
            "acks_this_run": child.acks,
            "ok": ok,
            **{k: result.get(k) for k in ("acked_writes", "keys_checked", "lost",
                                          "corrupt", "scanned", "open_ms", "wal_bytes",
                                          "problem", "error")},
        }
        records.append(rec)
        status = "ok  " if ok else "FAIL"
        print(
            f"[{i + 1:3d}/{args.iterations}] {status} {mode:<10} threads={threads} "
            f"acks+={child.acks:<6} total_acked={result.get('acked_writes')} "
            f"keys={result.get('keys_checked')} open={result.get('open_ms', 0):.1f}ms "
            f"wal={result.get('wal_bytes', 0)}B {damage}",
            flush=True,
        )
        if not ok:
            print(f"      problem: {result.get('problem') or result.get('error')} "
                  f"(child exit {rc}, errors={child.errors[:1]})", flush=True)
            if args.stop_on_failure:
                break

    elapsed = time.time() - started
    open_ms = [r["open_ms"] for r in records if isinstance(r.get("open_ms"), (int, float))]
    wal = [r["wal_bytes"] for r in records if isinstance(r.get("wal_bytes"), int)]
    by_mode = {m: sum(1 for r in records if r["mode"] == m) for m in MODES}
    summary = {
        "iterations": len(records),
        "failures": failures,
        "acknowledged_writes_verified": records[-1].get("acked_writes") if records else 0,
        "iterations_by_mode": by_mode,
        "recovery_open_ms": {
            "p50": statistics.median(open_ms) if open_ms else None,
            "max": max(open_ms) if open_ms else None,
        },
        "wal_bytes_replayed": {"max": max(wal) if wal else None,
                               "mean": statistics.mean(wal) if wal else None},
        "elapsed_seconds": round(elapsed, 1),
        "seed": args.seed,
        "config": {"keys": args.keys, "value_size": args.value_size,
                   "write_buffer": args.write_buffer,
                   "kill_window_ms": [args.min_kill_ms, args.max_kill_ms]},
    }
    print("\nSUMMARY " + json.dumps(summary, indent=2))
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps({"summary": summary, "iterations": records}, indent=1))
    if not args.workdir:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if failures == 0 else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", default="build/rel", help="directory holding lsmkv-stress")
    p.add_argument("--iterations", type=int, default=200)
    p.add_argument("--seed", type=int, default=20260923)
    p.add_argument("--keys", type=int, default=4000)
    p.add_argument("--value_size", type=int, default=120)
    p.add_argument("--write_buffer", type=int, default=256 << 10,
                   help="small => frequent flushes/compactions to crash inside")
    p.add_argument("--min_kill_ms", type=int, default=150)
    p.add_argument("--max_kill_ms", type=int, default=1500)
    p.add_argument("--workdir", default="", help="keep artifacts here (default: temp)")
    p.add_argument("--out", default="", help="write per-iteration JSON here")
    p.add_argument("--stop_on_failure", action="store_true")
    return run(p.parse_args())


if __name__ == "__main__":
    sys.exit(main())
