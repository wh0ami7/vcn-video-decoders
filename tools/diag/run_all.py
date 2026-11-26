#!/usr/bin/env python3
"""Runs the AV1 / VP9 decode diagnostics and prints one verdict per check.

  1. fields    every V4L2 stateless-decoder header field has a home in our parsers          (no GPU)
  2. parse     every test stream parses, and which coding tools the streams exercise        (no GPU)
  3. decode    every stream decodes on the GPU and matches a CPU reference frame by frame  (GPU)

  run_all.py [--no-gpu] [--linux DIR] [--extra DIR ...] [--python PY] [--strict]
  --python PY  interpreter for the decode step; give one that has PyAV (`python3 -m venv v; v/bin/pip install av`)
               or the step can only say a stream decoded, not that it is bit-exact
  --no-gpu     stop after the first two checks
Exit status: 0 all passed, 2 a stream failed to parse or decode, 3 a check could not prove what it checks
(unmapped field, tool not exercised with --strict, stream without a reference with --strict).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def step(title, cmd):
    print(f"\n######## {title}")
    sys.stdout.flush()
    rc = subprocess.call([str(c) for c in cmd], cwd=HERE)
    print(f"######## {title}: {'ok' if rc == 0 else f'exit {rc}'}")
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-gpu", action="store_true")
    ap.add_argument("--linux")
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args()
    extra = [x for d in a.extra for x in ("--extra", d)]
    results = {}
    results["fields"] = step("1/3 V4L2 field coverage", [sys.executable, "fields.py"] + (["--linux", a.linux] if a.linux else []))
    results["parse"] = step("2/3 stream parsing and coding-tool coverage",
                            [sys.executable, "features.py"] + extra + (["--require-all"] if a.strict else []))
    if not a.no_gpu:
        results["decode"] = step("3/3 GPU decode against a CPU reference",
                                 [a.python, "decode.py"] + extra + (["--strict"] if a.strict else []))
    print("\n== summary")
    for k, v in results.items():
        print(f"  {k:8} {'ok' if v == 0 else f'exit {v}'}")
    worst = max(results.values())
    return 2 if 2 in results.values() else worst


if __name__ == "__main__":
    sys.exit(main())
