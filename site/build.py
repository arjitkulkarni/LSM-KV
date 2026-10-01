#!/usr/bin/env python3
"""Builds the project website from site/src/page.html.

Inlines two data sources so the page works from file://, GitHub Pages or as a
hosted artifact, with no fetches:

  results/summary.json      written by bench/report.py (every number)
  site/data/artifacts.json  written by site/capture.py (every "screen")

Outputs:
  site/index.html            standalone document (GitHub Pages)
  site/dist/lsmkv-site.html  body fragment (hosts that supply their own <head>)
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"


def load(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def main() -> None:
    template = (SITE / "src" / "page.html").read_text(encoding="utf-8")
    data = {
        "results": load(ROOT / "results" / "summary.json"),
        "artifacts": load(SITE / "data" / "artifacts.json"),
    }
    payload = json.dumps(data, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")
    assert "/*__DATA__*/null" in template, "placeholder missing from template"
    fragment = template.replace("/*__DATA__*/null", payload)

    dist = SITE / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    (dist / "lsmkv-site.html").write_text(fragment, encoding="utf-8")

    standalone = (
        "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
        "</head>\n<body>\n" + fragment + "\n</body>\n</html>\n"
    )
    (SITE / "index.html").write_text(standalone, encoding="utf-8")
    size = len(standalone.encode("utf-8"))
    print(f"wrote site/index.html ({size / 1024:.0f} KiB) and site/dist/lsmkv-site.html")


if __name__ == "__main__":
    main()
