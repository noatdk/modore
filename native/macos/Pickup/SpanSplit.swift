// @testable
//
// Span-manipulation helpers used by the pickup pipeline. Kept here
// (separate from Pickup.swift) so the test driver in test/macos/ can compile
// them without dragging Cocoa / ApplicationServices in.
//
// The `@testable` marker above is the contract with native/macos/Makefile's
// `test` target: any *.swift file that opens with that comment gets pulled
// into the test binary alongside every *.swift under test/macos/. The test
// target links the scripting engine because these helpers delegate portable
// text policy to C.

import Foundation

// MARK: - UTF-8 ↔ UTF-16 mapping

private let kChromiumClipboardProbeBundleIDs: Set<String> = [
    "com.google.Chrome",
    "com.google.Chrome.canary",
    "org.chromium.Chromium",
]

func isChromiumClipboardProbeBundleID(_ bundleID: String?) -> Bool {
    guard let bundleID else { return false }
    return kChromiumClipboardProbeBundleIDs.contains(bundleID)
}

extension String {
    /// UTF-8 byte offset corresponding to a UTF-16 code-unit offset.
    /// Out-of-range inputs clamp to bounds. O(n) — the only callers run
    /// on the pickup pipeline, not per keystroke.
    func utf8ByteOffset(forUTF16Offset u: Int) -> Int {
        if u <= 0 { return 0 }
        let limit = utf16.count
        let safe = min(u, limit)
        let idx = String.Index(utf16Offset: safe, in: self)
        let utf8Anchor = idx.samePosition(in: utf8) ?? utf8.endIndex
        return utf8.distance(from: utf8.startIndex, to: utf8Anchor)
    }

    /// UTF-16 code-unit offset corresponding to a UTF-8 byte offset.
    /// Returns -1 if the byte offset lands in the middle of a multi-byte
    /// sequence (no valid UTF-16 anchor). Caller treats -1 as "ignore".
    func utf16Offset(forUTF8Byte b: Int) -> Int {
        if b <= 0 { return 0 }
        let limit = utf8.count
        let safe = min(b, limit)
        let utf8Idx = utf8.index(utf8.startIndex, offsetBy: safe)
        guard let mapped = utf8Idx.samePosition(in: utf16) else { return -1 }
        return utf16.distance(from: utf16.startIndex, to: mapped)
    }
}

private func utf8Range(_ s: String, start: Int, end: Int) -> String {
    guard start >= 0, end >= start else { return "" }
    let bytes = Array(s.utf8)
    guard end <= bytes.count else { return "" }
    return String(decoding: bytes[start..<end], as: UTF8.self)
}

/// Walk outward from `caret` over `text` to find the run of "word" code
/// units that contains it. Breaks at whitespace (space, tab, CR, LF) and at
/// ASCII ↔ non-ASCII script transitions. The script-break stop is what
/// keeps `kaitou`→hotkey→`hentai` from grabbing `回答hentai` as one word —
/// only the trailing `hentai` becomes the span.
///
/// TODO: this is a coarse split — every BMP code point ≥ 0x80 reads as
/// "non-ASCII" (kana, kanji, fullwidth, latin-1 supplement, …). Fine for
/// romaji→kana flow today; revisit when boundary cycling against Mozc's
/// segment output lands, since that will subsume this logic.
func wordBounds(_ text: String, caret: Int) -> (Int, Int) {
    guard !text.isEmpty else { return (0, 0) }
    let caretUTF16 = min(max(caret, 0), text.utf16.count)
    let caretByte = text.utf8ByteOffset(forUTF16Offset: caretUTF16)
    var bounds = mdr_byte_bounds_t(start_byte: 0, end_byte: 0)
    let byteCount = text.utf8.count
    let rc: Int32 = text.withCString { ptr in
        mdr_text_word_bounds(ptr, byteCount, caretByte, &bounds)
    }
    guard rc == 0 else { return (0, 0) }
    let start = text.utf16Offset(forUTF8Byte: Int(bounds.start_byte))
    let end = text.utf16Offset(forUTF8Byte: Int(bounds.end_byte))
    guard start >= 0, end >= start else { return (0, 0) }
    return (start, end)
}

/// Split a string at the boundary between any non-ASCII prefix and its
/// trailing run of ASCII UTF-16 code units. `"対人sen"` → `("対人", "sen")`,
/// `"祇園精舎のkaneno"` → `("祇園精舎の", "kaneno")`, `"sen"` → `("", "sen")`,
/// `"対人"` → `("対人", "")`. Mirrors `wordBounds`'s ASCII break (`< 0x80`);
/// used by both pickup paths to keep non-ASCII out of the Mozc bridge — the
/// bridge sends UTF-8 bytes individually as `key_code`, so non-ASCII inputs
/// come back as Latin-1 mojibake.
func splitTrailingASCII(_ s: String) -> (prefix: String, tail: String) {
    let byteCount = s.utf8.count
    var split: size_t = 0
    let rc: Int32 = s.withCString { ptr in
        mdr_text_split_trailing_ascii(ptr, byteCount, &split)
    }
    guard rc == 0 else { return (s, "") }
    return (
        utf8Range(s, start: 0, end: Int(split)),
        utf8Range(s, start: Int(split), end: byteCount)
    )
}

/// Split off a trailing ASCII punctuation run from an ASCII string while
/// keeping the core intact. Used after `splitTrailingASCII` so the pickup
/// pipeline can convert the core and suffix separately.
func splitTrailingASCIIPunctuation(_ ascii: String) -> (core: String, suffix: String) {
    let byteCount = ascii.utf8.count
    var split: size_t = 0
    let rc: Int32 = ascii.withCString { ptr in
        mdr_text_split_trailing_ascii_punctuation(ptr, byteCount, &split)
    }
    guard rc == 0 else { return (ascii, "") }
    return (
        utf8Range(ascii, start: 0, end: Int(split)),
        utf8Range(ascii, start: Int(split), end: byteCount)
    )
}

/// True when the selection is a plain lowercase ASCII word.
///
/// Shared between the clipboard fallback's expansion heuristics and the
/// tests so the Chrome-specific probe behavior stays explicit.
func isLowerASCIIWord(_ s: String) -> Bool {
    !s.isEmpty && s.utf8.allSatisfy { $0 >= 0x61 && $0 <= 0x7A }
}

/// True when the selection is a lowercase ASCII token that already
/// contains a hyphen. Kept separate from `isLowerASCIIWord` because the
/// clipboard fallback uses the two cases slightly differently.
func isLowerASCIIHyphenRun(_ s: String) -> Bool {
    !s.isEmpty && s.utf8.allSatisfy {
        ($0 >= 0x61 && $0 <= 0x7A) || $0 == 0x2D
    }
}

/// Decide whether the clipboard fallback should spend extra effort probing
/// leftward expansion for a hyphenated romaji token.
///
/// Chromium text fields split on hyphens more aggressively than the other
/// fallback targets we care about, so a plain lowercase word like
/// `thingu` can actually be the tail of `mi-thingu`. To keep the common
/// Chrome/Obsidian romaji path fast, we only spend extra probing effort
/// on short tails where a hyphen split is plausible, or when the original
/// selection already contains a hyphen.
func shouldProbeForcedHyphenatedSelectionExpansion(
    original: String,
    appBundleID: String?
) -> Bool {
    guard isLowerASCIIWord(original) || isLowerASCIIHyphenRun(original) else {
        return false
    }
    if original.utf8.contains(0x2D) || original.utf16.count <= 3 {
        return true
    }
    guard isChromiumClipboardProbeBundleID(appBundleID) else {
        return false
    }
    return original.utf16.count <= 6
}

/// True for the ASCII punctuation that the pickup pipeline can normalize
/// without a romaji core. Used to decide whether the clipboard fallback
/// should expand a one-character punctuation selection leftward.
func isConvertiblePunctuationOnly(_ s: String) -> Bool {
    !s.isEmpty && s.utf8.allSatisfy { $0 == 0x2E || $0 == 0x2C || $0 == 0x2D }
}

/// True when a forced clipboard selection has grown from punctuation-only
/// (`.`) into a useful romaji+punctuation token (`koe.`). This protects
/// Chrome HTML inputs, where Shift+Opt+Left often selects only the final
/// punctuation for `koe.[trigger]`.
func hasLowerASCIICoreBeforePunctuation(_ s: String) -> Bool {
    let (prefix, tail) = splitTrailingASCII(s)
    let ascii = prefix.isEmpty ? tail : tail
    let (core, suffix) = splitTrailingASCIIPunctuation(ascii)
    guard !core.isEmpty, !suffix.isEmpty else { return false }
    return core.utf8.contains { $0 >= 0x61 && $0 <= 0x7A }
}

/// Split off a leading acronym/code head from an ASCII string so Mozc only
/// sees the trailing romaji. `"R&Diraisho"` → `("R&D", "iraisho")`,
/// `"APIkaitou"` → `("API", "kaitou")`, `"iraisho"` → `("", "iraisho")`.
///
/// A head qualifies only if it (a) starts with an uppercase letter, (b) is
/// at least two chars long, (c) contains at least one upper/digit/symbol
/// beyond the first char, and (d) is followed by a lowercase letter. Rules
/// (b)+(c) keep ordinary capitalised words like `Karen` from being split.
/// Romaji never contains uppercase, digits, or these ASCII symbols, so the
/// transition is a strong signal that the head is not romaji-for-kana.
///
/// Symbols accepted in the head are the ones that appear in real acronyms
/// (`R&D`, `Wi-Fi`, `C++`, `.NET`, `IPv6`) but never in Hepburn / kunrei
/// romaji. Phase 2 will add a user dictionary at
/// `~/.config/modore/non-japanese.txt` for tokens this heuristic misses.
func splitAcronymHead(_ ascii: String) -> (head: String, tail: String) {
    let byteCount = ascii.utf8.count
    var split: size_t = 0
    let rc: Int32 = ascii.withCString { ptr in
        mdr_text_split_acronym_head(ptr, byteCount, &split)
    }
    guard rc == 0 else { return ("", ascii) }
    return (
        utf8Range(ascii, start: 0, end: Int(split)),
        utf8Range(ascii, start: Int(split), end: byteCount)
    )
}

/// UTF-16 substring helper, matching the AX/JS index domain used throughout
/// the pickup pipeline. Returns `nil` if the bounds are out of range or empty.
func sliceUTF16(_ text: String, start: Int, end: Int) -> String? {
    guard start >= 0, end <= text.utf16.count, start < end else { return nil }
    let startIdx = String.Index(utf16Offset: start, in: text)
    let endIdx = String.Index(utf16Offset: end, in: text)
    return String(text[startIdx..<endIdx])
}
