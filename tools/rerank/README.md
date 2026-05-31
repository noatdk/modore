# rerank — BERT/history candidate reranking experiment

Offline harness to answer one question before any live integration:
**does reranking Mozc's candidates by sentence context and/or per-app history
beat just taking Mozc's top pick?**

modore converts one word pulled from the cursor, so Mozc sees no phrase
context and its top candidate is context-blind. This measures the win from
fixing that.

## Run

```sh
make rerank-eval                                   # baselines on the seed set
make rerank-setup                                  # one-time: venv + torch/transformers (BERT only)
make rerank-eval RERANK_ARGS="--rerankers B0 B1 B2 R1 R2 --window 5"
make rerank-eval RERANK_ARGS="--rerankers B0 B1 --sweep"   # history-window 0..5
make rerank-eval RERANK_ARGS="--data ~/.config/modore/conversions.jsonl --rerankers B0 R2"
```

## Capture real data

The signal that matters comes from real conversions, not the synthetic seed
set. Turn on the host logger:

```ini
[experiment]
log_conversions = on
```

Each settled conversion (reading, candidate batch, the index you actually
chose after any cycling/undo, surrounding field text) appends to
`<configDir>/conversions.jsonl`. Off by default — it records what you type.
Point `--data` at that file.

## Rerankers

| id | what | cost |
|----|------|------|
| B0 | Mozc order (floor) | free |
| B1 | per-app reading→last-decided (MRU) | free |
| B2 | per-app reading→most-decided (frequency prior) | free |
| R1 | BERT pseudo-log-likelihood over `before + cand + after` (bidirectional) | ~1.3s/call — offline only |
| R2 | BERT single masked-slot prediction, candidates read off it | ~46ms/call — precomputable |

B1/B2 isolate how much a cheap history prior buys before any neural cost.
R1 vs R2 are the two approaches to compare: brute per-candidate rescoring vs.
predict-the-slot-then-match (the latter precomputes off the latency path).
`--history-weight > 0` prepends recent per-app decisions as pseudo-context for
the BERT rerankers.

## Live reranking (R2 sidecar)

Wires R2 into the running app. The host inserts Mozc's top pick as usual (zero
added latency), then asks the sidecar — async, over a Unix socket — whether
context/history favours another candidate, and swaps it in via the normal
cycle path **only if** the sidecar's confidence margin clears the threshold.

```sh
make rerank-setup          # one-time
make rerank-serve          # keep running; loads BERT, listens on <configDir>/reranker.sock
```

```ini
[experiment]
reranker = r2              # off (default) | r2
reranker_min_margin = 2.0  # top1−top2 log-prob gap required to override Mozc
```

Safety: the host never blocks on the sidecar (tight socket timeout; if it's
slow/absent, Mozc's pick stands), the swap is dropped if you've already
started a new conversion or edited the text, and `reranker_min_margin` is the
guard against regressing conversions Mozc already got right. Each override
re-commits the session, so the log/history reflect the reranked choice.
Requires `undo_window_ms > 0` (the swap rides the same window as cycle).

## Metrics (see metrics.py)

- **correction recall** — among records where you cycled away from Mozc's top,
  how often the reranker lifts your choice to position 0. The upside.
- **regression rate** — among records where Mozc's top was already right, how
  often the reranker wrongly displaces it. Must stay ≈0 or it's net-negative.
- **lift** — reranker top-1 minus Mozc-order top-1, over rerankable records.

## Caveats

- The seed set is **synthetic** — hand-ordered candidates, not real Mozc
  output — and over-weights hard homophones, so absolute top-1 is meaningless;
  it validates mechanics and gives a preliminary context signal only.
- BERT regresses some already-correct cases (seed: R1 20%, R2 10%). A live
  integration needs a confidence threshold before overriding Mozc.
- The per-app history win (B1/B2) only shows on real logs with repeated
  per-app terminology; the seed set has a few synthetic consistency runs.
- Latency here is offline wall-clock; live async/preempt would be lower.

Schema is shared with the host logger: `schema.py` ⇄
`native/macos/Pickup/ConversionLog.swift`. Keep field names in lockstep.
