#!/usr/bin/env python3
"""Evaluate the romaji/ASCII classifier against curated boundary cases."""

from __future__ import annotations

import argparse
import ctypes
import platform
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = REPO_ROOT / "tools/data/classifier-eval.tsv"
DEFAULT_MODEL = REPO_ROOT / "engine/models/classifier.mdl"
DEFAULT_DICT = REPO_ROOT / "engine/models/english_dict.txt"


class Segment(ctypes.Structure):
    _fields_ = [
        ("start", ctypes.c_size_t),
        ("end", ctypes.c_size_t),
        ("is_romaji", ctypes.c_int),
    ]


@dataclass(frozen=True)
class Case:
    tier: str
    text: str
    expected: tuple[tuple[str, str], ...]
    note: str


def default_lib_path() -> Path:
    suffix = "dylib" if platform.system() == "Darwin" else "so"
    return REPO_ROOT / f"build/engine/libmodore_script.{suffix}"


def ensure_engine(lib_path: Path) -> None:
    if lib_path.exists():
        return
    subprocess.run(
        ["cmake", "-S", "engine", "-B", "build/engine"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        ["cmake", "--build", "build/engine", "--target", "modore_script"],
        cwd=REPO_ROOT,
        check=True,
    )


def parse_expected(raw: str) -> tuple[tuple[str, str], ...]:
    parts: list[tuple[str, str]] = []
    for item in raw.split("|"):
        kind, _, text = item.partition(":")
        if kind not in {"A", "R"} or not text:
            raise ValueError(f"bad expected segment {item!r}")
        parts.append((kind, text))
    return tuple(parts)


def load_cases(path: Path) -> list[Case]:
    cases: list[Case] = []
    with path.open(encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 3:
                raise ValueError(f"{path}:{lineno}: expected at least 3 tab-separated fields")
            tier, text, expected = fields[:3]
            note = fields[3] if len(fields) > 3 else ""
            if tier not in {"must", "target"}:
                raise ValueError(f"{path}:{lineno}: tier must be must or target")
            cases.append(Case(tier, text, parse_expected(expected), note))
    return cases


def load_lib(path: Path):
    lib = ctypes.CDLL(str(path))
    lib.mdr_cls_load.argtypes = [ctypes.c_char_p]
    lib.mdr_cls_load.restype = ctypes.c_void_p
    lib.mdr_cls_load_dict.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.mdr_cls_load_dict.restype = ctypes.c_int
    lib.mdr_cls_segment.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.POINTER(Segment),
        ctypes.c_size_t,
    ]
    lib.mdr_cls_segment.restype = ctypes.c_int
    lib.mdr_cls_free.argtypes = [ctypes.c_void_p]
    lib.mdr_cls_free.restype = None
    return lib


def segment(lib, handle: int, text: str) -> tuple[tuple[str, str], ...]:
    raw = text.encode("ascii")
    out = (Segment * 32)()
    n = lib.mdr_cls_segment(handle, raw, len(raw), out, len(out))
    if n < 0:
        raise RuntimeError(f"mdr_cls_segment failed for {text!r}")
    result: list[tuple[str, str]] = []
    for i in range(n):
        kind = "R" if out[i].is_romaji else "A"
        result.append((kind, raw[out[i].start:out[i].end].decode("ascii")))
    return tuple(result)


def fmt(parts: tuple[tuple[str, str], ...]) -> str:
    return "|".join(f"{kind}:{text}" for kind, text in parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lib", type=Path, default=default_lib_path())
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--dict", type=Path, default=DEFAULT_DICT)
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES)
    parser.add_argument("--strict-targets", action="store_true")
    args = parser.parse_args()

    ensure_engine(args.lib)
    cases = load_cases(args.cases)
    lib = load_lib(args.lib)
    handle = lib.mdr_cls_load(str(args.model).encode())
    if not handle:
        raise SystemExit(f"failed to load model: {args.model}")
    if args.dict and args.dict.exists():
        rc = lib.mdr_cls_load_dict(handle, str(args.dict).encode())
        if rc != 0:
            raise SystemExit(f"failed to load dictionary: {args.dict}")

    failures = 0
    target_failures = 0
    try:
        for case in cases:
            actual = segment(lib, handle, case.text)
            ok = actual == case.expected
            if not ok and case.tier == "must":
                failures += 1
            elif not ok:
                target_failures += 1
            marker = "ok" if ok else ("FAIL" if case.tier == "must" else "todo")
            print(f"{marker:4s} {case.tier:6s} {case.text:28s} {fmt(actual)}")
            if not ok:
                print(f"      expected: {fmt(case.expected)}")
    finally:
        lib.mdr_cls_free(handle)

    print(
        f"\nsummary: {len(cases) - failures - target_failures}/{len(cases)} exact, "
        f"must_failures={failures}, target_failures={target_failures}"
    )
    if failures or (args.strict_targets and target_failures):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
