"""Metrics that decide whether reranking is a net win.

Top-1 accuracy alone hides the story, because Mozc's top is already correct
most of the time. The two numbers that matter:

  correction recall  - among records where the user cycled AWAY from Mozc's
                       top (decided_idx != mozc_top_idx), how often the
                       reranker lifts the gold candidate to position 0. This
                       is the upside.

  regression rate    - among records where Mozc's top was ALREADY right
                       (decided_idx == mozc_top_idx), how often the reranker
                       wrongly displaces it. This is the downside; it must stay
                       near zero or the feature is net-negative.

`net_lift` = reranker top-1 - baseline (Mozc-order) top-1, over rerankable
records (undo records are excluded from accuracy and reported separately).
"""

from __future__ import annotations

from dataclasses import dataclass

from schema import ConversionRecord


@dataclass
class EvalResult:
    name: str
    n_total: int = 0
    n_rerankable: int = 0      # decided_idx >= 0
    n_undo: int = 0
    top1_hits: int = 0         # reranker[0] == decided_idx
    n_corrections: int = 0     # decided_idx != mozc_top_idx (and >= 0)
    correction_hits: int = 0   # corrections the reranker put on top
    n_already_top: int = 0     # decided_idx == mozc_top_idx
    regressions: int = 0       # already-top cases the reranker displaced
    total_latency_ms: float = 0.0
    n_latency: int = 0

    def observe(self, rec: ConversionRecord, ranked: list[int], latency_ms: float = 0.0) -> None:
        self.n_total += 1
        if latency_ms:
            self.total_latency_ms += latency_ms
            self.n_latency += 1
        if rec.is_undo:
            self.n_undo += 1
            return
        self.n_rerankable += 1
        top = ranked[0] if ranked else rec.mozc_top_idx
        hit = top == rec.decided_idx
        if hit:
            self.top1_hits += 1
        if rec.is_correction:
            self.n_corrections += 1
            if hit:
                self.correction_hits += 1
        else:  # decided_idx == mozc_top_idx: Mozc was already right
            self.n_already_top += 1
            if top != rec.mozc_top_idx:
                self.regressions += 1

    @property
    def top1_acc(self) -> float:
        return self.top1_hits / self.n_rerankable if self.n_rerankable else 0.0

    @property
    def correction_recall(self) -> float:
        return self.correction_hits / self.n_corrections if self.n_corrections else 0.0

    @property
    def regression_rate(self) -> float:
        return self.regressions / self.n_already_top if self.n_already_top else 0.0

    @property
    def avg_latency_ms(self) -> float:
        return self.total_latency_ms / self.n_latency if self.n_latency else 0.0


def format_table(results: list[EvalResult], baseline_acc: float) -> str:
    rows = [
        (
            r.name,
            f"{r.top1_acc:6.1%}",
            f"{(r.top1_acc - baseline_acc):+6.1%}",
            f"{r.correction_recall:6.1%} ({r.correction_hits}/{r.n_corrections})",
            f"{r.regression_rate:6.1%} ({r.regressions}/{r.n_already_top})",
            f"{r.avg_latency_ms:6.1f}ms" if r.n_latency else "     -",
        )
        for r in results
    ]
    head = ("reranker", "top1", "lift", "correction-recall", "regression", "lat/call")
    widths = [max(len(h), *(len(row[i]) for row in rows)) for i, h in enumerate(head)]
    line = lambda cols: "  ".join(c.ljust(widths[i]) for i, c in enumerate(cols))
    out = [line(head), line(["-" * w for w in widths])]
    out += [line(row) for row in rows]
    return "\n".join(out)
