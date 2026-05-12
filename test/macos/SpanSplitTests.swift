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
