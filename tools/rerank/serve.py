#!/usr/bin/env python3
"""Reranker sidecar — the live half of the experiment.

Loads BERT once and serves candidate-reranking requests over a Unix socket.
The modore host connects opt-in (`[experiment] reranker = r2`), and AFTER it
has already inserted Mozc's top pick, asks this for a better candidate; if the
answer clears the confidence margin, the host swaps it in via its normal cycle
path. The host never blocks on us — if we're slow or absent, Mozc's pick
stands. Stateless: history arrives with each request (the host owns it).

  make rerank-serve                 # default socket ~/.config/modore/reranker.sock
  python serve.py --socket /tmp/r.sock --history-weight 0.3

Protocol: newline-delimited JSON, one request per line.
  req:  {"reading","candidates":[...],"context_before","context_after",
         "history":[{"reading","decided_value"}...]}
  resp: {"order":[idx...],"top":idx,"margin":float,"scores":[...]}
        {"error":"..."} on failure (host treats as "keep Mozc's pick")
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys

import rerankers
from schema import ConversionRecord


def _history(items) -> list[ConversionRecord]:
    out = []
    for h in items or []:
        val = h.get("decided_value")
        if not val:
            continue
        out.append(ConversionRecord(
            reading=h.get("reading", ""), candidates=[val], decided_idx=0, decided_value=val))
    return out


def handle(reranker, req: dict) -> dict:
    cands = req.get("candidates") or []
    if not cands:
        return {"order": [], "top": -1, "margin": 0.0}
    rec = ConversionRecord(
        reading=req.get("reading", ""),
        candidates=cands,
        decided_idx=0,
        context_before=req.get("context_before"),
        context_after=req.get("context_after"),
    )
    order, margin, scores = reranker.rank_scored(rec, _history(req.get("history")))
    return {"order": order, "top": order[0], "margin": margin, "scores": scores}


def _log_request(req: dict, resp: dict) -> None:
    """Echo each request so host↔sidecar traffic is visible while testing.
    The verdict mirrors the host's actual gate: it only overrides when the
    pick differs from Mozc's top AND the margin clears the host's min_margin."""
    cands = req.get("candidates") or []
    top = resp.get("top")
    margin = resp.get("margin") or 0.0
    min_margin = req.get("min_margin", 0.0)
    pick = cands[top] if isinstance(top, int) and 0 <= top < len(cands) else "?"
    before = req.get("context_before") or ""
    after = req.get("context_after") or ""
    if top in (0, None):
        verdict = "keep-mozc[0]"
    elif margin >= min_margin:
        verdict = "OVERRIDE"
    else:
        verdict = f"skip (margin<{min_margin})"
    print(f"req: {req.get('reading','?')!r} ctx={before!r}|{after!r} cands={len(cands)} "
          f"-> top={top}({pick}) margin={margin:.2f} {verdict}", flush=True)


def serve(sock_path: str, history_weight: float) -> None:
    print(f"rerank-serve: loading {rerankers._MODEL_NAME} ...", flush=True)
    reranker = rerankers.BertPreempt(history_weight=history_weight)
    # Warm the model so the first real request isn't slow (cold MPS/CUDA
    # kernels can take seconds — long enough that the host's read times out and
    # closes the connection mid-response).
    try:
        handle(reranker, {"reading": "あめ", "candidates": ["雨", "飴"],
                          "context_before": "今日は", "context_after": "が降る", "history": []})
    except Exception:
        pass
    print(f"rerank-serve: ready on device={reranker.be.device} (warmed)", flush=True)

    if os.path.exists(sock_path):
        os.unlink(sock_path)
    os.makedirs(os.path.dirname(sock_path), exist_ok=True)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(8)
    print(f"rerank-serve: listening on {sock_path}", flush=True)
    try:
        while True:
            conn, _ = srv.accept()
            # The host opens a connection per request and may close it early
            # (its read timed out). A disconnect must end that connection, not
            # the server — catch the pipe errors and go back to accept().
            try:
                with conn, conn.makefile("rwb") as f:
                    for raw in f:
                        raw = raw.strip()
                        if not raw:
                            continue
                        try:
                            req = json.loads(raw)
                            resp = handle(reranker, req)
                            _log_request(req, resp)
                        except Exception as exc:  # bad request -> error, keep connection
                            resp = {"error": str(exc)}
                            print(f"req: ERROR {exc}", flush=True)
                        f.write((json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8"))
                        f.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                continue
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        if os.path.exists(sock_path):
            os.unlink(sock_path)


def main():
    default_sock = os.path.join(
        os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
        "modore", "reranker.sock")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default=default_sock)
    ap.add_argument("--history-weight", type=float, default=0.0)
    args = ap.parse_args()
    try:
        serve(args.socket, args.history_weight)
    except RuntimeError as exc:  # BERT deps missing
        print(f"rerank-serve: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
