#!/usr/bin/env python3
"""Captures the real artifacts the project website shows as its "screens".

Nothing is mocked: this builds a demo database with lsmkv-stress, then records
the literal output of lsmkv-dump (SSTable, WAL, MANIFEST, DB), the first lines
of the engine's JSON event log, a live /metrics scrape from a running
benchmark, and excerpts of the actual source. Output: site/data/artifacts.json.

  python site/capture.py --build build/rel
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def exe(build: Path, name: str) -> str:
    for c in (build / name, build / f"{name}.exe"):
        if c.exists():
            return str(c)
    sys.exit(f"missing {name} in {build}")


SCRUB: list[tuple[str, str]] = []


def run(cmd: list[str]) -> str:
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600).stdout
    # Never publish local paths (they contain the user name).
    for needle, repl in SCRUB:
        out = out.replace(needle, repl)
    return out


def source_excerpt(rel: str, start_pat: str, end_pat: str, max_lines: int = 60) -> str:
    lines = (ROOT / rel).read_text(encoding="utf-8").splitlines()
    out, on = [], False
    for line in lines:
        if not on and re.search(start_pat, line):
            on = True
        if on:
            out.append(line)
            if len(out) > 1 and re.search(end_pat, line):
                break
            if len(out) >= max_lines:
                break
    return "\n".join(out)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--build", default="build/rel")
    args = p.parse_args()
    build = ROOT / args.build
    stress, dump, bench = exe(build, "lsmkv-stress"), exe(build, "lsmkv-dump"), exe(build, "lsmkv-bench")

    work = Path(tempfile.mkdtemp(prefix="lsmkv-site-"))
    db = work / "demo"
    for variant in (str(db).replace("\\", "\\\\"), str(db), str(db).replace("\\", "/"),
                    db.as_posix()):
        SCRUB.append((variant, "demo"))
    try:
        # A DB with several levels and a WAL full of group-commit records.
        subprocess.run([stress, "write", f"--db={db}", "--threads=8", "--keys=20000",
                        "--value_size=100", "--max_ops=300000", "--write_buffer=262144"],
                       stdout=subprocess.DEVNULL, timeout=600, check=False)
        a: dict[str, dict] = {}
        a["db"] = {"cmd": f"lsmkv-dump db demo", "text": run([dump, "db", str(db)])}

        ssts = sorted(db.glob("*.sst"), key=lambda p: p.stat().st_size, reverse=True)
        if ssts:
            sst = ssts[0]
            a["sst"] = {"cmd": f"lsmkv-dump sst demo/{sst.name} --entries=6",
                        "text": run([dump, "sst", str(sst), "--entries=6"])}
        logs = sorted(db.glob("*.log"))
        if logs:
            a["wal"] = {"cmd": f"lsmkv-dump wal demo/{logs[-1].name} --entries=10",
                        "text": run([dump, "wal", str(logs[-1]), "--entries=10"])}
        manifest = (db / "CURRENT").read_text().strip()
        mtext = run([dump, "manifest", str(db / manifest)])
        a["manifest"] = {"cmd": f"lsmkv-dump manifest demo/{manifest}",
                         "text": "\n".join(mtext.splitlines()[:48])}
        log_text = (db / "LOG").read_text(encoding="utf-8")
        for needle, repl in SCRUB:
            log_text = log_text.replace(needle, repl)
        log_lines = log_text.splitlines()
        interesting = [l for l in log_lines if any(k in l for k in
                       ("db_opened", "flush_finished", "compaction_finished", "trivial_move",
                        "wal_tail", "write_stall", "compaction_started"))]
        a["log"] = {"cmd": "tail demo/LOG   # one JSON object per event",
                    "text": "\n".join(interesting[:14])}

        # A live Prometheus scrape while a benchmark is running.
        proc = subprocess.Popen([bench, "--workload=A", "--records=200000", "--threads=8",
                                 "--warmup=1", "--duration=6", "--reps=1",
                                 f"--db={work / 'bench'}", "--metrics_port=9464"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        scrape = ""
        for _ in range(40):
            time.sleep(0.5)
            try:
                scrape = urllib.request.urlopen("http://127.0.0.1:9464/metrics", timeout=2).read().decode()
                if "lsmkv_ops_total" in scrape and time.time():
                    time.sleep(3)
                    scrape = urllib.request.urlopen("http://127.0.0.1:9464/metrics", timeout=2).read().decode()
                    break
            except OSError:
                continue
        proc.wait(timeout=120)
        keep = []
        for line in scrape.splitlines():
            if line.startswith("# HELP"):
                continue
            if "_bucket{" in line and 'le="+Inf"' not in line and 'le="0.0001"' not in line \
                    and 'le="1e-05"' not in line:
                continue
            keep.append(line)
        a["metrics"] = {"cmd": "curl -s localhost:9464/metrics", "text": "\n".join(keep[:70])}

        # Source, verbatim.
        a["api"] = {"cmd": "include/lsmkv/db.h",
                    "text": source_excerpt("include/lsmkv/db.h", r"^class DB \{", r"^\};", 70)}
        a["handoff"] = {"cmd": "src/db/db_impl.cc — the Day-10 fix",
                        "text": source_excerpt("src/db/db_impl.cc", r"^void DBImpl::SetWriterState",
                                               r"^  w->blocked = false;", 60) + "\n}"}
        a["recovery"] = {"cmd": "src/db/db_impl.cc — torn tail vs. corruption",
                         "text": source_excerpt("src/db/db_impl.cc",
                                                r"while \(reader.ReadRecord\(&record, &scratch\)\) \{",
                                                r"^    \}$", 20)}
        a["skiplist"] = {"cmd": "src/memtable/skiplist.h — lock-free readers",
                         "text": source_excerpt("src/memtable/skiplist.h",
                                                r"^void SkipList<Key, Comparator>::Insert",
                                                r"^\}", 40)}
        a["locks"] = {"cmd": "src/db/db_impl.h — the lock hierarchy",
                      "text": source_excerpt("src/db/db_impl.h", r"^// Locks, in acquisition order",
                                             r"^// no lock held\.", 40)}

        out = ROOT / "site" / "data" / "artifacts.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(a, indent=1), encoding="utf-8")
        print(f"wrote {out} ({', '.join(a)})")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
