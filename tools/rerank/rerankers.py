"""Candidate rerankers, smallest to largest.

A reranker takes one ConversionRecord plus the per-app history (prior sealed
decisions, most-recent-last) and returns the candidate indices in its preferred
order. Position 0 is the pick. The harness scores rank()[0] against the user's
settled choice.

  B0  MozcOrder        do nothing (the floor)
  B1  LastDecided      per-app reading -> last decided value (pure MRU)
  B2  FrequencyPrior   per-app reading -> most-decided value
  R1  BertBrute        BERT pseudo-log-likelihood over (before + cand + after)
  R2  BertPreempt      BERT predicts the slot once, candidates read off it

B1/B2 isolate how much a *cheap* history prior buys before any neural cost.
R1/R2 are the two approaches to compare: brute rescoring vs. predict-then-match
(the latter is the one that can be precomputed off the latency path).
"""

from __future__ import annotations

import re
from collections import Counter, defaultdict

from schema import ConversionRecord


class Reranker:
    name = "base"

    def rank(self, rec: ConversionRecord, history: list[ConversionRecord]) -> list[int]:
        raise NotImplementedError


class MozcOrder(Reranker):
    name = "B0 mozc-order"

    def rank(self, rec, history):
        return list(range(len(rec.candidates)))


def _history_decisions(history: list[ConversionRecord], reading: str):
    """Gold values previously settled for this exact reading, oldest-first."""
    return [
        h.gold_value
        for h in history
        if h.reading == reading and not h.is_undo and h.gold_value
    ]


def _stable_promote(candidates: list[str], preferred: list[str]) -> list[int]:
    """Return indices with `preferred` values pulled to the front (in the order
    given), the rest keeping Mozc's order. Values not in candidates are ignored."""
    idx_of = {}
    for i, c in enumerate(candidates):
        idx_of.setdefault(c, i)
    front, seen = [], set()
    for v in preferred:
        i = idx_of.get(v)
        if i is not None and i not in seen:
            front.append(i)
            seen.add(i)
    rest = [i for i in range(len(candidates)) if i not in seen]
    return front + rest


class LastDecided(Reranker):
    name = "B1 last-decided"

    def rank(self, rec, history):
        prev = _history_decisions(history, rec.reading)
        return _stable_promote(rec.candidates, list(reversed(prev)))


class FrequencyPrior(Reranker):
    name = "B2 freq-prior"

    def rank(self, rec, history):
        prev = _history_decisions(history, rec.reading)
        if not prev:
            return list(range(len(rec.candidates)))
        counts = Counter(prev)
        ordered = [v for v, _ in counts.most_common()]
        return _stable_promote(rec.candidates, ordered)


# --- BERT rerankers -------------------------------------------------------
# torch/transformers are heavy and opt-in. Import lazily so baselines run on a
# bare interpreter; only the BERT rerankers require `make rerank-setup`.

_MODEL_NAME = "cl-tohoku/bert-base-japanese-v3"


class _BertBackend:
    _instance = None

    def __init__(self, model_name: str = _MODEL_NAME, device: str | None = None):
        try:
            import torch
            from transformers import AutoModelForMaskedLM, AutoTokenizer
        except ImportError as exc:
            raise RuntimeError(
                "BERT rerankers need torch + transformers + fugashi + unidic-lite.\n"
                "Run `make rerank-setup` (creates tools/rerank/.venv) or install "
                "into your interpreter."
            ) from exc
        self.torch = torch
        if device is None:
            device = (
                "mps" if torch.backends.mps.is_available()
                else "cuda" if torch.cuda.is_available()
                else "cpu"
            )
        self.device = device
        self.tokenizer = AutoTokenizer.from_pretrained(model_name)
        self.model = AutoModelForMaskedLM.from_pretrained(model_name).to(device).eval()
        self.mask_id = self.tokenizer.mask_token_id

    @classmethod
    def shared(cls) -> "_BertBackend":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance


def _wrap_history_context(rec: ConversionRecord, history: list[ConversionRecord], weight: float) -> str:
    """Light history conditioning: prepend recently-decided surface forms from
    the same app as a short pseudo-context the LM can lean on. weight<=0 -> off."""
    if weight <= 0 or not history:
        return rec.context_before or ""
    recent = [h.gold_value for h in history[-3:] if h.gold_value and not h.is_undo]
    prefix = "".join(recent)
    return prefix + (rec.context_before or "")


class BertBrute(Reranker):
    """R1: score each candidate by pseudo-log-likelihood of the full sentence
    `before + candidate + after`, masking each candidate token in turn. Uses
    BOTH sides of context (modore has the suffix; ibus-hiragana only had the
    prefix). Slow but the most faithful per-candidate signal."""

    name = "R1 bert-brute"

    def __init__(self, history_weight: float = 0.0):
        self.history_weight = history_weight
        self.be = _BertBackend.shared()

    def _pll(self, before: str, candidate: str, after: str) -> float:
        torch = self.be.torch
        tk = self.be.tokenizer
        b = tk.encode(before, add_special_tokens=False) if before else []
        c = tk.encode(candidate, add_special_tokens=False)
        a = tk.encode(after, add_special_tokens=False) if after else []
        if not c:
            return float("-inf")
        cls, sep = tk.cls_token_id, tk.sep_token_id
        base = [cls] + b + c + a + [sep]
        span = range(1 + len(b), 1 + len(b) + len(c))
        total = 0.0
        for pos in span:
            ids = list(base)
            true_tok = ids[pos]
            ids[pos] = self.be.mask_id
            with torch.no_grad():
                t = torch.tensor([ids], device=self.be.device)
                logits = self.be.model(t).logits[0, pos]
                logp = torch.log_softmax(logits, dim=-1)[true_tok].item()
            total += logp
        return total / len(c)  # length-normalized

    def rank(self, rec, history):
        before = _wrap_history_context(rec, history, self.history_weight)
        after = rec.context_after or ""
        scores = [(self._pll(before, cand, after), i) for i, cand in enumerate(rec.candidates)]
        scores.sort(key=lambda s: (-s[0], s[1]))
        return [i for _, i in scores]


class BertPreempt(Reranker):
    """R2: mask the slot once and read each candidate's probability off the
    single prediction (product of its sub-token probs). One forward pass for
    the slot instead of one-per-candidate-token -> precomputable before the
    candidate batch is even known. Approximate for multi-token candidates."""

    name = "R2 bert-preempt"

    def __init__(self, history_weight: float = 0.0):
        self.history_weight = history_weight
        self.be = _BertBackend.shared()

    def _scores(self, rec, history) -> list[float]:
        torch = self.be.torch
        tk = self.be.tokenizer
        before = _wrap_history_context(rec, history, self.history_weight)
        after = rec.context_after or ""
        toks = [tk.encode(c, add_special_tokens=False) for c in rec.candidates]
        max_len = max((len(t) for t in toks), default=1) or 1
        b = tk.encode(before, add_special_tokens=False) if before else []
        a = tk.encode(after, add_special_tokens=False) if after else []
        cls, sep = tk.cls_token_id, tk.sep_token_id
        ids = [cls] + b + [self.be.mask_id] * max_len + a + [sep]
        slot = list(range(1 + len(b), 1 + len(b) + max_len))
        with torch.no_grad():
            t = torch.tensor([ids], device=self.be.device)
            logits = self.be.model(t).logits[0]
            logp = torch.log_softmax(logits, dim=-1)
        out = []
        for ct in toks:
            if not ct:
                out.append(float("-inf"))
                continue
            out.append(sum(logp[slot[j], tok].item() for j, tok in enumerate(ct[:max_len])) / len(ct))
        return out

    def rank(self, rec, history):
        return self.rank_scored(rec, history)[0]

    def rank_scored(self, rec, history):
        """Return (order, margin, scores). `margin` is the top-1 minus top-2
        score gap (log-prob units) — the live path gates auto-override on it
        so BERT only displaces Mozc's pick when it's confidently better."""
        s = self._scores(rec, history)
        order = sorted(range(len(s)), key=lambda i: (-s[i], i))
        margin = (s[order[0]] - s[order[1]]) if len(order) > 1 else float("inf")
        return order, margin, s


def build(names: list[str], history_weight: float = 0.0) -> list[Reranker]:
    table = {
        "B0": MozcOrder,
        "B1": LastDecided,
        "B2": FrequencyPrior,
        "R1": lambda: BertBrute(history_weight=history_weight),
        "R2": lambda: BertPreempt(history_weight=history_weight),
    }
    out = []
    for n in names:
        key = n.upper()
        if key not in table:
            raise SystemExit(f"unknown reranker {n!r}; pick from {sorted(table)}")
        out.append(table[key]())
    return out
