#!/usr/bin/env python3
"""Offline reranker eval: does context/history reranking beat Mozc's top pick?

Replays a JSONL conversion log (real captures from the host logger, or the
bundled synthetic seed set) the way a live reranker would see it -- in order,
each record paired with the last N prior decisions from the same app -- runs
each reranker, and reports the metrics that decide the win (see metrics.py).

  # baselines only, on the bundled seed set (no torch needed):
  python eval.py --rerankers B0 B1 B2

  # add BERT (needs `make rerank-setup` first):
  python eval.py --rerankers B0 B1 B2 R1 R2 --window 5

  # on real captured data:
  python eval.py --data ~/.config/modore/conversions.jsonl --rerankers B0 R1 R2
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import metrics
import rerankers
from schema import iter_with_history, load_jsonl

SEED = Path(__file__).parent / "data" / "seed_homophones.jsonl"


def run(records, names, window, history_weight):
    rk = rerankers.build(names, history_weight=history_weight)
    results = {r.name: metrics.EvalResult(r.name) for r in rk}
    for r in rk:
        for rec, hist in iter_with_history(records, window):
            t0 = time.perf_counter()
            ranked = r.rank(rec, hist)
            dt = (time.perf_counter() - t0) * 1000.0
            results[r.name].observe(rec, ranked, latency_ms=dt)
    return [results[r.name] for r in rk]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=str(SEED), help="JSONL conversion log (default: bundled seed set)")
    ap.add_argument("--rerankers", nargs="+", default=["B0", "B1", "B2"],
                    help="subset of B0 B1 B2 R1 R2")
    ap.add_argument("--window", type=int, default=5, help="per-app history depth (your 'last 5')")
    ap.add_argument("--history-weight", type=float, default=0.0,
                    help="prepend recent decisions as pseudo-context for R1/R2 (0=off)")
    ap.add_argument("--sweep", action="store_true",
                    help="sweep window 0..5 for history-aware rerankers")
    args = ap.parse_args()

    records = load_jsonl(args.data)
    n_corr = sum(1 for r in records if r.is_correction)
    n_undo = sum(1 for r in records if r.is_undo)
    print(f"loaded {len(records)} records from {args.data}")
    print(f"  corrections (user cycled off Mozc top): {n_corr}")
    print(f"  undo (rejected all candidates):          {n_undo}")
    print(f"  history window: {args.window}\n")

    if args.sweep:
        for w in range(0, args.window + 1):
            res = run(records, args.rerankers, w, args.history_weight)
            base = next((r.top1_acc for r in res if r.name.startswith("B0")), res[0].top1_acc)
            print(f"=== window={w} ===")
            print(metrics.format_table(res, base))
            print()
        return

    res = run(records, args.rerankers, args.window, args.history_weight)
    base = next((r.top1_acc for r in res if r.name.startswith("B0")), res[0].top1_acc)
    print(metrics.format_table(res, base))


if __name__ == "__main__":
    main()
