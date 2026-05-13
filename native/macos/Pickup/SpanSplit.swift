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

/// UTF-16 substring helper, matching the AX/JS index domain used throughout
/// the pickup pipeline. Returns `nil` if the bounds are out of range or empty.
func sliceUTF16(_ text: String, start: Int, end: Int) -> String? {
    let utf16 = Array(text.utf16)
    guard start >= 0, end <= utf16.count, start < end else { return nil }
    return String(utf16CodeUnits: Array(utf16[start..<end]), count: end - start)
}
