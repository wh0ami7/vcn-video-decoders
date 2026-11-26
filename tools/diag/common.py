"""Shared helpers for the AV1 / VP9 decoder diagnostics (python3, standard library only)."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIAG = ROOT / "tools" / "diag"
MEDIA = ROOT / "tests" / "media"
DEFAULT_LINUX = Path(os.environ.get("LINUX_SRC", "/Users/izzy/Documents/Programming/linux-7.3.6"))


def run(cmd, timeout=120, env=None):
    """Runs cmd, returns (returncode, stdout, stderr); a timeout is rc 124."""
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                           timeout=timeout, env={**os.environ, **(env or {})})
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        return 124, e.stdout or "", f"timeout after {timeout}s"


def build_featdump():
    """Builds tools/diag/featdump from the decoders' own parser sources."""
    out = DIAG / "featdump"
    srcs = [DIAG / "featdump.c", ROOT / "vcnav1" / "av1parse.c", ROOT / "vcnvp9" / "vp9parse.c"]
    if out.exists() and out.stat().st_mtime >= max(s.stat().st_mtime for s in srcs):
        return out
    rc, so, se = run(["clang", "-O1", "-Wno-unused-parameter", "-I", ROOT / "vcnav1", "-I", ROOT / "vcnvp9",
                      *srcs, "-o", out], timeout=180)
    if rc:
        sys.exit(f"could not build featdump:\n{se}")
    return out


def streams(codec):
    """Test streams of a codec under tests/media (invalid ones are not included)."""
    d = MEDIA / codec
    return sorted(p for p in d.glob("*") if p.suffix in (".ivf", ".webm", ".mkv")) if d.is_dir() else []


def frames(codec, path):
    """Per-frame feature dicts from featdump; (frames, error text or '')."""
    rc, so, se = run([build_featdump(), codec, path], timeout=120)
    out = []
    for line in so.splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out, (se.strip() if rc else "")


def strip_c(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def struct_members(header_text, name):
    """Member names of `typedef struct {...} name;` or `struct name {...}` in C source."""
    t = strip_c(header_text)
    m = re.search(r"typedef\s+struct\s*\{(.*?)\}\s*" + re.escape(name) + r"\s*;", t, re.S) or \
        re.search(r"struct\s+" + re.escape(name) + r"\s*\{(.*?)\}\s*;", t, re.S)
    if not m:
        return None
    body = m.group(1)
    # drop nested braces
    while re.search(r"\{[^{}]*\}", body):
        body = re.sub(r"\{[^{}]*\}", " ", body)
    names = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        decl = re.sub(r"\[[^\]]*\]", "", decl)
        decl = re.sub(r"^(?:struct\s+|enum\s+|unsigned\s+|const\s+)*[A-Za-z_]\w*[\s\*]+", "", decl, count=1) \
            if "," not in decl else decl
        for part in decl.split(","):
            n = re.findall(r"[A-Za-z_]\w*", part)
            if n:
                names.append(n[-1])
    # the type-stripping above keeps only the last identifier; for "int a, b, c" lists re-split
    return names


def struct_members_c(header_text, name):
    """Member names with comma lists (`int a, b[3], c;`) handled."""
    t = strip_c(header_text)
    m = re.search(r"typedef\s+struct\s*\{(.*?)\}\s*" + re.escape(name) + r"\s*;", t, re.S) or \
        re.search(r"struct\s+" + re.escape(name) + r"\s*\{(.*?)\}\s*;", t, re.S)
    if not m:
        return None
    body = m.group(1)
    while re.search(r"\{[^{}]*\}", body):
        body = re.sub(r"\{[^{}]*\}", " ", body)
    names = []
    for decl in body.split(";"):
        decl = re.sub(r"\[[^\]]*\]", "", decl).strip()
        if not decl:
            continue
        parts = decl.split(",")
        first = re.findall(r"[A-Za-z_]\w*", parts[0])
        if not first:
            continue
        names.append(first[-1])
        for part in parts[1:]:
            n = re.findall(r"[A-Za-z_]\w*", part)
            if n:
                names.append(n[-1])
    return names
