"""Conversion-record schema shared by the host logger and the eval harness.

One record = one sealed conversion decision. The macOS host appends these as
JSONL to `<configDir>/conversions.jsonl` when `[experiment] log_conversions =
on`; the eval harness reads the same shape. Keep the field names in lockstep
with native/macos/Pickup/ConversionLog.swift.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, Optional


@dataclass
class ConversionRecord:
    # The reading that was converted (romaji or kana, as picked up).
    reading: str
    # Mozc's candidate batch, in Mozc's order. candidates[mozc_top_idx] is what
    # modore committed first.
    candidates: list[str]
    # Index into `candidates` the user actually settled on at session-seal.
    #   0..N-1 -> that candidate (>0 means the user cycled away from Mozc's top)
    #   -1     -> Esc/undo: none of the candidates; wanted the raw reading
    decided_idx: int
    mozc_top_idx: int = 0
    app_id: Optional[str] = None
    decided_value: Optional[str] = None
    # Surrounding field text. Populated on the AX path only (clipboard/scripted
    # paths lack a stable field handle) -> may be None.
    context_before: Optional[str] = None
    context_after: Optional[str] = None
    backing: Optional[str] = None
    ts: Optional[int] = None  # epoch millis
    id: Optional[str] = None  # session uuid; later log lines for an id supersede earlier ones

    @property
    def is_correction(self) -> bool:
        """User cycled away from Mozc's top -> the case a reranker must win."""
        return self.decided_idx >= 0 and self.decided_idx != self.mozc_top_idx

    @property
    def is_undo(self) -> bool:
        return self.decided_idx < 0

    @property
    def gold_value(self) -> Optional[str]:
        if 0 <= self.decided_idx < len(self.candidates):
            return self.candidates[self.decided_idx]
        return self.decided_value

    @classmethod
    def from_json(cls, obj: dict) -> "ConversionRecord":
        return cls(
            reading=obj["reading"],
            candidates=list(obj["candidates"]),
            decided_idx=int(obj["decided_idx"]),
            mozc_top_idx=int(obj.get("mozc_top_idx", 0)),
            app_id=obj.get("app_id"),
            decided_value=obj.get("decided_value"),
            context_before=obj.get("context_before"),
            context_after=obj.get("context_after"),
            backing=obj.get("backing"),
            ts=obj.get("ts"),
            id=obj.get("id"),
        )

    def to_json(self) -> dict:
        return {
            "id": self.id,
            "ts": self.ts,
            "app_id": self.app_id,
            "reading": self.reading,
            "candidates": self.candidates,
            "mozc_top_idx": self.mozc_top_idx,
            "decided_idx": self.decided_idx,
            "decided_value": self.decided_value or self.gold_value,
            "context_before": self.context_before,
            "context_after": self.context_after,
            "backing": self.backing,
        }


def load_jsonl(path: str | Path) -> list[ConversionRecord]:
    """Load + lightly validate a JSONL conversion log. Skips blank/comment
    lines and records with an empty candidate list (nothing to rerank).

    The host overwrites a session's history entry as the user cycles/undos, so
    the log can hold several lines for one session id (the trajectory). We keep
    only the LAST line per id — the final decision — preserving its original
    position. Records without an id (e.g. the seed set) are kept as-is."""
    raw: list[ConversionRecord] = []
    p = Path(path)
    with p.open("r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                rec = ConversionRecord.from_json(json.loads(line))
            except (json.JSONDecodeError, KeyError) as exc:
                raise ValueError(f"{p}:{lineno}: bad record: {exc}") from exc
            if not rec.candidates:
                continue
            raw.append(rec)

    latest_pos: dict[str, int] = {}
    for i, rec in enumerate(raw):
        if rec.id is not None:
            latest_pos[rec.id] = i
    out: list[ConversionRecord] = []
    for i, rec in enumerate(raw):
        if rec.id is not None and latest_pos[rec.id] != i:
            continue  # superseded by a later line for the same session
        out.append(rec)
    return out


def iter_with_history(
    records: list[ConversionRecord], window: int
) -> Iterator[tuple[ConversionRecord, list[ConversionRecord]]]:
    """Replay records the way a live reranker sees them: in timestamp order,
    each yielded with the last `window` *prior* sealed decisions from the SAME
    app (most-recent-last). This is the per-app history the host keeps."""
    by_app: dict[Optional[str], list[ConversionRecord]] = {}
    ordered = sorted(records, key=lambda r: (r.ts if r.ts is not None else 0))
    for rec in ordered:
        prior = by_app.get(rec.app_id, [])
        hist = prior[-window:] if window > 0 else []
        yield rec, hist
        by_app.setdefault(rec.app_id, []).append(rec)
