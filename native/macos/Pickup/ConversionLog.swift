// Conversion-decision logger: the data-capture half of the BERT reranking
// experiment (see tools/rerank/). When a ConversionSession retires (a new
// conversion overwrites it, focus changes, or the undo window elapses), the
// store seals it — the candidate the user actually settled on is now final —
// and, when `[experiment] log_conversions = on`, appends one JSONL line here.
//
// The schema is the contract with tools/rerank/schema.py; keep field names in
// lockstep. Opt-in and default-off: this records what the user types and
// converts, so it must never run unless explicitly enabled.
//
// Best-effort and off the critical path: writes happen on a private serial
// queue, and any I/O failure disables the log for the session rather than
// disturbing a conversion.

import Foundation

/// One sealed conversion decision. snake_case on the wire (via the encoder's
/// key strategy) to match the Python harness without a translation layer.
struct SealedConversion: Codable {
    let id: String                 // session uuid; later commits overwrite by id
    let ts: Int64                  // epoch millis (session's last-touch time)
    let appId: String?
    let reading: String            // what was picked up (romaji or kana)
    let candidates: [String]       // Mozc order; candidates[mozcTopIdx] = top
    let mozcTopIdx: Int
    let decidedIdx: Int            // settled index; -1 = undo (rejected all)
    let decidedValue: String?
    let contextBefore: String?     // AX path only; nil on clipboard/scripted
    let contextAfter: String?
    let backing: String            // "ax" | "clipboard"
}

enum ConversionLog {
    private static let queue = DispatchQueue(label: "com.modore.conversion-log")
    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.keyEncodingStrategy = .convertToSnakeCase
        e.outputFormatting = .withoutEscapingSlashes  // compact; one line per record
        return e
    }()
    private static var handle: FileHandle?
    private static var disabled = false

    static func path() -> String {
        "\(ModoreConfig.configDir())/conversions.jsonl"
    }

    /// Append a sealed decision. No-op unless logging is enabled. Failures are
    /// logged once and disable the sink for the rest of the session.
    static func append(_ record: SealedConversion) {
        guard gConversionLogEnabled, !disabled else { return }
        queue.async {
            do {
                let fh = try openHandle()
                var line = try encoder.encode(record)
                line.append(0x0A)
                try fh.write(contentsOf: line)
            } catch {
                disabled = true
                handle = nil
                Log.boot("conversion log disabled (write failed): \(String(describing: error))")
            }
        }
    }

    private static func openHandle() throws -> FileHandle {
        if let h = handle { return h }
        let p = path()
        let fm = FileManager.default
        if !fm.fileExists(atPath: p) {
            fm.createFile(atPath: p, contents: nil)
        }
        guard let fh = FileHandle(forWritingAtPath: p) else {
            throw CocoaError(.fileWriteUnknown)
        }
        try fh.seekToEnd()
        handle = fh
        return fh
    }
}
