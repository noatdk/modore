// Unit tests for the pure span-domain helpers in
// native/macos/Pickup/SpanSplit.swift. Driven from the macos Makefile —
// see `test-macos`. No XCTest dependency (the project doesn't ship a
// SwiftPM manifest); we just count assertions and exit non-zero on the
// first failure, so output stays one line per test in CI.

import Foundation

@main
struct SpanSplitTests {
    static var failures = 0
    static var passes = 0

    static func expectEqual<T: Equatable>(
        _ actual: T, _ expected: T, _ name: String,
        file: StaticString = #file, line: UInt = #line
    ) {
        if actual == expected {
            passes += 1
            return
        }
        failures += 1
        FileHandle.standardError.write(Data(
            "FAIL [\(name)] \(file):\(line)\n  expected: \(expected)\n  actual:   \(actual)\n".utf8))
    }

    static func expectTuple<A: Equatable, B: Equatable>(
        _ actual: (A, B), _ expected: (A, B), _ name: String,
        file: StaticString = #file, line: UInt = #line
    ) {
        if actual == expected {
            passes += 1
            return
        }
        failures += 1
        FileHandle.standardError.write(Data(
            "FAIL [\(name)] \(file):\(line)\n  expected: \(expected)\n  actual:   \(actual)\n".utf8))
    }

    static func main() {
        // MARK: - splitTrailingASCII
        //
        // The bug this protects against: in Discord/Sublime, Shift+Opt+Left
        // grabs the whole word (kanji prefix + trailing romaji), and Mozc's
        // bridge then reads each UTF-8 byte as a Latin-1 codepoint, yielding
        // mojibake. The split keeps non-ASCII out of the bridge entirely.
        expectTuple(splitTrailingASCII("対人sen"),         ("対人", "sen"),         "split kanji+romaji")
        expectTuple(splitTrailingASCII("祇園精舎のkaneno"), ("祇園精舎の", "kaneno"), "split long kana+romaji")
        expectTuple(splitTrailingASCII("sen"),             ("", "sen"),             "split pure ASCII")
        expectTuple(splitTrailingASCII("対人"),            ("対人", ""),            "split pure non-ASCII")
        expectTuple(splitTrailingASCII(""),                ("", ""),                "split empty")
        // Only the TRAILING ASCII run counts — interior ASCII stays with the
        // prefix. Matches the AX path's `wordBounds`: outward scan from the
        // caret stops at the first script flip, so the run touching the
        // caret is what we keep.
        expectTuple(splitTrailingASCII("kaitou回答hentai"), ("kaitou回答", "hentai"), "split interior ASCII stays with prefix")
        expectTuple(splitTrailingASCII(" sen"),            ("", " sen"),            "split leading space is ASCII")
        expectTuple(splitTrailingASCII("対人  "),          ("対人", "  "),          "split trailing spaces are ASCII")
        // Surrogate pairs: a supplementary-plane codepoint splits into two
        // UTF-16 units, both in 0xD800-0xDFFF — well above 0x80, so they
        // stick with the prefix. Without this property the split could land
        // mid-surrogate and produce invalid UTF-16. 𠮷 (U+20BB7) is the
        // canonical "tsuchi-yoshi" test.
        expectTuple(splitTrailingASCII("𠮷野yoshi"),       ("𠮷野", "yoshi"),       "split keeps surrogate pair intact")

        // MARK: - splitAcronymHead
        //
        // Acronyms/codes embedded as a prefix to romaji (`R&Diraisho`) would
        // otherwise be fed wholesale to Mozc, which can't romaji-convert
        // uppercase / symbol / digit chars and returns either garbage or
        // the input unchanged. Phase 1 heuristic: leading uppercase run with
        // at least one non-letter signal, followed by a lowercase letter.
        expectTuple(splitAcronymHead("R&Diraisho"),  ("R&D",  "iraisho"),  "acronym U-S-U head")
        expectTuple(splitAcronymHead("APIiraisho"),  ("API",  "iraisho"),  "acronym 3xU head")
        expectTuple(splitAcronymHead("JSONkaitou"),  ("JSON", "kaitou"),   "acronym 4xU head")
        // Heads with internal lowercase (`IPv6`, `iPhone`) aren't split —
        // the walker stops at the first lowercase. Acceptable: those don't
        // fit a clean acronym pattern anyway, and Phase 2's user dict
        // covers them.
        expectTuple(splitAcronymHead("IPv6settei"),  ("",     "IPv6settei"), "no split: mixed-case head")
        expectTuple(splitAcronymHead(".NETkaihatsu"),("",     ".NETkaihatsu"), "no split: must start with uppercase letter")
        // Single-capital words like names must NOT split — `Karen` is not
        // an acronym. Two-uppercase heads are also rejected to keep noise
        // down (`IT`, `JS` could be typos; users can hit hotkey on the
        // lowercase part only if needed).
        expectTuple(splitAcronymHead("Karen"),       ("",     "Karen"),    "no split: single uppercase head")
        expectTuple(splitAcronymHead("ABcde"),       ("",     "ABcde"),    "no split: 2-uppercase head without symbol")
        expectTuple(splitAcronymHead("iraisho"),     ("",     "iraisho"),  "no split: plain romaji")
        expectTuple(splitAcronymHead("desu"),        ("",     "desu"),     "no split: plain romaji short")
        expectTuple(splitAcronymHead("R&D"),         ("",     "R&D"),      "no split: no lowercase tail")
        expectTuple(splitAcronymHead("C++"),         ("",     "C++"),      "no split: no lowercase tail (symbols)")
        expectTuple(splitAcronymHead("8byte"),       ("",     "8byte"),    "no split: doesn't start with uppercase")
        expectTuple(splitAcronymHead(""),            ("",     ""),         "no split: empty")
        expectTuple(splitAcronymHead("A"),           ("",     "A"),        "no split: single char")

        // MARK: - wordBounds (sanity coverage — the script-break stop is the load-bearing part)

        expectTuple(wordBounds(Array("hello world".utf16), caret: 3),  (0, 5),  "wordBounds caret in first word")
        expectTuple(wordBounds(Array("hello world".utf16), caret: 8),  (6, 11), "wordBounds caret in second word")
        expectTuple(wordBounds([], caret: 0),                          (0, 0),  "wordBounds empty input")
        // Caret clamping: out-of-range caret gets clipped to the string bounds.
        expectTuple(wordBounds(Array("abc".utf16), caret: 99),         (0, 3),  "wordBounds caret past end")
        expectTuple(wordBounds(Array("abc".utf16), caret: -5),         (0, 3),  "wordBounds caret before start (clamps to 0 then walks forward)")
        // Script-break stops mid-word: caret in trailing romaji must NOT
        // pull the kanji prefix into the span. This is the AX path's
        // analogue of splitTrailingASCII — they have to agree.
        let kanjiRoman = Array("対人sen".utf16)
        let (kStart, kEnd) = wordBounds(kanjiRoman, caret: kanjiRoman.count)
        expectTuple((kStart, kEnd), (2, 5), "wordBounds stops at ASCII↔non-ASCII transition")

        // MARK: - sliceUTF16

        expectEqual(sliceUTF16("hello", start: 1, end: 4),    "ell", "sliceUTF16 middle")
        expectEqual(sliceUTF16("対人sen", start: 2, end: 5),  "sen", "sliceUTF16 across script boundary")
        expectEqual(sliceUTF16("hi", start: 0, end: 0),       nil,   "sliceUTF16 empty range -> nil")
        expectEqual(sliceUTF16("hi", start: 0, end: 99),      nil,   "sliceUTF16 out of range -> nil")
        expectEqual(sliceUTF16("hi", start: -1, end: 2),      nil,   "sliceUTF16 negative start -> nil")

        // MARK: - Report

        if failures == 0 {
            print("OK: \(passes) tests passed")
            exit(0)
        } else {
            FileHandle.standardError.write(Data("\(failures) failure(s), \(passes) passed\n".utf8))
            exit(1)
        }
    }
}
