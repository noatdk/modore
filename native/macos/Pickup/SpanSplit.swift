// @testable
//
// Pure span-manipulation helpers used by the pickup pipeline. Kept here
// (separate from Pickup.swift) so the test driver in test/macos/ can compile
// them without dragging Cocoa / ApplicationServices in.
//
// The `@testable` marker above is the contract with native/macos/Makefile's
// `test` target: any *.swift file that opens with that comment gets pulled
// into the test binary alongside every *.swift under test/macos/. No need
// to edit the Makefile when adding a new pure helper.

import Foundation

/// Walk outward from `caret` over `utf16` to find the run of "word" code
/// units that contains it. Breaks at whitespace (space, tab, CR, LF) and at
/// ASCII ↔ non-ASCII script transitions. The script-break stop is what
/// keeps `kaitou`→hotkey→`hentai` from grabbing `回答hentai` as one word —
/// only the trailing `hentai` becomes the span.
///
/// TODO: this is a coarse split — every BMP code point ≥ 0x80 reads as
/// "non-ASCII" (kana, kanji, fullwidth, latin-1 supplement, …). Fine for
/// romaji→kana flow today; revisit when boundary cycling against Mozc's
/// segment output lands, since that will subsume this logic.
func wordBounds(_ utf16: [UInt16], caret: Int) -> (Int, Int) {
    if utf16.isEmpty { return (0, 0) }
    let c = min(max(caret, 0), utf16.count)
    let isWS: (UInt16) -> Bool = { ch in
        ch == 0x20 || ch == 0x09 || ch == 0x0A || ch == 0x0D
    }
    let isASCII: (UInt16) -> Bool = { $0 < 0x80 }
    let scriptBreak: (UInt16, UInt16) -> Bool = { a, b in
        isASCII(a) != isASCII(b)
    }
    var start = c
    while start > 0 && !isWS(utf16[start - 1]) {
        if start < utf16.count, scriptBreak(utf16[start - 1], utf16[start]) {
            break
        }
        start -= 1
    }
    var end = c
    while end < utf16.count && !isWS(utf16[end]) {
        if end > 0, scriptBreak(utf16[end - 1], utf16[end]) {
            break
        }
        end += 1
    }
    if start == end {
        if c < utf16.count { return (c, c + 1) }
        if c > 0 { return (c - 1, c) }
    }
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
    let utf16 = Array(s.utf16)
    var split = utf16.count
    while split > 0 && utf16[split - 1] < 0x80 {
        split -= 1
    }
    let prefix = split > 0
        ? String(utf16CodeUnits: Array(utf16[0..<split]), count: split)
        : ""
    let tail = split < utf16.count
        ? String(utf16CodeUnits: Array(utf16[split..<utf16.count]), count: utf16.count - split)
        : ""
    return (prefix, tail)
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
    let chars = Array(ascii.unicodeScalars)
    guard chars.count >= 2 else { return ("", ascii) }
    let isUpper: (Unicode.Scalar) -> Bool = { $0.value >= 0x41 && $0.value <= 0x5A }
    let isLower: (Unicode.Scalar) -> Bool = { $0.value >= 0x61 && $0.value <= 0x7A }
    let isDigit: (Unicode.Scalar) -> Bool = { $0.value >= 0x30 && $0.value <= 0x39 }
    let acronymSymbols: Set<Unicode.Scalar> = ["&", "-", ".", "_", "+", "/", ":", "@", "#"]
    let isAcronymSym: (Unicode.Scalar) -> Bool = { acronymSymbols.contains($0) }

    guard isUpper(chars[0]) else { return ("", ascii) }
    var i = 1
    var sawNonLetter = false
    while i < chars.count {
        let c = chars[i]
        if isUpper(c) {
            i += 1
        } else if isDigit(c) || isAcronymSym(c) {
            sawNonLetter = true
            i += 1
        } else {
            break
        }
    }
    guard i >= 2, i < chars.count, isLower(chars[i]) else { return ("", ascii) }
    guard i >= 3 || sawNonLetter else { return ("", ascii) }
    let head = String(String.UnicodeScalarView(chars[0..<i]))
    let tail = String(String.UnicodeScalarView(chars[i..<chars.count]))
    return (head, tail)
}

/// UTF-16 substring helper, matching the AX/JS index domain used throughout
/// the pickup pipeline. Returns `nil` if the bounds are out of range or empty.
func sliceUTF16(_ text: String, start: Int, end: Int) -> String? {
    let utf16 = Array(text.utf16)
    guard start >= 0, end <= utf16.count, start < end else { return nil }
    return String(utf16CodeUnits: Array(utf16[start..<end]), count: end - start)
}

/// Use the ML classifier to split an ASCII string into romaji and ASCII
/// segments, convert each romaji segment through Mozc independently, and
/// reassemble. Falls back to nil when the classifier is not loaded or the
/// string is not mixed (single-type → let the caller use the normal path).
///
/// When mixed, returns `(replacement, candidates)` where candidates are
/// generated for the primary (longest) romaji segment with other segments
/// frozen.
func classifierSegmentedConvert(
    _ ascii: String,
    request: PickupRequest
) -> (replacement: String, candidates: [String])? {
    guard let segments = Classifier.segment(ascii),
          Classifier.isMixed(segments) else { return nil }

    let utf8 = Array(ascii.utf8)

    // Find the primary romaji segment (longest) for candidate generation
    var primaryIdx = -1
    var primaryLen = 0
    for (i, seg) in segments.enumerated() where seg.isRomaji {
        let len = seg.end - seg.start
        if len > primaryLen {
            primaryLen = len
            primaryIdx = i
        }
    }

    // Convert each segment
    var parts: [String] = []
    var primaryResult: ConvertResult? = nil

    for (i, seg) in segments.enumerated() {
        let startIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.start)
        let endIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.end)
        let chunk = String(ascii.utf8[startIdx..<endIdx])!

        if seg.isRomaji {
            let wantCands = (i == primaryIdx)
            if let result = callBackend(chunk, request: request,
                                        wantCandidates: wantCands) {
                parts.append(result.replacement)
                if wantCands { primaryResult = result }
            } else {
                parts.append(chunk)
            }
        } else {
            parts.append(chunk)
        }
    }

    let replacement = parts.joined()

    // Build candidate list: for each candidate of the primary segment,
    // splice it back with the other (fixed) parts.
    var candidates: [String] = [replacement]
    if let pr = primaryResult, !pr.candidates.isEmpty {
        candidates = pr.candidates.map { cand in
            var rebuilt: [String] = []
            for (i, seg) in segments.enumerated() {
                if i == primaryIdx {
                    rebuilt.append(cand)
                } else {
                    let startIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.start)
                    let endIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.end)
                    let chunk = String(ascii.utf8[startIdx..<endIdx])!
                    if seg.isRomaji {
                        if let r = callBackend(chunk, request: request) {
                            rebuilt.append(r.replacement)
                        } else {
                            rebuilt.append(chunk)
                        }
                    } else {
                        rebuilt.append(chunk)
                    }
                }
            }
            return rebuilt.joined()
        }
    }

    Log.pickup("classifier segmented: \(segments.count) segments → \(replacement)")
    return (replacement, candidates)
}
