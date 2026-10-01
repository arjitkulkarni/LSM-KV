#!/usr/bin/env python3
"""Interleaved A/B benchmark driver.

Comparisons (before/after a fix, LSM-KV vs LevelDB) are only fair if slow
drift in the machine -- thermals, an IDE re-indexing, a virus scanner
touching fresh SSTables -- hits both sides equally. Running all reps of A
and then all reps of B does not guarantee that: a quiet ten minutes for A and
a noisy ten for B can manufacture a 2x "win". So this driver runs one rep at
a time and alternates the order each round (A B, B A, A B, ...), then merges
the single-rep results into the same JSON shape lsmkv-bench writes, with
per-metric medians, for bench/report.py.

  python bench/ab.py scaling --build build/rel     --out results
  python bench/ab.py leveldb --build build/leveldb --out results
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


def find_bench(build: Path) -> str:
    for c in (build / "lsmkv-bench", build / "lsmkv-bench.exe"):
        if c.exists():
            return str(c)
    sys.exit(f"lsmkv-bench not found in {build}")


def merge(runs: list[dict]) -> dict:
    """Merges single-rep result files into one multi-rep result."""
    base = dict(runs[0])
    reps = [r["reps"][0] for r in runs]
    base["reps"] = reps
    base["config"] = dict(base["config"], reps=len(reps))
    base["driver"] = "bench/ab.py (interleaved, alternating order)"
    tput = [r["throughput"] for r in reps]
    med = {"throughput": statistics.median(tput),
           "load_ops_per_sec": statistics.median(r["load_ops_per_sec"] for r in reps),
           "latency_us": {}}
    ops = reps[0]["latency_us"].keys()
    for op in ops:
        vals = [r["latency_us"][op] for r in reps if op in r["latency_us"]]
        med["latency_us"][op] = {k: statistics.median(v[k] for v in vals)
                                 for k in ("p50", "p99", "p999", "mean")}
    base["median"] = med
    # CDF from the run whose throughput is closest to the median.
    target = med["throughput"]
    best = min(range(len(runs)), key=lambda i: abs(runs[i]["reps"][0]["throughput"] - target))
    base["cdf_us"] = runs[best].get("cdf_us", [])
    return base


def run_cells(bench: str, cells: list[tuple[str, dict[str, list[str]]]], common: list[str],
              reps: int, outdir: Path, scratch: Path) -> None:
    """cells: [(cell_name, {variant_name: extra_args})]; writes <cell>-<variant>.json."""
    outdir.mkdir(parents=True, exist_ok=True)
    for cell, variants in cells:
        names = list(variants)
        results: dict[str, list[dict]] = {n: [] for n in names}
        for rep in range(reps):
            order = names if rep % 2 == 0 else list(reversed(names))
            for n in order:
                with tempfile.NamedTemporaryFile(suffix=".json", delete=False, dir=scratch) as tmp:
                    tmp_path = Path(tmp.name)
                cmd = [bench, *common, *variants[n], "--reps=1", f"--out={tmp_path}",
                       f"--db={scratch / 'ab-db'}"]
                print(f"[{cell}] rep {rep + 1}/{reps} {n}: {' '.join(variants[n])}", flush=True)
                proc = subprocess.run(cmd, capture_output=True, text=True)
                line = next((l for l in proc.stdout.splitlines() if "rep 1:" in l), proc.stdout[-300:])
                print("   " + line.strip(), flush=True)
                if proc.returncode != 0:
                    sys.exit(f"bench failed: {proc.stderr[-2000:]}")
                results[n].append(json.loads(tmp_path.read_text()))
                tmp_path.unlink()
        for n in names:
            merged = merge(results[n])
            (outdir / f"{cell}-{n}.json").write_text(json.dumps(merged))
            print(f"[{cell}] {n}: median {merged['median']['throughput']:,.0f} ops/s "
                  f"(reps {[round(r['throughput']) for r in merged['reps']]})", flush=True)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("experiment", choices=["scaling", "leveldb", "shards"])
    p.add_argument("--build", default="build/rel")
    p.add_argument("--out", default="results")
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--records", type=int, default=1000000)
    p.add_argument("--warmup", type=float, default=10)
    p.add_argument("--duration", type=float, default=60)
    p.add_argument("--threads", default="1,2,4,8,16")
    args = p.parse_args()

    bench = find_bench(Path(args.build))
    out = Path(args.out)
    scratch = Path(args.build).resolve() / "ab-scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    timing = [f"--records={args.records}", f"--warmup={args.warmup}", f"--duration={args.duration}"]

    if args.experiment == "scaling":
        threads = [int(t) for t in args.threads.split(",")]
        for wl in ("load", "A"):
            cells = [(f"{wl}-t{t}", {"aw0": [f"--threads={t}", "--adaptive_wait=0"],
                                     "aw1": [f"--threads={t}", "--adaptive_wait=1"]})
                     for t in threads]
            run_cells(bench, cells, [f"--workload={wl}", *timing], args.reps, out / "scaling", scratch)
    elif args.experiment == "shards":
        cells = [("A-t8", {"s1": ["--shards=1"], "s8": ["--shards=8"]})]
        run_cells(bench, cells, ["--workload=A", "--threads=8", *timing], args.reps,
                  out / "shards", scratch)
    else:
        cells = [(w, {"lsmkv": ["--engine=lsmkv"], "leveldb": ["--engine=leveldb"]})
                 for w in ("load", "A", "B", "C", "F")]
        # Files are named <engine>-<workload>.json, so write per cell then rename.
        tmp_out = out / "leveldb" / "_ab"
        run_cells(bench, cells, ["--threads=8", *timing], args.reps, tmp_out, scratch)
        for w in ("load", "A", "B", "C", "F"):
            for e in ("lsmkv", "leveldb"):
                src = tmp_out / f"{w}-{e}.json"
                if src.exists():
                    src.replace(out / "leveldb" / f"{e}-{w}.json")
        tmp_out.rmdir()
    return 0


if __name__ == "__main__":
    sys.exit(main())
