#!/usr/bin/env python3
"""Train the romaji/ASCII n-gram classifier for modore.

Data sources:
  - Mozc OSS dictionary (hiragana readings → romaji via Mozc's own table)
  - Mozc UT dictionaries (third_party/merge-ut-dictionaries)
  - Japanese Wikipedia article titles
  - System English dictionary (/usr/share/dict/words)
  - COCA frequency list
  - UD_Japanese-GSD sentence data (CoNLL-U)
  - Tatoeba Japanese sentences (via fugashi tokenizer)

Generates: engine/models/classifier.mdl

Usage:
    python3 tools/train_classifier.py
    python3 tools/train_classifier.py --out model.mdl --epochs 40
"""

import argparse
import functools
import random
import struct
import sys
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
MOZC_DICT_DIR = REPO_ROOT / "third_party/fcitx5-mozc/mozc/src/data/dictionary_oss"
MOZC_ROMAJI_TABLE = (
    REPO_ROOT
    / "third_party/fcitx5-mozc/mozc/src/data/preedit/romanji-hiragana.tsv"
)
UT_DICT_DIR = (
    REPO_ROOT / "third_party/merge-ut-dictionaries/src/merge"
)
DATA_DIR = REPO_ROOT / "tools" / "data"
JAWIKI_TITLES = DATA_DIR / "jawiki-titles"
COCA_FREQ = DATA_DIR / "coca-frequency.txt"
UD_GSD_DIR = DATA_DIR  # contains ja_gsd-ud-{train,dev,test}.conllu
TATOEBA_FILE = DATA_DIR / "tatoeba-jpn.tsv"
CLASSIFIER_EVAL = DATA_DIR / "classifier-eval.tsv"
MIXED_TEXT_DIRS: list[Path] = [
    Path("/tmp/js-primer/source"),
    Path("/tmp/progit/ja"),
    Path("/tmp/Hatena-Textbook"),
    Path("/tmp/mdn-translated-content/files/ja"),
]
SYSTEM_DICT = Path("/usr/share/dict/words")
TECH_DICT_FILES: list[Path] = [
    DATA_DIR / "software-terms.txt",
    DATA_DIR / "software-tools.txt",
    DATA_DIR / "computing-acronyms.txt",
]
MODEL_FILENAME = "classifier.mdl"
DEFAULT_OUTPUT = REPO_ROOT / "engine/models" / MODEL_FILENAME
DICT_OUTPUT = REPO_ROOT / "engine/models/english_dict.txt"
CACHE_DIR = DATA_DIR / "cache"
TRAINING_DATA_VERSION = 7

# ---------------------------------------------------------------------------
# Model hyper-parameters (must match mdr_classifier.h / classifier.c)
# ---------------------------------------------------------------------------

N_BUCKETS = 32768
NGRAM_MAX = 4
WINDOW = 4
HISTORY = 3
MAGIC = b"MDRC"
VERSION = 1
THRESHOLD = 0.55

# Feature count per position:
#   2 types (surface+type) * (2*WINDOW+1) offsets * NGRAM_MAX n-gram sizes
#   + 1 romaji-validity feature
#   + HISTORY history label features
#   + 2 dictionary features (word boundary + prefix)
MAX_FEAT_PAIRS = 2 * (2 * WINDOW + 1) * NGRAM_MAX  # = 72
MAX_FEATS = MAX_FEAT_PAIRS + 1 + HISTORY + 2  # +1 validity +3 history +2 dict

# ---------------------------------------------------------------------------
# FNV-1a hash (must match C implementation)
# ---------------------------------------------------------------------------


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def hash_feature(key: str) -> int:
    return fnv1a(key.encode("ascii")) % N_BUCKETS


# ---------------------------------------------------------------------------
# Character type (must match C)
# ---------------------------------------------------------------------------


def char_type(c: str) -> str:
    if "a" <= c <= "z":
        return "L"
    if "A" <= c <= "Z":
        return "U"
    if "0" <= c <= "9":
        return "N"
    return "S"


# ---------------------------------------------------------------------------
# Romaji validity trie (built from Mozc's own romaji table)
# ---------------------------------------------------------------------------

_ROMAJI_VALID: set[str] = set()
_ROMAJI_PREFIXES: set[str] = set()


def _build_romaji_trie(table_path: Path) -> None:
    """Populate _ROMAJI_VALID and _ROMAJI_PREFIXES from Mozc's table."""
    global _ROMAJI_VALID, _ROMAJI_PREFIXES
    with open(table_path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            rom = parts[0]
            if not rom or not rom.isalpha() or not rom.isascii():
                continue
            rom_lower = rom.lower()
            _ROMAJI_VALID.add(rom_lower)
            for i in range(1, len(rom_lower) + 1):
                _ROMAJI_PREFIXES.add(rom_lower[:i])


def romaji_coverage(text: str) -> list[bool]:
    """Greedily parse `text` as romaji (longest match). Returns a bool
    per character: True if covered by a matched romaji syllable."""
    text_lower = text.lower()
    n = len(text_lower)
    covered = [False] * n
    i = 0
    while i < n:
        best = 0
        for length in range(min(4, n - i), 0, -1):
            chunk = text_lower[i : i + length]
            if chunk in _ROMAJI_VALID:
                best = length
                break
            if chunk == "n" and length == 1:
                if i + 1 >= n or text_lower[i + 1] not in "aiueo":
                    best = 1
                    break
            # Doubled consonant → っ (e.g. tt, kk, ss, pp)
            if length == 1 and chunk[0].isalpha() and chunk[0] != "n":
                if i + 1 < n and text_lower[i + 1] == chunk[0]:
                    best = 1
                    break
        if best > 0:
            for j in range(i, i + best):
                covered[j] = True
            i += best
        else:
            i += 1
    return covered


def _romaji_fully_covered(text: str) -> bool:
    return bool(text) and all(romaji_coverage(text))


def _has_strong_romaji_marker(text: str) -> bool:
    markers = (
        "sh", "ch", "ts", "ty", "sy", "zy", "jy", "dy", "fy", "vy",
        "kw", "gw", "wh", "nn", "kk", "ss", "tt", "pp", "cc", "jj",
        "mm", "rr", "yy", "ww", "xx", "ll",
    )
    return any(marker in text for marker in markers)


def _forced_ascii_prefix_len(run: str) -> int:
    """Return the length of a lowercase ASCII prefix that should be forced
    to ASCII, leaving a romaji-parseable suffix to be scored as a fresh
    segment.

    We only split lowercase runs here. Uppercase / digit / symbol handling
    already has a hard rule in the classifier, so this pass is for the
    problematic lowercase-only cases such as `vrtyatto` or
    `dockerdetukutta`.
    """

    if len(run) < 5 or _romaji_fully_covered(run):
        return 0

    for cut in range(1, min(4, len(run) - 4)):
        prefix = run[:cut]
        suffix = run[cut:]
        if (_romaji_fully_covered(suffix)
                and not _romaji_fully_covered(prefix)
                and _has_strong_romaji_marker(suffix)):
            return cut
    return 0


# ---------------------------------------------------------------------------
# Feature extraction (mirrors classifier.c)
# ---------------------------------------------------------------------------


_ENGLISH_WORDS: set[str] = set()
_ENGLISH_PREFIXES: set[str] = set()


def _build_english_lookup(words: list[str]) -> None:
    """Build word and prefix sets for dictionary features."""
    global _ENGLISH_WORDS, _ENGLISH_PREFIXES
    _ENGLISH_WORDS = set(w.lower() for w in words
                         if w.isascii() and w.isalpha())
    prefixes: set[str] = set()
    for w in _ENGLISH_WORDS:
        for i in range(1, len(w)):
            prefixes.add(w[:i])
    _ENGLISH_PREFIXES = prefixes


def _ascii_run_at(text: str, pos: int) -> str:
    """Return the lowercase ASCII run ending at pos (inclusive)."""
    end = pos + 1
    start = pos
    while start > 0 and text[start - 1].isalpha() and text[start - 1].isascii():
        start -= 1
    return text[start:end].lower()


def extract_feature_indices(text: str, pos: int,
                            validity: list[bool] | None = None,
                            history: list[int] | None = None) -> list[int]:
    """Return list of bucket indices for features at `pos` in `text`."""
    indices: list[int] = []
    length = len(text)

    for off in range(-WINDOW, WINDOW + 1):
        for n in range(1, NGRAM_MAX + 1):
            start = pos + off
            end = start + n
            if start < 0 or end > length:
                indices.extend([-1, -1])
                continue

            gram = text[start:end]
            indices.append(hash_feature(f"S:{off}/{gram}"))

            tgram = "".join(char_type(c) for c in gram)
            indices.append(hash_feature(f"T:{off}/{tgram}"))

    # Romaji-validity feature
    if validity is not None and pos < len(validity) and validity[pos]:
        indices.append(hash_feature("V:covered"))
    else:
        indices.append(-1)

    # History features: n-grams over previous labels (K=kana, A=ASCII)
    for n in range(1, HISTORY + 1):
        if history is not None and len(history) >= n:
            hgram = "".join(
                "K" if history[-(n - j)] else "A"
                for j in range(n)
            )
            indices.append(hash_feature(f"H:{-n}/{hgram}"))
        else:
            indices.append(-1)

    # Dictionary features: does the ASCII run ending here form a word / prefix?
    if _ENGLISH_WORDS:
        run = _ascii_run_at(text, pos)
        if run in _ENGLISH_WORDS:
            indices.append(hash_feature("D:word"))
        else:
            indices.append(-1)
        if run in _ENGLISH_PREFIXES or run in _ENGLISH_WORDS:
            indices.append(hash_feature("D:prefix"))
        else:
            indices.append(-1)
    else:
        indices.extend([-1, -1])

    return indices


# ---------------------------------------------------------------------------
# Build kana → romaji table from Mozc
# ---------------------------------------------------------------------------


def load_kana_to_romaji(path: Path) -> dict[str, str]:
    kana2rom: dict[str, str] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            romaji, kana = parts[0], parts[1]
            if not romaji or not all(c.isascii() and c.isalpha() for c in romaji):
                continue
            if kana not in kana2rom or len(romaji) < len(kana2rom[kana]):
                kana2rom[kana] = romaji
    return kana2rom


def hiragana_to_romaji(reading: str, table: dict[str, str]) -> str | None:
    result: list[str] = []
    i = 0
    while i < len(reading):
        matched = False
        for length in range(min(4, len(reading) - i), 0, -1):
            chunk = reading[i : i + length]
            if chunk in table:
                result.append(table[chunk])
                i += length
                matched = True
                break
        if not matched:
            return None
    return "".join(result)


# ---------------------------------------------------------------------------
# Smarter kana-to-romaji for sentence data (handles っ and ー)
# ---------------------------------------------------------------------------

_KATAKANA_OFFSET = ord("ア") - ord("あ")


def _kata_to_hira(s: str) -> str:
    out: list[str] = []
    for ch in s:
        cp = ord(ch)
        if 0x30A1 <= cp <= 0x30F6:
            out.append(chr(cp - _KATAKANA_OFFSET))
        else:
            out.append(ch)
    return "".join(out)


def smart_hira_to_romaji(
    reading: str, table: dict[str, str]
) -> str | None:
    """Like hiragana_to_romaji but handles っ (consonant doubling) and
    ー (long vowel) which appear in UD/dictionary katakana readings."""
    result: list[str] = []
    i = 0
    while i < len(reading):
        ch = reading[i]
        if ch == "ー":
            if result:
                for c in reversed(result[-1]):
                    if c in "aiueo":
                        result.append(c)
                        break
            i += 1
            continue
        if ch == "っ" and i + 1 < len(reading):
            next_rom = None
            for length in range(min(4, len(reading) - i - 1), 0, -1):
                chunk = reading[i + 1 : i + 1 + length]
                if chunk in table:
                    next_rom = table[chunk]
                    break
            if next_rom and next_rom[0].isalpha():
                result.append(next_rom[0])
                i += 1
                continue
        matched = False
        for length in range(min(4, len(reading) - i), 0, -1):
            chunk = reading[i : i + length]
            if chunk in table:
                result.append(table[chunk])
                i += length
                matched = True
                break
        if not matched:
            return None
    return "".join(result)


# ---------------------------------------------------------------------------
# Sentence data: UD_Japanese-GSD + Tatoeba (via fugashi)
# ---------------------------------------------------------------------------


def _parse_ud_conllu(
    path: Path, kana2rom: dict[str, str]
) -> list[list[tuple[str, bool]]]:
    """Parse CoNLL-U file → list of sentences.
    Each sentence is a list of (romaji_word, is_content) tuples.
    is_content=True for content words, False for particles/auxiliary."""
    sentences: list[list[tuple[str, bool]]] = []
    cur_tokens: list[tuple[str, bool]] = []

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line == "":
                if cur_tokens:
                    sentences.append(cur_tokens)
                cur_tokens = []
            elif line.startswith("#"):
                continue
            else:
                parts = line.split("\t")
                if len(parts) < 10 or "-" in parts[0]:
                    continue
                upos = parts[3]
                xpos = parts[4]
                misc = parts[9]

                if upos in ("PUNCT", "SYM"):
                    continue

                reading = None
                for field in misc.split("|"):
                    if field.startswith("UnidicInfo="):
                        reading = field[len("UnidicInfo="):].split(",")[0]
                        break
                if not reading or not reading.strip():
                    continue

                hira = _kata_to_hira(reading)
                rom = smart_hira_to_romaji(hira, kana2rom)
                if not rom or not rom.isalpha() or not rom.islower():
                    continue

                is_content = upos not in (
                    "ADP", "AUX", "SCONJ", "CCONJ", "PART",
                ) and not xpos.startswith("助詞") and not xpos.startswith("助動詞")
                cur_tokens.append((rom, is_content))

    if cur_tokens:
        sentences.append(cur_tokens)
    return sentences


def load_ud_sentences(
    data_dir: Path, kana2rom: dict[str, str]
) -> list[list[tuple[str, bool]]]:
    """Load all UD_Japanese-GSD CoNLL-U files → romaji sentence data."""
    all_sents: list[list[tuple[str, bool]]] = []
    for name in ["ja_gsd-ud-train.conllu", "ja_gsd-ud-dev.conllu",
                  "ja_gsd-ud-test.conllu"]:
        p = data_dir / name
        if p.exists():
            sents = _parse_ud_conllu(p, kana2rom)
            all_sents.extend(sents)
            print(f"    {name}: {len(sents):,} sentences")
    return [s for s in all_sents if len(s) >= 3]


def load_tatoeba_sentences(
    path: Path, kana2rom: dict[str, str]
) -> list[list[tuple[str, bool]]]:
    """Load Tatoeba Japanese sentences, tokenize with fugashi, convert to
    romaji word lists."""
    if not path.exists():
        print(f"    {path} not found — skipping")
        return []

    try:
        import fugashi
    except ImportError:
        print("    fugashi not installed — skipping Tatoeba")
        return []

    tagger = fugashi.Tagger()
    sentences: list[list[tuple[str, bool]]] = []
    n_lines = 0

    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            text = parts[2]
            if not text or len(text) < 4 or len(text) > 200:
                continue
            n_lines += 1

            tokens: list[tuple[str, bool]] = []
            for word in tagger(text):
                feat = word.feature
                reading = getattr(feat, "kana", None) or getattr(feat, "pron", None)
                if not reading or reading == "*":
                    continue

                hira = _kata_to_hira(reading)
                rom = smart_hira_to_romaji(hira, kana2rom)
                if not rom or not rom.isalpha() or not rom.islower():
                    continue

                pos1 = getattr(feat, "pos1", "") or ""
                is_content = pos1 not in ("助詞", "助動詞", "記号", "補助記号")
                tokens.append((rom, is_content))

            if len(tokens) >= 3:
                sentences.append(tokens)

    print(f"    Tatoeba: {len(sentences):,} usable sentences "
          f"from {n_lines:,} lines")
    return sentences


# ---------------------------------------------------------------------------
# Mixed-language text extraction (programming docs, tech books)
# ---------------------------------------------------------------------------

_ASCII_TOKEN_CHARS = set("._+-/#&@")
_TEXT_EXTENSIONS = {".md", ".markdown", ".mdx", ".txt", ".rst", ".html"}


def _is_ascii_token_char(ch: str) -> bool:
    return ch.isascii() and (ch.isalnum() or ch in _ASCII_TOKEN_CHARS)


def _trim_ascii_token(token: str) -> str:
    return token.strip("._+-/#&@")


def _extract_mixed_sequences(
    text: str,
    kana2rom: dict[str, str],
    tagger: object,
) -> list[tuple[str, list[int]]]:
    """Extract training sequences from text that naturally mixes ASCII and
    Japanese. Returns list of (romaji_text, char_labels) pairs.

    Japanese characters → romaji via fugashi readings (label=1).
    ASCII technical-token runs → kept as-is (label=0).
    Lines with no mixing or too short are skipped.
    """
    results: list[tuple[str, list[int]]] = []

    for line in text.split("\n"):
        line = line.strip()
        if len(line) < 10:
            continue
        # Skip markdown headers, code blocks, URLs
        if line.startswith("#") or line.startswith("```") or line.startswith("http"):
            continue

        # Split into runs of ASCII technical tokens vs Japanese (U+3000+).
        # Dots/slashes/pluses stay inside tokens so real docs produce
        # labels like `Node.js`, `C++`, `S3`, `R&D`, and `HTTP/2`.
        runs: list[tuple[str, str]] = []
        i = 0
        while i < len(line):
            ch = line[i]
            if _is_ascii_token_char(ch):
                j = i
                while j < len(line) and _is_ascii_token_char(line[j]):
                    j += 1
                token = _trim_ascii_token(line[i:j])
                if token and any(c.isalnum() for c in token):
                    runs.append(("ascii", token))
                i = j
            elif ord(ch) >= 0x3000:
                j = i
                while j < len(line) and ord(line[j]) >= 0x3000:
                    j += 1
                runs.append(("ja", line[i:j]))
                i = j
            else:
                i += 1

        # Need at least one ASCII and one Japanese run
        has_ascii = any(t == "ascii" for t, _ in runs)
        has_ja = any(t == "ja" for t, _ in runs)
        if not has_ascii or not has_ja:
            continue
        ja_chars = sum(len(txt) for typ, txt in runs if typ == "ja")
        ascii_chars = sum(len(txt) for typ, txt in runs if typ == "ascii")
        if ja_chars < ascii_chars:
            continue

        seq_text = ""
        seq_labels: list[int] = []
        skip = False
        for typ, txt in runs:
            if typ == "ascii":
                seq_text += txt
                seq_labels.extend([0] * len(txt))
            else:
                romaji_parts: list[str] = []
                for word in tagger(txt):
                    reading = getattr(word.feature, "kana", None) or getattr(
                        word.feature, "pron", None
                    )
                    if not reading or reading == "*":
                        continue
                    hira = _kata_to_hira(reading)
                    rom = smart_hira_to_romaji(hira, kana2rom)
                    if rom and rom.isalpha() and rom.islower():
                        romaji_parts.append(rom)
                ja_rom = "".join(romaji_parts)
                if not ja_rom:
                    skip = True
                    break
                seq_text += ja_rom
                seq_labels.extend([1] * len(ja_rom))

        if skip or len(seq_text) < 8 or len(seq_text) > 200:
            continue
        results.append((seq_text, seq_labels))

    return results


def load_mixed_text_sequences(
    dirs: list[Path], kana2rom: dict[str, str]
) -> list[tuple[str, list[int]]]:
    """Load and convert mixed Japanese/English tech text into labeled
    training sequences. Each sequence has char-level labels:
    0=ASCII, 1=romaji."""
    try:
        import fugashi
    except ImportError:
        print("    fugashi not installed — skipping mixed text")
        return []

    tagger = fugashi.Tagger()
    all_seqs: list[tuple[str, list[int]]] = []

    for d in dirs:
        if not d.exists():
            continue
        n_before = len(all_seqs)
        files = [p for p in d.rglob("*") if p.is_file() and p.suffix.lower() in _TEXT_EXTENSIONS]
        for md_file in sorted(files):
            try:
                text = md_file.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            seqs = _extract_mixed_sequences(text, kana2rom, tagger)
            all_seqs.extend(seqs)
        added = len(all_seqs) - n_before
        if added:
            print(f"    {d.name}: {added:,} mixed sequences")

    print(f"    Total mixed text sequences: {len(all_seqs):,}")
    return all_seqs


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------


def _extract_romaji_from_dict(
    path: Path, kana2rom: dict[str, str], out: set[str]
) -> int:
    added = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4:
                continue
            hiragana = parts[0]
            if len(hiragana) < 2 or len(hiragana) > 15:
                continue
            rom = hiragana_to_romaji(hiragana, kana2rom)
            if rom and 2 <= len(rom) <= 25 and rom.isalpha() and rom.islower():
                if rom not in out:
                    out.add(rom)
                    added += 1
    return added


def load_mozc_romaji_words(
    dict_dir: Path, kana2rom: dict[str, str],
    ut_dict_dir: Path | None = None,
) -> list[str]:
    readings: set[str] = set()

    for txt in sorted(dict_dir.glob("dictionary*.txt")):
        n = _extract_romaji_from_dict(txt, kana2rom, readings)
        print(f"    {txt.name}: +{n:,} (total {len(readings):,})")

    if ut_dict_dir and ut_dict_dir.exists():
        for txt in sorted(ut_dict_dir.glob("mozcdic-ut-*/mozcdic-ut-*.txt")):
            n = _extract_romaji_from_dict(txt, kana2rom, readings)
            print(f"    {txt.parent.name}/{txt.name}: +{n:,} (total {len(readings):,})")

    result = list(readings)
    random.shuffle(result)
    return result


def _load_tech_words() -> set[str]:
    """Load tech vocabulary from cspell software-terms dictionaries."""
    words: set[str] = set()
    for path in TECH_DICT_FILES:
        if not path.exists():
            continue
        with open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                w = line.strip()
                if w and not w.startswith("#") and w.isascii() and w.isalpha():
                    words.add(w.lower())
    return words


def load_english_words(
    dict_path: Path, coca_path: Path | None = None,
) -> list[str]:
    words: set[str] = set()

    if dict_path.exists():
        with open(dict_path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                w = line.strip()
                if 3 <= len(w) <= 20 and w.isascii() and w.isalpha():
                    words.add(w.lower())
        print(f"    System dict: {len(words):,} words")
    else:
        print(f"    {dict_path} not found — using fallback word list")
        words.update(w.lower() for w in _fallback_english())

    if coca_path and coca_path.exists():
        n_before = len(words)
        with open(coca_path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    w = parts[1].strip()
                    if 3 <= len(w) <= 20 and w.isascii() and w.isalpha():
                        words.add(w.lower())
        print(f"    COCA freq list: +{len(words) - n_before:,} words")

    tech = _load_tech_words()
    n_before_tech = len(words)
    words.update(tech)
    print(f"    Tech dictionaries: +{len(words) - n_before_tech:,} words")
    result = list(words)
    random.shuffle(result)
    return result


def load_jawiki_romaji(
    titles_path: Path, kana2rom: dict[str, str]
) -> list[str]:
    """Extract romaji from Japanese Wikipedia article titles."""
    if not titles_path.exists():
        print(f"    {titles_path} not found — skipping")
        return []

    readings: set[str] = set()
    katakana_offset = ord("ア") - ord("あ")

    with open(titles_path, encoding="utf-8") as f:
        for line in f:
            title = line.rstrip("\n").replace("_", "")
            if not title or len(title) < 2 or len(title) > 15:
                continue
            hiragana = ""
            all_kana = True
            for ch in title:
                cp = ord(ch)
                if 0x3041 <= cp <= 0x3096:
                    hiragana += ch
                elif 0x30A1 <= cp <= 0x30F6:
                    hiragana += chr(cp - katakana_offset)
                else:
                    all_kana = False
                    break
            if not all_kana or len(hiragana) < 2:
                continue
            rom = hiragana_to_romaji(hiragana, kana2rom)
            if rom and 2 <= len(rom) <= 25 and rom.isalpha() and rom.islower():
                readings.add(rom)

    result = list(readings)
    random.shuffle(result)
    return result


def _fallback_english() -> list[str]:
    return [
        "function", "return", "class", "object", "method", "array",
        "string", "integer", "boolean", "variable", "import", "export",
        "const", "interface", "struct", "async", "await", "promise",
        "python", "javascript", "typescript", "docker", "server",
        "client", "database", "table", "query", "index", "delete",
        "update", "select", "insert", "column", "field", "value",
        "config", "setting", "option", "parameter", "argument",
        "about", "after", "again", "below", "between", "could",
        "every", "first", "found", "great", "hello", "house",
        "large", "little", "never", "number", "other", "place",
        "point", "right", "should", "small", "sound", "still",
        "study", "their", "there", "these", "thing", "think",
        "three", "through", "together", "under", "water", "where",
        "which", "while", "world", "would", "write", "year",
    ]


# ---------------------------------------------------------------------------
# Curated classifier evaluation / hard cases
# ---------------------------------------------------------------------------


def _labels_from_expected(text: str, expected: str) -> list[int]:
    labels: list[int] = []
    rebuilt = ""
    for raw_part in expected.split("|"):
        kind, sep, value = raw_part.partition(":")
        if sep != ":" or kind not in {"A", "R"} or not value:
            raise ValueError(f"bad classifier-eval expected segment: {raw_part!r}")
        rebuilt += value
        labels.extend([1 if kind == "R" else 0] * len(value))
    if rebuilt != text:
        raise ValueError(f"classifier-eval text mismatch: {text!r} vs {rebuilt!r}")
    return labels


HardCase = tuple[str, str, list[int]]


def load_classifier_eval_cases(path: Path) -> list[HardCase]:
    if not path.exists():
        print(f"    {path} not found — no curated hard cases")
        return []
    cases: list[HardCase] = []
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 3:
                raise ValueError(f"{path}:{lineno}: expected tier, text, expected")
            tier, text, expected = fields[:3]
            if tier not in {"must", "target"}:
                raise ValueError(f"{path}:{lineno}: tier must be must or target")
            if not text.isascii():
                raise ValueError(f"{path}:{lineno}: text must be ASCII")
            cases.append((tier, text, _labels_from_expected(text, expected)))
    print(f"    Curated classifier cases: {len(cases):,}")
    return cases


# ---------------------------------------------------------------------------
# Training data generation
# ---------------------------------------------------------------------------

_ABBREVS = [
    "API", "URL", "HTTP", "JSON", "XML", "CSS", "HTML", "SQL", "SDK",
    "GPU", "CPU", "RAM", "SSD", "USB", "HDMI", "WiFi", "TCP", "UDP",
    "DNS", "FTP", "SSH", "TLS", "JWT", "OAuth", "REST", "GRPC",
    "IDE", "CLI", "GUI", "ORM", "MVC", "CRUD", "YAML", "PNG",
    "JPEG", "SVG", "PDF", "CSV", "GIF", "MP3", "MP4", "AWS",
    "GCP", "Linux", "macOS", "Windows", "iOS", "Node", "React",
    "Vue", "Svelte", "Django", "Flask", "Rails", "Git", "Docker",
    "Kubernetes", "Nginx", "Redis", "Kafka", "PostgreSQL", "MongoDB",
    "v1", "v2", "v3", "8Byte", "16bit", "32bit", "64bit",
    "R&D", "Wi-Fi", "C++", "IPv6", "IPv4", "HTTP2", "ES6",
]

# Common Japanese particles (romaji) used at word boundaries
_PARTICLES = [
    "de", "ni", "wo", "ha", "ga", "mo", "no", "to",
    "he", "wa", "ka", "ya", "yo", "ne", "na",
]

_PARTICLE_PHRASES = [
    "de", "ni", "wo", "ha", "ga", "mo", "no", "to", "he",
    "nitsuite", "nitaisite", "nikanshite", "nimukete", "nituite",
    "deha", "demo", "dake", "kara", "made", "toshite",
]

_COMMON_ROMAJI_TAILS = [
    "suru", "shita", "shite", "dekiru", "dekinai", "ugokanai",
    "naosu", "naoshita", "tukau", "tukutta", "yobu", "okuru",
    "kaku", "kaita", "kesu", "kieru", "kowareta", "toosu",
    "hanasu", "miru", "mirenai", "hairanai", "tsunagu", "tsunagaranai",
]

# Weighted patterns: romaji-heavy since that's modore's primary use case
_PATTERNS = [
    ("romaji", 3),
    ("ascii", 2),
    ("romaji+ascii", 2),
    ("ascii+romaji", 2),
    ("romaji+ascii+romaji", 4),
    ("ascii+romaji+ascii", 2),
    ("romaji+romaji", 1),
    ("ascii+particle+romaji", 5),
    ("ascii+particle+ascii", 4),
    ("ascii+particle+ascii+romaji", 5),
    ("ascii+particle+ascii+romaji_tail", 6),
    ("romaji+ascii+particle+ascii", 3),
    ("romaji+ascii+particle+romaji", 4),
    ("romaji+ascii+particle+ascii+romaji", 4),
    ("romaji+ascii+particle+ascii+romaji_tail", 5),
]

# Sentence patterns: how to use real sentence data
_SENT_PATTERNS = [
    ("sentence", 3),
    ("sentence+inject", 4),
    ("sentence+prefix_ascii", 2),
    ("ascii+sentence", 2),
]


def _build_sentence_sequence(
    sentence: list[tuple[str, bool]],
    ascii_tokens: list[str],
    pattern: str,
) -> tuple[str, list[int]]:
    """Build a training sequence from a real sentence."""
    if pattern == "sentence":
        text = "".join(w for w, _ in sentence)
        labels = [1] * len(text)
        return text, labels

    if pattern == "sentence+inject":
        text = ""
        labels: list[int] = []
        content_indices = [i for i, (_, c) in enumerate(sentence) if c]
        if not content_indices:
            text = "".join(w for w, _ in sentence)
            return text, [1] * len(text)
        n_inject = random.randint(1, min(2, len(content_indices)))
        inject_at = set(random.sample(
            content_indices, min(n_inject, len(content_indices))))
        for i, (w, is_content) in enumerate(sentence):
            if i in inject_at:
                eng = random.choice(ascii_tokens)
                text += eng
                labels.extend([0] * len(eng))
            else:
                text += w
                labels.extend([1] * len(w))
        return text, labels

    if pattern == "sentence+prefix_ascii":
        eng = random.choice(ascii_tokens)
        sent_text = "".join(w for w, _ in sentence)
        text = eng + sent_text
        labels = [0] * len(eng) + [1] * len(sent_text)
        return text, labels

    if pattern == "ascii+sentence":
        eng = random.choice(ascii_tokens)
        sent_text = "".join(w for w, _ in sentence)
        text = sent_text + eng
        labels = [1] * len(sent_text) + [0] * len(eng)
        return text, labels

    text = "".join(w for w, _ in sentence)
    return text, [1] * len(text)


def build_training_arrays(
    romaji_words: list[str],
    english_words: list[str],
    n_sequences: int = 100000,
    sentence_data: list[list[tuple[str, bool]]] | None = None,
    mixed_text_data: list[tuple[str, list[int]]] | None = None,
    hard_cases: list[HardCase] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    ascii_tokens = list(english_words)
    ascii_tokens.extend(_ABBREVS)
    for _ in range(200):
        ascii_tokens.append(str(random.randint(1, 9999)))

    weighted_pats: list[str] = []
    for pat, w in _PATTERNS:
        weighted_pats.extend([pat] * w)

    weighted_sent_pats: list[str] = []
    for pat, w in _SENT_PATTERNS:
        weighted_sent_pats.extend([pat] * w)

    use_sentences = sentence_data and len(sentence_data) > 0
    use_mixed = mixed_text_data and len(mixed_text_data) > 0

    # Allocation: 20% mixed text, 20% sentence, 60% synthetic word combos
    mixed_ratio = 0.20 if use_mixed else 0.0
    sent_ratio = 0.20 if use_sentences else 0.0

    all_indices: list[list[int]] = []
    all_labels: list[int] = []

    for seq_i in range(n_sequences):
        r = random.random()
        if use_mixed and r < mixed_ratio:
            text, char_labels = random.choice(mixed_text_data)
        elif use_sentences and r < mixed_ratio + sent_ratio:
            sent = random.choice(sentence_data)
            spat = random.choice(weighted_sent_pats)
            text, char_labels = _build_sentence_sequence(
                sent, ascii_tokens, spat)
        else:
            pat = random.choice(weighted_pats)
            text = ""
            char_labels = []
            for part in pat.split("+"):
                if part == "romaji":
                    w = random.choice(romaji_words)
                    text += w
                    char_labels.extend([1] * len(w))
                elif part == "particle":
                    p = random.choice(_PARTICLE_PHRASES)
                    text += p
                    char_labels.extend([1] * len(p))
                elif part == "romaji_tail":
                    w = random.choice(_COMMON_ROMAJI_TAILS)
                    text += w
                    char_labels.extend([1] * len(w))
                else:
                    w = random.choice(ascii_tokens)
                    text += w
                    char_labels.extend([0] * len(w))

        if len(text) > 120:
            text = text[:120]
            char_labels = char_labels[:120]

        validity = romaji_coverage(text)

        for pos in range(len(text)):
            if pos > 0 and random.random() < 0.85:
                history = char_labels[:pos]
            else:
                history = None
            idxs = extract_feature_indices(text, pos, validity, history)
            all_indices.append(idxs)
            all_labels.append(char_labels[pos])

        if (seq_i + 1) % 20000 == 0:
            print(f"    {seq_i + 1}/{n_sequences} sequences, "
                  f"{len(all_labels):,} examples")

    if hard_cases:
        must_cases = [(text, labels) for tier, text, labels in hard_cases
                      if tier == "must"]
        target_cases = [(text, labels) for tier, text, labels in hard_cases
                        if tier == "target"]
        repeat_plan = [
            ("must", must_cases, max(250, n_sequences // 625)),
            ("target", target_cases, max(50, n_sequences // 5000)),
        ]
        for tier, cases, repeats in repeat_plan:
            if not cases:
                continue
            for text, char_labels in cases:
                validity = romaji_coverage(text)
                for _ in range(repeats):
                    for pos in range(len(text)):
                        history = char_labels[:pos]
                        idxs = extract_feature_indices(text, pos, validity, history)
                        all_indices.append(idxs)
                        all_labels.append(char_labels[pos])
            print(f"    Added {len(cases):,} curated {tier} cases x{repeats}")

        # Add boundary-focused variants without perfect history so the
        # autoregressive inference path sees the same cases after mistakes.
        for tier, cases, repeats in repeat_plan:
            if tier != "must" or not cases:
                continue
            noisy_repeats = max(25, repeats // 8)
            for text, char_labels in cases:
                validity = romaji_coverage(text)
                for _ in range(noisy_repeats):
                    for pos in range(len(text)):
                        history = None if random.random() < 0.6 else char_labels[:pos]
                        idxs = extract_feature_indices(text, pos, validity, history)
                        all_indices.append(idxs)
                        all_labels.append(char_labels[pos])
            print(f"    Added {len(cases):,} curated must no-history cases x{noisy_repeats}")

    features = np.array(all_indices, dtype=np.int32)
    labels = np.array(all_labels, dtype=np.int8)
    return features, labels


def _cache_key(n_sequences: int, seed: int, n_romaji: int, n_english: int,
               n_sentences: int = 0, n_hard_cases: int = 0) -> str:
    """Deterministic cache filename from parameters that affect the arrays."""
    import hashlib
    h = hashlib.sha1(
        f"v{TRAINING_DATA_VERSION}:seq={n_sequences}:seed={seed}:rom={n_romaji}:eng={n_english}"
        f":snt={n_sentences}:hard={n_hard_cases}"
        f":nb={N_BUCKETS}:ng={NGRAM_MAX}:w={WINDOW}".encode()
    ).hexdigest()[:12]
    return f"train_{h}.npz"


def save_cache(
    path: Path, features: np.ndarray, labels: np.ndarray
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(path, features=features, labels=labels)
    size_mb = path.stat().st_size / (1024 * 1024)
    print(f"  Cache saved to {path} ({size_mb:.1f} MB)")


def load_cache(path: Path) -> tuple[np.ndarray, np.ndarray] | None:
    if not path.exists():
        return None
    data = np.load(path)
    return data["features"], data["labels"]


# ---------------------------------------------------------------------------
# Vectorized mini-batch SGD
# ---------------------------------------------------------------------------


def _train_logistic(
    train_feats: np.ndarray, train_labels: np.ndarray,
    val_feats: np.ndarray, val_labels: np.ndarray,
    weights: np.ndarray, bias: float,
    lr: float, batch_size: int, l2: float,
    epoch: int, n_epochs: int,
) -> tuple[np.ndarray, float, float]:
    """One epoch of logistic regression (log loss) SGD."""
    n_train = train_feats.shape[0]
    shuffle = np.random.permutation(n_train)
    epoch_loss = 0.0

    for start in range(0, n_train, batch_size):
        end = min(start + batch_size, n_train)
        batch_idx = shuffle[start:end]
        bf = train_feats[batch_idx]
        bl = train_labels[batch_idx]
        B = bf.shape[0]

        scores = bias + weights[bf].sum(axis=1)
        scores = np.clip(scores, -500, 500)
        preds = 1.0 / (1.0 + np.exp(-scores))

        eps = 1e-15
        loss = -(bl * np.log(preds + eps) +
                 (1 - bl) * np.log(1 - preds + eps))
        epoch_loss += loss.sum()

        grad = (preds - bl) / B
        bias -= lr * grad.sum()

        grad_exp = np.broadcast_to(grad[:, None], bf.shape)
        np.add.at(weights, bf.ravel(), -(lr * grad_exp.ravel()))
        weights[:N_BUCKETS] *= (1.0 - lr * l2)

    weights[N_BUCKETS] = 0.0
    return weights, bias, epoch_loss / n_train


def _train_svm(
    train_feats: np.ndarray, train_labels: np.ndarray,
    val_feats: np.ndarray, val_labels: np.ndarray,
    weights: np.ndarray, bias: float,
    lr: float, batch_size: int, l2: float,
    epoch: int, n_epochs: int,
) -> tuple[np.ndarray, float, float]:
    """One epoch of linear SVM (hinge loss) SGD."""
    n_train = train_feats.shape[0]
    shuffle = np.random.permutation(n_train)
    epoch_loss = 0.0

    for start in range(0, n_train, batch_size):
        end = min(start + batch_size, n_train)
        batch_idx = shuffle[start:end]
        bf = train_feats[batch_idx]
        bl = train_labels[batch_idx]
        B = bf.shape[0]

        scores = bias + weights[bf].sum(axis=1)
        y_svm = 2.0 * bl - 1.0         # {0,1} → {-1,+1}
        margin = y_svm * scores
        hinge = np.maximum(0.0, 1.0 - margin)
        epoch_loss += hinge.sum()

        active = (margin < 1.0).astype(np.float64)
        grad = (-y_svm * active) / B
        bias -= lr * grad.sum()

        grad_exp = np.broadcast_to(grad[:, None], bf.shape)
        np.add.at(weights, bf.ravel(), -(lr * grad_exp.ravel()))
        weights[:N_BUCKETS] *= (1.0 - lr * l2)

    weights[N_BUCKETS] = 0.0
    return weights, bias, epoch_loss / n_train


def train(
    features: np.ndarray,
    labels: np.ndarray,
    lr: float = 0.05,
    n_epochs: int = 20,
    batch_size: int = 4096,
    l2: float = 0.0003,
    loss: str = "log",
) -> tuple[float, np.ndarray]:
    n_examples = features.shape[0]
    weights = np.zeros(N_BUCKETS + 1, dtype=np.float64)

    perm = np.random.permutation(n_examples)
    split = int(n_examples * 0.9)
    train_idx, val_idx = perm[:split], perm[split:]

    features = features.copy()
    features[features == -1] = N_BUCKETS

    train_feats = features[train_idx]
    train_labels = labels[train_idx].astype(np.float64)
    val_feats = features[val_idx]
    val_labels = labels[val_idx].astype(np.float64)

    bias = 0.0
    n_train = train_feats.shape[0]
    best_val_acc = 0.0
    best_bias, best_weights = bias, weights[:N_BUCKETS].copy()

    epoch_fn = _train_svm if loss == "hinge" else _train_logistic
    loss_label = "SVM (hinge)" if loss == "hinge" else "Logistic (log)"
    print(f"  Loss: {loss_label}")

    # SVM benefits from a slightly higher LR and less aggressive decay
    if loss == "hinge":
        lr = max(lr, 0.1)
        decay = 0.96
    else:
        decay = 0.94

    cur_lr = lr
    for epoch in range(n_epochs):
        weights, bias, avg_loss = epoch_fn(
            train_feats, train_labels, val_feats, val_labels,
            weights, bias, cur_lr, batch_size, l2, epoch, n_epochs)

        # Validation (use sigmoid + threshold for both — same inference path)
        vs = bias + weights[val_feats].sum(axis=1)
        vs = np.clip(vs, -500, 500)
        vp = 1.0 / (1.0 + np.exp(-vs))
        vl = (vp >= THRESHOLD).astype(np.float64)

        tp = ((vl == 1) & (val_labels == 1)).sum()
        fp = ((vl == 1) & (val_labels == 0)).sum()
        tn = ((vl == 0) & (val_labels == 0)).sum()
        fn = ((vl == 0) & (val_labels == 1)).sum()
        val_acc = (tp + tn) / max(tp + fp + tn + fn, 1)
        rom_f1 = 2 * tp / max(2 * tp + fp + fn, 1)
        asc_f1 = 2 * tn / max(2 * tn + fp + fn, 1)

        if val_acc > best_val_acc:
            best_val_acc = val_acc
            best_bias = bias
            best_weights = weights[:N_BUCKETS].copy()

        print(
            f"  epoch {epoch + 1:2d}/{n_epochs}: "
            f"loss={avg_loss:.4f}  acc={val_acc:.4f}  "
            f"rom_f1={rom_f1:.4f}  asc_f1={asc_f1:.4f}"
        )
        cur_lr *= decay

    print(f"  Best val accuracy: {best_val_acc:.4f}")
    return best_bias, best_weights


# ---------------------------------------------------------------------------
# Model export
# ---------------------------------------------------------------------------


def save_dict(words: list[str], path: Path) -> None:
    """Export sorted English dictionary for C boundary refinement."""
    sorted_words = sorted(set(w.lower() for w in words
                              if w.isascii() and w.isalpha() and 2 <= len(w) <= 30))
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="ascii") as f:
        for w in sorted_words:
            f.write(w + "\n")
    print(f"Dictionary saved to {path} ({len(sorted_words):,} words)")


def save_model(path: Path, bias: float, weights: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    # Rolling backup: if the output model already exists, rename it with a
    # timestamp before overwriting so we never lose a previous model.
    if path.exists():
        import datetime
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = path.with_name(f"classifier_{ts}.mdl")
        path.rename(backup)
        print(f"  Previous model backed up to {backup.name}")

    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", VERSION))
        f.write(struct.pack("<I", N_BUCKETS))
        f.write(struct.pack("<I", NGRAM_MAX))
        f.write(struct.pack("<I", WINDOW))
        f.write(struct.pack("<d", bias))
        f.write(weights.astype("<f8").tobytes())
    print(f"Model saved to {path} ({path.stat().st_size:,} bytes)")


# ---------------------------------------------------------------------------
# Diagnostic
# ---------------------------------------------------------------------------


_DICT_WORDS: set[str] = set()


def _refine_boundaries(text: str, labels: list[int]) -> None:
    """Dictionary-guided boundary refinement (mirrors C refine_boundaries)."""
    if not _DICT_WORDS:
        return
    particles = ("de", "ni", "no", "wo", "ha", "ga", "to", "mo", "he", "ya",
                 "kara", "made", "yori")
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        seg_len = j - i
        if labels[i] == 0 and seg_len >= 4 and (j == len(labels) or labels[j] == 1):
            seg_text = text[i:j].lower()
            if seg_text not in _DICT_WORDS:
                best_cand = None
                for trim in range(1, min(7, seg_len - 2)):
                    cand = seg_text[:seg_len - trim]
                    if cand in _DICT_WORDS:
                        if best_cand is None:
                            best_cand = cand
                        if seg_text[len(cand):].startswith(particles):
                            best_cand = cand
                            break
                if best_cand is not None:
                    for k in range(i + len(best_cand), j):
                        labels[k] = 1
        i = j

    # Pass 2: extend short ASCII segments forward into following romaji
    # when the combined text matches a dictionary word.
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if labels[i] == 0 and j < len(labels) and labels[j] == 1:
            rom_end = j
            while rom_end < len(labels) and labels[rom_end] == 1:
                rom_end += 1
            rom_len = rom_end - j
            max_ext = min(rom_len, 10)
            best_ext = 0
            for ext in range(1, max_ext + 1):
                cand = text[i : j + ext].lower()
                if ext >= 2 and cand in _DICT_WORDS:
                    best_ext = ext
            if best_ext > 0:
                for k in range(j, j + best_ext):
                    labels[k] = 0
        i = j


def segment_test(bias: float, weights: np.ndarray, text: str) -> str:
    w = np.zeros(N_BUCKETS + 1, dtype=np.float64)
    w[:N_BUCKETS] = weights

    validity = romaji_coverage(text)
    labels: list[int] = []
    pos = 0
    while pos < len(text):
        if not ("a" <= text[pos] <= "z"):
            if text[pos] == "-" and labels and labels[-1] == 1:
                labels.append(1)
            else:
                labels.append(0)
            pos += 1
            continue

        run_end = pos + 1
        while run_end < len(text) and ("a" <= text[run_end] <= "z"):
            run_end += 1
        run = text[pos:run_end]
        split_at = _forced_ascii_prefix_len(run)

        if split_at > 0:
            labels.extend([0] * split_at)
            suffix_labels: list[int] = []
            suffix_text = text[pos + split_at : run_end]
            for local_pos, _ in enumerate(suffix_text):
                idxs = extract_feature_indices(
                    suffix_text, local_pos, validity[pos + split_at : run_end], suffix_labels
                )
                ia = np.array(idxs, dtype=np.int32)
                ia = np.where(ia == -1, N_BUCKETS, ia)
                score = bias + w[ia].sum()
                pred = 1.0 / (1.0 + np.exp(-np.clip(score, -500, 500)))
                lbl = 1 if pred >= THRESHOLD else 0
                labels.append(lbl)
                suffix_labels.append(lbl)
        else:
            for p in range(pos, run_end):
                history = labels if labels else None
                idxs = extract_feature_indices(text, p, validity, history)
                ia = np.array(idxs, dtype=np.int32)
                ia = np.where(ia == -1, N_BUCKETS, ia)
                score = bias + w[ia].sum()
                pred = 1.0 / (1.0 + np.exp(-np.clip(score, -500, 500)))
                labels.append(1 if pred >= THRESHOLD else 0)
        pos = run_end

    # Smooth short runs
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if j - i < 2 and i > 0:
            for k in range(i, j):
                labels[k] = labels[i - 1]
        i = j

    # Dictionary post-processing: force ASCII on romaji segments ≥5 chars
    # that contain non-romaji substrings
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if labels[i] == 1 and j - i >= 5:
            seg_validity = romaji_coverage(text[i:j])
            if not all(seg_validity):
                for k in range(i, j):
                    labels[k] = 0
        i = j

    # Re-smooth after dictionary overrides
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if j - i < 2 and i > 0:
            for k in range(i, j):
                labels[k] = labels[i - 1]
        i = j

    # Boundary refinement using English dictionary
    _refine_boundaries(text, labels)
    i = 0
    while i < len(labels):
        j = i
        while j < len(labels) and labels[j] == labels[i]:
            j += 1
        if j - i < 2 and i > 0:
            for k in range(i, j):
                labels[k] = labels[i - 1]
        i = j

    parts: list[str] = []
    i = 0
    while i < len(text):
        j = i
        while j < len(text) and labels[j] == labels[i]:
            j += 1
        parts.append(f"[{'R' if labels[i] else 'A'}:{text[i:j]}]")
        i = j
    return " ".join(parts)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    import builtins
    global print
    print = functools.partial(builtins.print, flush=True)

    parser = argparse.ArgumentParser(
        description="Train modore romaji/ASCII n-gram classifier")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--lr", type=float, default=0.05)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--sequences", type=int, default=500000)
    parser.add_argument("--no-cache", action="store_true",
                        help="Force rebuild even if cached arrays exist")
    parser.add_argument("--no-tatoeba", action="store_true",
                        help="Skip Tatoeba sentence data")
    parser.add_argument("--no-mixed", action="store_true",
                        help="Skip mixed-language tech text data")
    parser.add_argument("--mixed-dir", action="append", type=Path, default=[],
                        help="Additional Japanese-primary mixed text corpus directory")
    parser.add_argument("--svm", action="store_true",
                        help="Use SVM (hinge loss) instead of logistic regression")
    args = parser.parse_args()

    random.seed(args.seed)
    np.random.seed(args.seed)

    # 1) Load romaji table + build validity trie
    print("Loading Mozc romaji table...")
    if not MOZC_ROMAJI_TABLE.exists():
        print(f"ERROR: {MOZC_ROMAJI_TABLE} not found")
        sys.exit(1)
    kana2rom = load_kana_to_romaji(MOZC_ROMAJI_TABLE)
    _build_romaji_trie(MOZC_ROMAJI_TABLE)
    print(f"  {len(kana2rom)} kana→romaji mappings, "
          f"{len(_ROMAJI_VALID)} valid romaji inputs")

    # 2) Romaji words (Mozc OSS + UT dictionaries + Wikipedia titles)
    print("Extracting romaji words from all dictionaries...")
    romaji_words = load_mozc_romaji_words(
        MOZC_DICT_DIR, kana2rom, ut_dict_dir=UT_DICT_DIR)
    wiki_romaji = load_jawiki_romaji(JAWIKI_TITLES, kana2rom)
    if wiki_romaji:
        before = len(set(romaji_words))
        merged = list(set(romaji_words) | set(wiki_romaji))
        random.shuffle(merged)
        romaji_words = merged
        print(f"    jawiki titles: +{len(romaji_words) - before:,} new")
    print(f"  {len(romaji_words):,} unique romaji words total")

    # 3) English words (system dict + COCA + tech vocabulary)
    print("Loading English words...")
    english_words = load_english_words(SYSTEM_DICT, coca_path=COCA_FREQ)
    print(f"  {len(english_words):,} English words total")

    # Populate lookups for dictionary features + boundary refinement
    _build_english_lookup(english_words)
    global _DICT_WORDS
    _DICT_WORDS = _ENGLISH_WORDS

    # 4) Sentence data (UD_Japanese-GSD + optionally Tatoeba)
    print("Loading sentence data...")
    sentence_data: list[list[tuple[str, bool]]] = []
    ud_sents = load_ud_sentences(UD_GSD_DIR, kana2rom)
    sentence_data.extend(ud_sents)
    if not args.no_tatoeba:
        tat_sents = load_tatoeba_sentences(TATOEBA_FILE, kana2rom)
        sentence_data.extend(tat_sents)
    else:
        print("    Tatoeba: skipped (--no-tatoeba)")
    print(f"  {len(sentence_data):,} sentences total")

    # 5) Mixed-language text (Japanese tech books/docs)
    mixed_text_data: list[tuple[str, list[int]]] = []
    if not args.no_mixed:
        print("Loading mixed-language tech text...")
        mixed_dirs = MIXED_TEXT_DIRS + args.mixed_dir
        mixed_text_data = load_mixed_text_sequences(mixed_dirs, kana2rom)
    else:
        print("  Mixed text: skipped (--no-mixed)")

    # Curated hard cases double as evaluation fixtures and training anchors.
    print("Loading curated classifier hard cases...")
    hard_cases = load_classifier_eval_cases(CLASSIFIER_EVAL)

    # 6) Build or load cached arrays
    n_rom = len(romaji_words)
    n_asc = len(english_words)
    n_snt = len(sentence_data)
    n_mix = len(mixed_text_data)
    n_hard = len(hard_cases)
    cache_name = _cache_key(args.sequences, args.seed, n_rom, n_asc,
                            n_snt + n_mix, n_hard)
    cache_path = CACHE_DIR / cache_name

    features: np.ndarray
    labels: np.ndarray

    def _build():
        print(f"Building training arrays ({args.sequences} sequences)...")
        return build_training_arrays(
            romaji_words, english_words,
            n_sequences=args.sequences,
            sentence_data=sentence_data if sentence_data else None,
            mixed_text_data=mixed_text_data if mixed_text_data else None,
            hard_cases=hard_cases if hard_cases else None)

    if not args.no_cache:
        cached = load_cache(cache_path)
        if cached is not None:
            features, labels = cached
            nr = (labels == 1).sum()
            na = (labels == 0).sum()
            print(f"  Loaded from cache: {len(labels):,} examples "
                  f"(romaji={nr:,}, ascii={na:,})")
        else:
            features, labels = _build()
            save_cache(cache_path, features, labels)
            nr = (labels == 1).sum()
            na = (labels == 0).sum()
            print(f"  {len(labels):,} examples (romaji={nr:,}, ascii={na:,})")
    else:
        features, labels = _build()
        save_cache(cache_path, features, labels)
        nr = (labels == 1).sum()
        na = (labels == 0).sum()
        print(f"  {len(labels):,} examples (romaji={nr:,}, ascii={na:,})")

    # 6) Train
    loss_type = "hinge" if args.svm else "log"
    print(f"Training ({args.epochs} epochs, batch={args.batch_size})...")
    bias, weights = train(
        features, labels,
        lr=args.lr, n_epochs=args.epochs, batch_size=args.batch_size,
        loss=loss_type)

    nz = np.count_nonzero(weights)
    print(f"Model: bias={bias:.4f}, non-zero={nz}/{N_BUCKETS}")

    # 7) Save model + dictionary
    save_model(args.out, bias, weights)
    save_dict(english_words, DICT_OUTPUT)

    # 8) Test
    cases = [
        # --- pure romaji / pure ASCII ---
        "nihongo",
        "hello",
        "senshuuryokou",
        "kubaborisu",
        "kuaborisu",
        # --- ASCII + particle + romaji (core boundary cases) ---
        "korehapythondesu",
        "dockerdetsukuru",
        "dockerdetukutta",
        "reactnitsuite",
        "pythondesagyousuru",
        "linaborisu",
        # --- romaji + ASCII + romaji ---
        "nihongodeAPIwoyobu",
        "dockerdeAPIwoyobu",
        "areha8Bytedesu",
        "APIkaitou",
        "HTTP2senkou",
        "vrtyatto",
        # --- doubled consonants (っ) ---
        "APIwotukutte",
        "tukuttemita",
        "kittekudasai",
        "happyoukai",
        "mattekudasai",
        "zasshiwoyomu",
        # --- realistic mixed sentences ---
        "nekohacatdesu",
        "watashihaengineerdesu",
        "konikinoLinuxdesetupshita",
        "gitdecommitshitaato",
        "Slackdekaiginorenraku",
        "kaborishimasen",
        "servernitsunagaranai",
        "bugwonaosu",
        "TypeScriptdekaihatsu",
        "macOSnoupdategaatta",
        "iikinoAPIgadekita",
        "nodejsdeugokanai",
        "awsnoEC2dedeployshita",
        "konofilehairanaito",
        "CSSdezureteiru",
    ]
    print("\nSegmentation tests:")
    for tc in cases:
        print(f"  {tc:40s} → {segment_test(bias, weights, tc)}")


if __name__ == "__main__":
    main()
