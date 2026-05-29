// Swift façade for the ML-based romaji/ASCII segmenter (libmodore_script).
//
// The classifier is loaded once at boot from a model file and used during
// pickup to split mixed ASCII strings (e.g. "areha8Bytedesu") into romaji
// and ASCII segments. Romaji segments go to Mozc; ASCII segments are
// kept as-is.
//
// If the model file is missing or the classifier can't load, all public
// functions return nil and the host falls back to the existing heuristic
// pipeline (splitAcronymHead).

import Foundation

enum Classifier {
    static let modelFileName = "classifier.mdl"
    static let bundledModelResourceName = "classifier"

    // Process-wide classifier handle. Written once at boot, read from the
    // pickup dispatch queue. Same visibility contract as ModoreScript.engine.
    private static var handle: OpaquePointer? = nil

    struct Segment {
        let start: Int
        let end: Int
        let isRomaji: Bool
    }

    /// Load a model file. Call once at boot. Returns true on success.
    static func load(modelPath: String) -> Bool {
        guard let h = mdr_cls_load(modelPath) else {
            Log.tagged("classifier", "failed to load model: \(modelPath)")
            return false
        }
        handle = h
        Log.tagged("classifier", "model loaded from \(modelPath)")

        // Load English dictionary for boundary refinement (optional).
        let modelURL = URL(fileURLWithPath: modelPath)
        let dictPath = modelURL.deletingLastPathComponent()
            .appendingPathComponent("english_dict.txt").path
        if FileManager.default.fileExists(atPath: dictPath) {
            let rc = mdr_cls_load_dict(h, dictPath)
            if rc == 0 {
                Log.tagged("classifier", "dictionary loaded from \(dictPath)")
            }
        }
        return true
    }

    static func configModelPath(configDir: String) -> String {
        "\(configDir)/\(modelFileName)"
    }

    static func bundledModelPath() -> String? {
        Bundle.main.path(forResource: bundledModelResourceName, ofType: "mdl")
    }

    static var isLoaded: Bool { handle != nil }

    /// Segment an ASCII string into romaji / ASCII runs.
    /// Returns nil if no classifier is loaded. Returns an array of
    /// Segment when segmentation succeeds. A single-segment result
    /// means the entire string is one type (no split needed).
    static func segment(_ text: String) -> [Segment]? {
        guard let h = handle else { return nil }
        let utf8 = Array(text.utf8)
        guard !utf8.isEmpty else { return nil }

        var segs = [mdr_segment_t](repeating: mdr_segment_t(), count: 16)
        let n: Int32 = utf8.withUnsafeBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buf.count) { cptr in
                segs.withUnsafeMutableBufferPointer { segBuf in
                    mdr_cls_segment(h, cptr, buf.count,
                                    segBuf.baseAddress, segBuf.count)
                }
            }
        }
        guard n > 0 else { return nil }

        var result: [Segment] = []
        result.reserveCapacity(Int(n))
        for i in 0..<Int(n) {
            let s = segs[i]
            result.append(Segment(
                start: Int(s.start),
                end: Int(s.end),
                isRomaji: s.is_romaji != 0
            ))
        }
        return result
    }

    /// True if the segmentation found a mix of romaji and ASCII parts.
    /// A single-segment result (all romaji or all ASCII) is not "mixed."
    static func isMixed(_ segments: [Segment]) -> Bool {
        guard segments.count > 1 else { return false }
        let hasRomaji = segments.contains { $0.isRomaji }
        let hasASCII = segments.contains { !$0.isRomaji }
        return hasRomaji && hasASCII
    }

    static func shutdown() {
        if let h = handle {
            mdr_cls_free(h)
            handle = nil
        }
    }
}
