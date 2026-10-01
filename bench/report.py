#!/usr/bin/env python3
"""Turns raw harness output (results/**/*.json) into:

  results/summary.json   every headline number, machine-readable
  BENCHMARKS.md          the generated tables between <!-- BEGIN:x --> markers
  site/data/results.js   window.LSMKV_RESULTS for the project website

Nothing here invents a number: every figure is read from a JSON file the
harness wrote, and headline claims are rounded *down*.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def round_down(x: float, sig: int = 2) -> float:
    """Round down to `sig` significant figures (for headline claims)."""
    if not x or x <= 0 or math.isnan(x):
        return 0
    mag = 10 ** (math.floor(math.log10(x)) - sig + 1)
    return math.floor(x / mag) * mag


def fmt_int(x) -> str:
    return "—" if x is None else f"{int(x):,}"


def fmt_us(x) -> str:
    if x is None:
        return "—"
    if x >= 1e6:
        return f"{x / 1e6:.1f} s"
    if x >= 1000:
        return f"{x / 1000:.2f} ms"
    return f"{x:.1f} µs" if x < 100 else f"{x:.0f} µs"


def median_lat(j, op="all", key="p99"):
    try:
        return j["median"]["latency_us"][op][key]
    except (KeyError, TypeError):
        return None


def code_stats():
    lines = 0
    files = 0
    for pattern in ("include/**/*.h", "src/**/*.h", "src/**/*.cc", "tools/*.cc", "bench/*.cc",
                    "bench/*.h"):
        for p in ROOT.glob(pattern):
            files += 1
            lines += sum(1 for _ in p.open(encoding="utf-8", errors="ignore"))
    test_lines = 0
    test_cases = 0
    for p in ROOT.glob("tests/*.cc"):
        text = p.read_text(encoding="utf-8", errors="ignore")
        test_lines += text.count("\n")
        test_cases += len(re.findall(r"^TEST(_F|_P)?\(", text, re.M))
    return {"source_files": files, "source_lines": lines, "test_lines": test_lines,
            "test_cases": test_cases}


def build_summary(res: Path) -> dict:
    s: dict = {"generated_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
               "code": code_stats()}
    tests = (res / "tests.txt")
    if tests.exists():
        m = re.search(r"Total Tests:\s*(\d+)", tests.read_text())
        if m:
            s["code"]["ctest_tests"] = int(m.group(1))

    micro = load(res / "micro.json")
    if micro:
        s["micro"] = micro["results"]
        s["system"] = micro["system"]

    bloom = load(res / "bloom_sweep.json")
    if bloom:
        s["bloom"] = bloom["rows"]
        s["bloom_config"] = {"records": bloom["records"], "lookups": bloom["lookups"]}

    ycsb = []
    for f in sorted((res / "ycsb").glob("lsmkv-*.json")):
        j = load(f)
        if not j:
            continue
        s.setdefault("system", j["system"])
        cfg = j["config"]
        row = {
            "workload": cfg["workload"], "distribution": cfg["distribution"],
            "threads": cfg["threads"], "records": cfg["records"], "value_size": cfg["value_size"],
            "reps": cfg["reps"], "duration_s": cfg["duration_s"], "warmup_s": cfg["warmup_s"],
            "throughput": j["median"]["throughput"],
            "p50": median_lat(j, "all", "p50"), "p99": median_lat(j, "all", "p99"),
            "p999": median_lat(j, "all", "p999"),
            "ops": {op: v for op, v in j["median"]["latency_us"].items() if op != "all"},
            "command": j["system"]["command_line"],
        }
        if cfg["workload"] == "load":
            row["throughput"] = j["median"]["load_ops_per_sec"]
            rep_t = [r["load_ops_per_sec"] for r in j.get("reps", [])]
        else:
            rep_t = [r["throughput"] for r in j.get("reps", [])]
        if rep_t:
            row["tput_min"], row["tput_max"] = min(rep_t), max(rep_t)
        ycsb.append(row)
    if ycsb:
        s["ycsb"] = ycsb
        a = next((r for r in ycsb if r["workload"] == "A" and r["distribution"] == "zipfian"), None)
        if a:
            j = load(res / "ycsb" / "lsmkv-A-zipfian.json")
            s["cdf_A"] = j.get("cdf_us", []) if j else []

    scaling = {}
    for f in sorted((res / "scaling").glob("*.json")):
        m = re.match(r"(\w+)-t(\d+)-aw(\d)\.json", f.name)
        j = load(f)
        if not m or not j:
            continue
        wl, t, aw = m.group(1), int(m.group(2)), m.group(3)
        variant = "adaptive" if aw == "1" else "baseline"
        entry = scaling.setdefault(wl, {}).setdefault(variant, {})
        tput = j["median"]["load_ops_per_sec"] if wl == "load" else j["median"]["throughput"]
        reps = j.get("reps", [])
        wp = [r.get("write_path", {}) for r in reps]
        entry[str(t)] = {
            "throughput": tput,
            "p99": median_lat(j, "all", "p99"),
            "mean_group_size": sorted(w.get("mean_group_size", 0) for w in wp)[len(wp) // 2] if wp else None,
            "queue_wait_p50_us": sorted(w.get("queue_wait_p50_us", 0) for w in wp)[len(wp) // 2] if wp else None,
            "wal_append_p50_us": sorted(w.get("wal_append_p50_us", 0) for w in wp)[len(wp) // 2] if wp else None,
        }
    if scaling:
        s["scaling"] = scaling

    shards = {}
    for f in sorted((res / "shards").glob("A-t8-s*.json")):
        j = load(f)
        if j:
            shards[f.stem.split("-s")[-1]] = {"throughput": j["median"]["throughput"],
                                             "p99": median_lat(j, "all", "p99")}
    if shards:
        s["shards"] = shards

    openloop = []
    closed = load(res / "openloop" / "A-calibration.json") or load(res / "ycsb" / "lsmkv-A-zipfian.json")
    for f in sorted((res / "openloop").glob("A-rate*.json"),
                    key=lambda p: int(re.findall(r"\d+", p.stem)[-1])):
        j = load(f)
        if not j:
            continue
        pct = int(re.findall(r"\d+", f.stem)[-1])
        svc = [r["service_us"]["all"]["p99"] for r in j["reps"] if "all" in r.get("service_us", {})]
        openloop.append({
            "pct_of_max": pct, "rate": j["config"]["rate"],
            "achieved": j["median"]["throughput"],
            "latency_p50": median_lat(j, "all", "p50"), "latency_p99": median_lat(j, "all", "p99"),
            "latency_p999": median_lat(j, "all", "p999"),
            "service_p99": sorted(svc)[len(svc) // 2] if svc else None,
            "overloaded": any(r.get("overloaded") for r in j["reps"]),
        })
    if openloop:
        s["openloop"] = openloop
        if closed:
            s["closedloop_A"] = {"throughput": closed["median"]["throughput"],
                                 "p50": median_lat(closed, "all", "p50"),
                                 "p99": median_lat(closed, "all", "p99"),
                                 "p999": median_lat(closed, "all", "p999")}

    stall = {}
    for name in ("normal", "tiny"):
        j = load(res / "stall" / f"{name}.json")
        ts = load(res / "stall" / f"{name}-timeseries.json")
        if j:
            eng = j["reps"][0].get("engine", {})
            stall[name] = {
                "write_buffer": j["config"]["write_buffer"],
                "throughput": j["median"]["throughput"],
                "p99": median_lat(j, "all", "p99"), "p999": median_lat(j, "all", "p999"),
                "update_p99": median_lat(j, "update", "p99"),
                "stall_count": eng.get("stall_count"), "stall_micros": eng.get("stall_micros"),
                "timeseries": ts or [],
            }
    if stall:
        s["stall"] = stall

    crash = load(res / "crash_matrix.json")
    if crash:
        its = crash["iterations"]
        s["crash"] = {
            "summary": crash["summary"],
            "open_ms": [r.get("open_ms") for r in its],
            "wal_bytes": [r.get("wal_bytes") for r in its],
            "modes": [r.get("mode") for r in its],
        }

    cmp_rows = []
    for w in ("load", "A", "B", "C", "F"):
        a = load(res / "leveldb" / f"lsmkv-{w}.json")
        b = load(res / "leveldb" / f"leveldb-{w}.json")
        if not a or not b:
            continue
        key = "load_ops_per_sec" if w == "load" else "throughput"
        cmp_rows.append({
            "workload": w,
            "lsmkv": a["median"][key], "leveldb": b["median"][key],
            "lsmkv_p99": median_lat(a, "all", "p99"), "leveldb_p99": median_lat(b, "all", "p99"),
            "lsmkv_wa": _wa(a), "leveldb_wa": None,
        })
    if cmp_rows:
        s["leveldb"] = cmp_rows

    s["headline"] = headline(s)
    return s


def _wa(j):
    try:
        e = j["reps"][0]["engine"]
        user = e["user_bytes"]
        return (e["wal_bytes"] + e["flush_bytes"] + e["compaction_bytes_written"]) / user if user else None
    except (KeyError, IndexError, TypeError):
        return None


def headline(s: dict) -> dict:
    h = {}
    y = {(r["workload"], r["distribution"]): r for r in s.get("ycsb", [])}
    if ("A", "zipfian") in y:
        h["ycsb_a_ops"] = round_down(y[("A", "zipfian")]["throughput"])
        h["ycsb_a_p99_us"] = y[("A", "zipfian")]["p99"]
    if ("C", "zipfian") in y:
        h["ycsb_c_ops"] = round_down(y[("C", "zipfian")]["throughput"])
    sc = s.get("scaling", {}).get("A", {})
    if "8" in sc.get("baseline", {}) and "8" in sc.get("adaptive", {}):
        b, a = sc["baseline"]["8"]["throughput"], sc["adaptive"]["8"]["throughput"]
        h["fix_speedup_A_8t"] = math.floor(a / b * 10) / 10
    ld = s.get("scaling", {}).get("load", {})
    if "8" in ld.get("baseline", {}) and "8" in ld.get("adaptive", {}):
        b, a = ld["baseline"]["8"]["throughput"], ld["adaptive"]["8"]["throughput"]
        h["fix_speedup_load_8t"] = math.floor(a / b * 10) / 10
    if s.get("crash"):
        cs = s["crash"]["summary"]
        h["crash_iterations"] = cs["iterations"]
        h["crash_failures"] = cs["failures"]
        h["acked_writes_verified"] = cs["acknowledged_writes_verified"]
    b10 = next((r for r in s.get("bloom", []) if r["bits_per_key"] == 10), None)
    b0 = next((r for r in s.get("bloom", []) if r["bits_per_key"] == 0), None)
    if b10 and b0:
        h["bloom10_fp_pct"] = round(b10["fp_measured"] * 100, 2)
        h["bloom_reads_saved_pct"] = math.floor(
            (1 - b10["reads_per_negative_get"] / b0["reads_per_negative_get"]) * 1000) / 10 \
            if b0["reads_per_negative_get"] else None
    if s.get("code"):
        h["test_cases"] = s["code"]["test_cases"]
    return h


# ---- Markdown tables -----------------------------------------------------------------

def md_env(s):
    sysi = s.get("system", {})
    return "\n".join([
        "| | |", "|---|---|",
        f"| CPU | {sysi.get('cpu_model', '?')} ({sysi.get('hardware_threads', '?')} hardware threads) |",
        f"| Memory | {sysi.get('memory_gib', '?')} GiB |",
        f"| OS | {sysi.get('os', '?')} |",
        f"| CPU governor | {sysi.get('cpu_governor', '?')} |",
        f"| Compiler | {sysi.get('compiler', '?')} |",
        f"| Build | `{sysi.get('build_flags', '?')}` |",
        f"| Generated | {s['generated_utc']} |",
    ])


def md_ycsb(s):
    rows = ["| Workload | Keys | Throughput (ops/s, median) | Reps min–max | p50 | p99 | p99.9 | Per-op p99 |",
            "|---|---|---:|---:|---:|---:|---:|---|"]
    order = {"load": 0, "A": 1, "B": 2, "C": 3, "D": 4, "F": 5}
    for r in sorted(s.get("ycsb", []), key=lambda r: (order.get(r["workload"], 9), r["distribution"])):
        per = ", ".join(f"{op} {fmt_us(v['p99'])}" for op, v in r["ops"].items())
        spread = (f"{_compact(r['tput_min'])}–{_compact(r['tput_max'])}"
                  if r.get("tput_min") is not None else "—")
        rows.append(f"| {r['workload']} | {r['distribution']} | {fmt_int(r['throughput'])} | {spread} | "
                    f"{fmt_us(r['p50'])} | {fmt_us(r['p99'])} | {fmt_us(r['p999'])} | {per} |")
    return "\n".join(rows)


def md_bloom(s):
    rows = ["| bits/key | k | FP rate (measured) | FP rate (theory) | block reads / negative Get | "
            "block reads / positive Get | negative Get |",
            "|---:|---:|---:|---:|---:|---:|---:|"]
    for r in s.get("bloom", []):
        fp = "— (no filter)" if r["bits_per_key"] == 0 else f"{r['fp_measured'] * 100:.2f}%"
        th = "—" if r["bits_per_key"] == 0 else f"{r['fp_theory'] * 100:.2f}%"
        rows.append(f"| {r['bits_per_key']} | {r['k'] or '—'} | {fp} | {th} | "
                    f"{r['reads_per_negative_get']:.3f} | {r['reads_per_positive_get']:.3f} | "
                    f"{fmt_us(r['negative_get_us'])} |")
    return "\n".join(rows)


def md_scaling(s, wl):
    sc = s.get("scaling", {}).get(wl, {})
    if not sc:
        return "_not run_"
    rows = ["| Threads | Baseline (block on condvar) | After fix (spin → yield → block) | Speed-up | "
            "Mean group size (after) | Queue wait p50 before → after |",
            "|---:|---:|---:|---:|---:|---|"]
    for t in sorted({int(k) for v in sc.values() for k in v}):
        b = sc.get("baseline", {}).get(str(t), {})
        a = sc.get("adaptive", {}).get(str(t), {})
        sp = (a.get("throughput", 0) / b["throughput"]) if b.get("throughput") else None
        rows.append(f"| {t} | {fmt_int(b.get('throughput'))} | {fmt_int(a.get('throughput'))} | "
                    f"{'—' if sp is None else f'{sp:.2f}×'} | "
                    f"{a.get('mean_group_size', 0) or 0:.2f} | "
                    f"{fmt_us(b.get('queue_wait_p50_us'))} → {fmt_us(a.get('queue_wait_p50_us'))} |")
    return "\n".join(rows)


def md_shards(s):
    sh = s.get("shards", {})
    if not sh:
        return "_not run_"
    rows = ["| Memtable shards | Throughput (ops/s) | p99 |", "|---:|---:|---:|"]
    for k in sorted(sh, key=int):
        rows.append(f"| {k} | {fmt_int(sh[k]['throughput'])} | {fmt_us(sh[k]['p99'])} |")
    return "\n".join(rows)


def md_openloop(s):
    ol = s.get("openloop", [])
    if not ol:
        return "_not run_"
    rows = ["| Mode | Offered rate | Achieved (ops/s) | p50 | p99 | p99.9 | Service-time p99 |",
            "|---|---:|---:|---:|---:|---:|---:|"]
    c = s.get("closedloop_A")
    if c:
        rows.append(f"| closed loop (calibration: max) | — | {fmt_int(c['throughput'])} | {fmt_us(c['p50'])} | "
                    f"{fmt_us(c['p99'])} | {fmt_us(c['p999'])} | = latency |")
    for r in ol:
        tag = " **(overloaded)**" if r.get("overloaded") else ""
        rows.append(f"| open loop @ {r['pct_of_max']}% of max{tag} | {fmt_int(r['rate'])} | {fmt_int(r['achieved'])} | "
                    f"{fmt_us(r['latency_p50'])} | {fmt_us(r['latency_p99'])} | {fmt_us(r['latency_p999'])} | "
                    f"{fmt_us(r['service_p99'])} |")
    return "\n".join(rows)


def md_stall(s):
    st = s.get("stall", {})
    if not st:
        return "_not run_"
    rows = ["| Memtable | Throughput (ops/s) | update p99 | p99.9 (all) | Write stalls | Time stalled |",
            "|---|---:|---:|---:|---:|---:|"]
    for name in ("normal", "tiny"):
        r = st.get(name)
        if not r:
            continue
        label = f"{r['write_buffer'] // 1024} KiB" + (" (regression)" if name == "tiny" else "")
        rows.append(f"| {label} | {fmt_int(r['throughput'])} | {fmt_us(r['update_p99'])} | "
                    f"{fmt_us(r['p999'])} | {fmt_int(r['stall_count'])} | "
                    f"{(r['stall_micros'] or 0) / 1e6:.2f} s |")
    return "\n".join(rows)


def md_crash(s):
    c = s.get("crash")
    if not c:
        return "_not run_"
    sm = c["summary"]
    modes = ", ".join(f"{k}: {v}" for k, v in sm["iterations_by_mode"].items())
    ro = sm["recovery_open_ms"]
    return "\n".join([
        "| | |", "|---|---|",
        f"| Iterations | {sm['iterations']} ({modes}) |",
        f"| Failures | **{sm['failures']}** |",
        f"| Acknowledged writes verified after restart | {fmt_int(sm['acknowledged_writes_verified'])} |",
        f"| Recovery (Open) time | p50 {ro['p50']:.1f} ms, max {ro['max']:.1f} ms |",
        f"| WAL replayed per restart | max {fmt_int(sm['wal_bytes_replayed']['max'])} bytes |",
        f"| Wall clock | {sm['elapsed_seconds']:.0f} s, seed {sm['seed']} |",
    ])


def md_leveldb(s):
    rows = s.get("leveldb", [])
    if not rows:
        return "_not run_"
    out = ["| Workload | LSM-KV (ops/s) | LevelDB 1.23 (ops/s) | Ratio | LSM-KV p99 | LevelDB p99 |",
           "|---|---:|---:|---:|---:|---:|"]
    for r in rows:
        ratio = r["lsmkv"] / r["leveldb"] if r["leveldb"] else 0
        out.append(f"| {r['workload']} | {fmt_int(r['lsmkv'])} | {fmt_int(r['leveldb'])} | "
                   f"{ratio:.2f}× | {fmt_us(r['lsmkv_p99'])} | {fmt_us(r['leveldb_p99'])} |")
    return "\n".join(out)


def md_micro(s):
    m = s.get("micro")
    if not m:
        return "_not run_"
    pick = [
        ("skiplist_insert_ns_per_op", "skiplist insert (1M random u64)", "ns"),
        ("skiplist_lookup_ns_per_op", "skiplist lookup", "ns"),
        ("std_map_insert_ns_per_op", "std::map insert (same keys)", "ns"),
        ("std_map_lookup_ns_per_op", "std::map lookup", "ns"),
        ("memtable_1shard_insert_ns_per_op", "memtable insert, 1 shard", "ns"),
        ("memtable_8shard_insert_ns_per_op", "memtable insert, 8 shards", "ns"),
        ("memtable_1shard_get_ns_per_op", "memtable get", "ns"),
        ("memtable_bytes_per_entry", "memtable bytes per entry (16 B key, 100 B value)", "B"),
        ("memtable_overhead_bytes_per_entry", "memtable overhead per entry", "B"),
        ("crc32c_gb_per_sec", "CRC32C (software, slicing-by-8)", "GB/s"),
        ("bloom_probe_ns", "Bloom probe (10 bits/key)", "ns"),
        ("wal_append_128B_records_per_sec", "WAL append, 128 B records, no fsync", "rec/s"),
        ("fsync_p50_us", "fsync p50 (this disk)", "µs"),
        ("fsync_p99_us", "fsync p99 (this disk)", "µs"),
        ("memtable_1shard_8thr_mops", "memtable 8-thread inserts, 1 shard", "M/s"),
        ("memtable_8shard_8thr_mops", "memtable 8-thread inserts, 8 shards", "M/s"),
        ("cache_1shard_8thr_mlookups", "LRU cache 8-thread lookups, 1 shard", "M/s"),
        ("cache_16shard_8thr_mlookups", "LRU cache 8-thread lookups, 16 shards", "M/s"),
    ]
    rows = ["| Measurement | Value |", "|---|---:|"]
    for key, label, unit in pick:
        if key in m:
            v = m[key]
            rows.append(f"| {label} | {v:,.2f} {unit} |" if v < 100 else f"| {label} | {v:,.0f} {unit} |")
    return "\n".join(rows)


def md_headline(s):
    h = s.get("headline", {})
    y = {(r["workload"], r["distribution"]): r for r in s.get("ycsb", [])}
    rows = ["| | |", "|---|---|"]
    if "ycsb_a_ops" in h:
        a = y[("A", "zipfian")]
        rows.append(f"| YCSB-A (50% updates, Zipfian), 8 threads | **{fmt_int(h['ycsb_a_ops'])}+ ops/s**, "
                    f"p99 {fmt_us(a['p99'])}, p99.9 {fmt_us(a['p999'])} |")
    if "ycsb_c_ops" in h:
        c = y[("C", "zipfian")]
        rows.append(f"| YCSB-C (read-only, Zipfian), 8 threads | **{fmt_int(h['ycsb_c_ops'])}+ ops/s**, "
                    f"p99 {fmt_us(c['p99'])} |")
    if "fix_speedup_load_8t" in h:
        ld = s["scaling"]["load"]
        rows.append(f"| Write-path fix (spin-then-block commit queue), 8 writers | "
                    f"**{h['fix_speedup_load_8t']:.1f}×** write throughput "
                    f"({fmt_int(ld['baseline']['8']['throughput'])} → {fmt_int(ld['adaptive']['8']['throughput'])} ops/s) |")
    if "fix_speedup_A_8t" in h:
        rows.append(f"| Same fix on YCSB-A, 8 threads | **{h['fix_speedup_A_8t']:.1f}×** |")
    if "crash_iterations" in h:
        rows.append(f"| Crash matrix (SIGKILL / torn WAL tail / fsync failure) | "
                    f"**{h['crash_iterations']} iterations, {h['crash_failures']} failures**, "
                    f"{fmt_int(h['acked_writes_verified'])} acknowledged writes verified |")
    if "bloom10_fp_pct" in h:
        rows.append(f"| Bloom filter, 10 bits/key | {h['bloom10_fp_pct']}% false positives; "
                    f"**{h.get('bloom_reads_saved_pct')}% fewer** SSTable reads per negative lookup |")
    if s.get("crash"):
        ro = s["crash"]["summary"]["recovery_open_ms"]
        rows.append(f"| Recovery (WAL replay + Open) after a crash | p50 {ro['p50']:.1f} ms, "
                    f"max {ro['max']:.1f} ms |")
    m = s.get("micro", {})
    if "memtable_bytes_per_entry" in m:
        rows.append(f"| Memtable memory per entry (16 B key, 100 B value) | "
                    f"{m['memtable_bytes_per_entry']:.0f} B "
                    f"({m['memtable_overhead_bytes_per_entry']:.0f} B overhead) |")
    if s.get("leveldb"):
        parts = []
        for r in s["leveldb"]:
            parts.append(f"{r['workload']} {r['lsmkv'] / r['leveldb']:.2f}×")
        rows.append(f"| vs LevelDB 1.23, same harness & box (throughput ratio) | {', '.join(parts)} |")
    sysi = s.get("system", {})
    rows.append(f"| Machine | {sysi.get('cpu_model', '?')}, {sysi.get('os', '?')} |")
    return "\n".join(rows)


def rewrite_readme(s):
    path = ROOT / "README.md"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    pat = re.compile(r"(<!-- BEGIN:headline -->)(.*?)(<!-- END:headline -->)", re.S)
    text = pat.sub(lambda m: f"{m.group(1)}\n{md_headline(s)}\n{m.group(3)}", text)
    path.write_text(text, encoding="utf-8")


def rewrite_benchmarks_md(s):
    path = ROOT / "BENCHMARKS.md"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    sections = {
        "env": md_env(s), "micro": md_micro(s), "ycsb": md_ycsb(s), "bloom": md_bloom(s),
        "scaling-load": md_scaling(s, "load"), "scaling-A": md_scaling(s, "A"),
        "shards": md_shards(s), "openloop": md_openloop(s), "stall": md_stall(s),
        "crash": md_crash(s), "leveldb": md_leveldb(s),
    }
    for name, body in sections.items():
        pat = re.compile(rf"(<!-- BEGIN:{re.escape(name)} -->)(.*?)(<!-- END:{re.escape(name)} -->)", re.S)
        text = pat.sub(lambda m: f"{m.group(1)}\n{body}\n{m.group(3)}", text)
    path.write_text(text, encoding="utf-8")


# ---- Static SVG plots (embedded by README.md; light background so they read
# on GitHub in both themes) ------------------------------------------------------

INK, MUTED, GRID, ACCENT, BASE, BG = "#0C0F14", "#697281", "#E1E4E9", "#2346FF", "#A3AAB5", "#FFFFFF"
FONT = "font-family='IBM Plex Mono, Consolas, monospace'"


def _svg(w, h, body, title):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {w} {h}' width='{w}' height='{h}' "
            f"role='img' aria-label='{title}'><title>{title}</title>"
            f"<rect width='{w}' height='{h}' fill='{BG}' rx='12'/>{body}</svg>\n")


def _nice(v):
    if v <= 0:
        return 1
    p = 10 ** math.floor(math.log10(v))
    for m in (1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if m * p >= v:
            return m * p
    return 10 * p


def _compact(v):
    return f"{v / 1e6:.1f}M" if v >= 1e6 else f"{v / 1e3:.0f}k" if v >= 1e3 else f"{v:.0f}"


def plot_scaling(s, wl):
    sc = s.get("scaling", {}).get(wl, {})
    if not sc.get("baseline") or not sc.get("adaptive"):
        return None
    threads = sorted({int(t) for v in sc.values() for t in v})
    W, H, l, r, t, b = 760, 420, 70, 30, 56, 56
    ymax = _nice(max(e["throughput"] for v in sc.values() for e in v.values()))
    lx = lambda v: l + (math.log2(v) - math.log2(threads[0])) / max(1e-9, math.log2(threads[-1]) - math.log2(threads[0])) * (W - l - r)
    ly = lambda v: H - b - v / ymax * (H - t - b)
    g = [f"<text x='{l}' y='30' fill='{INK}' font-size='15' font-weight='600' {FONT}>"
         f"{'Write-only load' if wl == 'load' else 'YCSB-A'}: throughput vs client threads</text>"]
    for i in range(5):
        v = ymax * i / 4
        g.append(f"<line x1='{l}' x2='{W - r}' y1='{ly(v):.1f}' y2='{ly(v):.1f}' stroke='{GRID}'/>"
                 f"<text x='{l - 10}' y='{ly(v) + 4:.1f}' text-anchor='end' fill='{MUTED}' font-size='11' {FONT}>{_compact(v)}</text>")
    for th in threads:
        g.append(f"<text x='{lx(th):.1f}' y='{H - b + 22}' text-anchor='middle' fill='{MUTED}' font-size='11' {FONT}>{th}</text>")
    g.append(f"<text x='{(l + W - r) / 2}' y='{H - 12}' text-anchor='middle' fill='{MUTED}' font-size='11' {FONT}>client threads</text>")
    for key, color, label in (("baseline", BASE, "baseline: block on condvar"),
                              ("adaptive", ACCENT, "fix: spin → yield → block")):
        pts = [(lx(th), ly(sc[key][str(th)]["throughput"])) for th in threads if str(th) in sc[key]]
        d = "".join(("M" if i == 0 else "L") + f"{x:.1f},{y:.1f}" for i, (x, y) in enumerate(pts))
        g.append(f"<path d='{d}' fill='none' stroke='{color}' stroke-width='2.5' stroke-linejoin='round'/>")
        g.extend(f"<circle cx='{x:.1f}' cy='{y:.1f}' r='4.5' fill='{color}'/>" for x, y in pts)
    g.append(f"<rect x='{W - r - 250}' y='{t - 8}' width='14' height='3' fill='{ACCENT}'/>"
             f"<text x='{W - r - 230}' y='{t - 3}' fill='{INK}' font-size='11' {FONT}>fix: spin → yield → block</text>"
             f"<rect x='{W - r - 250}' y='{t + 10}' width='14' height='3' fill='{BASE}'/>"
             f"<text x='{W - r - 230}' y='{t + 15}' fill='{INK}' font-size='11' {FONT}>baseline: block on condvar</text>")
    return _svg(W, H, "".join(g), "Throughput vs client threads, before and after the commit-queue fix")


def plot_cdf(s):
    cdf = s.get("cdf_A") or []
    if not cdf:
        return None
    W, H, l, r, t, b = 760, 400, 60, 30, 50, 50
    xs = [max(p[0], 0.05) for p in cdf]
    x0 = 10 ** math.floor(math.log10(min(xs)))
    x1 = 10 ** math.ceil(math.log10(max(xs)))
    lx = lambda v: l + (math.log10(max(v, x0)) - math.log10(x0)) / (math.log10(x1) - math.log10(x0)) * (W - l - r)
    ly = lambda v: H - b - v * (H - t - b)
    g = [f"<text x='{l}' y='30' fill='{INK}' font-size='15' font-weight='600' {FONT}>YCSB-A latency CDF (8 threads)</text>"]
    d = x0
    while d <= x1 * 1.0001:
        lab = f"{d / 1000:g} ms" if d >= 1000 else f"{d:g} µs"
        g.append(f"<line x1='{lx(d):.1f}' x2='{lx(d):.1f}' y1='{t}' y2='{H - b}' stroke='{GRID}'/>"
                 f"<text x='{lx(d):.1f}' y='{H - b + 20}' text-anchor='middle' fill='{MUTED}' font-size='11' {FONT}>{lab}</text>")
        d *= 10
    for v in (0, 0.5, 0.9, 0.99, 1):
        g.append(f"<text x='{l - 10}' y='{ly(v) + 4:.1f}' text-anchor='end' fill='{MUTED}' font-size='11' {FONT}>{v:g}</text>")
    pts = [(lx(p[0]), ly(p[1])) for p in cdf]
    path = "".join(("M" if i == 0 else "L") + f"{x:.1f},{y:.1f}" for i, (x, y) in enumerate(pts))
    g.append(f"<path d='{path}' fill='none' stroke='{ACCENT}' stroke-width='2.5'/>")
    return _svg(W, H, "".join(g), "YCSB-A latency CDF")


def plot_bloom(s):
    rows = s.get("bloom") or []
    if not rows:
        return None
    W, H, l, r, t, b = 760, 380, 60, 30, 56, 64
    ymax = _nice(max(x["reads_per_negative_get"] for x in rows))
    bw = (W - l - r) / len(rows)
    ly = lambda v: H - b - v / ymax * (H - t - b)
    g = [f"<text x='{l}' y='30' fill='{INK}' font-size='15' font-weight='600' {FONT}>SSTable block reads per lookup of an absent key</text>"]
    for i in range(5):
        v = ymax * i / 4
        g.append(f"<line x1='{l}' x2='{W - r}' y1='{ly(v):.1f}' y2='{ly(v):.1f}' stroke='{GRID}'/>"
                 f"<text x='{l - 10}' y='{ly(v) + 4:.1f}' text-anchor='end' fill='{MUTED}' font-size='11' {FONT}>{v:.2g}</text>")
    for i, row in enumerate(rows):
        cx = l + bw * i + bw / 2
        v = row["reads_per_negative_get"]
        color = ACCENT if row["bits_per_key"] == 10 else BASE
        g.append(f"<rect x='{cx - bw * 0.28:.1f}' y='{ly(v):.1f}' width='{bw * 0.56:.1f}' height='{max(H - b - ly(v), 1):.1f}' rx='3' fill='{color}'/>"
                 f"<text x='{cx:.1f}' y='{ly(v) - 8:.1f}' text-anchor='middle' fill='{INK}' font-size='11' {FONT}>{v:.3f}</text>"
                 f"<text x='{cx:.1f}' y='{H - b + 20}' text-anchor='middle' fill='{MUTED}' font-size='11' {FONT}>"
                 f"{'no filter' if row['bits_per_key'] == 0 else str(row['bits_per_key']) + ' bits/key'}</text>")
        if row["bits_per_key"]:
            g.append(f"<text x='{cx:.1f}' y='{H - b + 38}' text-anchor='middle' fill='{MUTED}' font-size='10' {FONT}>"
                     f"{row['fp_measured'] * 100:.2f}% FP</text>")
    return _svg(W, H, "".join(g), "Bloom filter sweep: block reads per negative lookup")


def write_plots(s, res):
    out = res / "plots"
    out.mkdir(parents=True, exist_ok=True)
    for name, svg in (("scaling-load.svg", plot_scaling(s, "load")), ("scaling-A.svg", plot_scaling(s, "A")),
                      ("latency-cdf-A.svg", plot_cdf(s)), ("bloom-sweep.svg", plot_bloom(s))):
        if svg:
            (out / name).write_text(svg, encoding="utf-8")


def render_resume(s):
    """docs/resume.template.md -> docs/RESUME.md with measured numbers."""
    tpl = ROOT / "docs" / "resume.template.md"
    if not tpl.exists():
        return
    h = s.get("headline", {})
    y = {(r["workload"], r["distribution"]): r for r in s.get("ycsb", [])}
    ld = s.get("scaling", {}).get("load", {})
    base8 = ld.get("baseline", {}).get("8", {}).get("throughput")
    fix8 = ld.get("adaptive", {}).get("8", {}).get("throughput")
    a = y.get(("A", "zipfian"), {})
    values = {
        "TESTS": s.get("code", {}).get("ctest_tests"),
        "ACKED": f"{int(h['acked_writes_verified']):,}" if h.get("acked_writes_verified") else None,
        "FIX": f"{h['fix_speedup_load_8t']:.1f}" if h.get("fix_speedup_load_8t") else None,
        "BASE8": f"{int(round_down(base8, 3)):,}" if base8 else None,
        "FIX8": f"{int(round_down(fix8, 3)):,}" if fix8 else None,
        "YCSBA": f"{int(h['ycsb_a_ops']):,}" if h.get("ycsb_a_ops") else None,
        "P99A": fmt_us(a.get("p99")) if a.get("p99") is not None else None,
        "P99A_TEX": (fmt_us(a.get("p99")).replace(" µs", "\\,\\textmu s").replace(" ms", "\\,ms")
                     if a.get("p99") is not None else None),
        "BLOOMSAVE": h.get("bloom_reads_saved_pct"),
        "BLOOMFP": h.get("bloom10_fp_pct"),
    }
    text = tpl.read_text(encoding="utf-8")
    for k, v in values.items():
        text = text.replace("{{" + k + "}}", str(v) if v is not None else f"[{k}: not measured]")
    header = ("<!-- Generated by bench/report.py from docs/resume.template.md and results/. "
              "Edit the template, not this file. -->\n")
    (ROOT / "docs" / "RESUME.md").write_text(header + text, encoding="utf-8")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--results", default="results")
    args = p.parse_args()
    res = (ROOT / args.results) if not Path(args.results).is_absolute() else Path(args.results)
    s = build_summary(res)
    (res / "summary.json").write_text(json.dumps(s, indent=1))
    site = ROOT / "site" / "data"
    site.mkdir(parents=True, exist_ok=True)
    (site / "results.js").write_text(
        "// Generated by bench/report.py from results/*.json. Do not edit.\n"
        "window.LSMKV_RESULTS = " + json.dumps(s, separators=(",", ":")) + ";\n",
        encoding="utf-8")
    rewrite_benchmarks_md(s)
    rewrite_readme(s)
    write_plots(s, res)
    render_resume(s)
    print(json.dumps(s["headline"], indent=1))


if __name__ == "__main__":
    main()
