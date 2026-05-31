// Live reranker client — the host half of the async R2 wiring.
//
// After modore commits Mozc's top candidate (so the user already sees a
// result with zero added latency), this asks the Python sidecar
// (tools/rerank/serve.py, over a Unix socket) whether a different candidate
// fits the surrounding context/history better. If the sidecar answers with
// high enough confidence AND the conversion is still live + untouched, the
// host swaps the candidate in via its normal cycle path. The host NEVER
// blocks on the sidecar: the socket round-trip runs on a background queue
// with a tight timeout, and if the sidecar is slow, absent, or unsure,
// Mozc's pick simply stands.
//
// Opt-in via `[experiment] reranker = r2`; default off. `gRerankerMinMargin`
// is the confidence gate (top-1 minus top-2 log-prob) below which we never
// override — the guard against regressing conversions Mozc already got right.

import Foundation

enum RerankerMode {
    case off
    case r2
}

enum RerankClient {
    private static let queue = DispatchQueue(label: "com.modore.rerank-client", qos: .utility)
    private static let connectTimeoutMs = 250
    // Read waits for the sidecar's inference (warm ~40-150ms, can spike on MPS),
    // so give it headroom — the round-trip runs on a background queue and never
    // blocks the UI, so a generous ceiling only delays a possible swap.
    private static let readTimeoutMs = 800
    private static var warnedUnavailable = false

    private struct Request: Encodable {
        let reading: String
        let candidates: [String]
        let contextBefore: String?
        let contextAfter: String?
        let minMargin: Double          // echoed so the sidecar log matches the host's gate
        let history: [Hist]
        struct Hist: Encodable { let reading: String; let decidedValue: String }
    }
    private struct Response: Decodable {
        let order: [Int]?
        let top: Int?
        let margin: Double?
        let error: String?
    }

    /// Fire-and-forget reranking for a just-committed session. Snapshots what
    /// it needs synchronously, then does the socket round-trip + any swap off
    /// the pickup thread.
    static func maybeRerank(session: ConversionSession) {
        guard gRerankerMode == .r2 else { return }
        let candidates = session.candidates.map { $0.value }
        guard candidates.count > 1 else { return }

        let id = session.id
        let reading = session.originalReading
        let before = session.contextBefore
        let after = session.contextAfter
        let appId = session.appId ?? session.backing.loggingBundleId
        let history = ConversionSessionStore.recentHistory(appId: appId)
            .filter { $0.id != id.uuidString && $0.decidedIdx >= 0 }
            .suffix(5)
            .compactMap { h -> Request.Hist? in
                guard let v = h.decidedValue else { return nil }
                return Request.Hist(reading: h.reading, decidedValue: v)
            }
        let req = Request(reading: reading, candidates: candidates,
                          contextBefore: before, contextAfter: after,
                          minMargin: gRerankerMinMargin, history: history)

        queue.async {
            guard let resp = roundtrip(req) else { return }
            if let err = resp.error {
                Log.cycle("rerank: sidecar error: \(err)")
                return
            }
            guard let top = resp.top, let margin = resp.margin,
                  top >= 0, top < candidates.count else { return }
            guard margin >= gRerankerMinMargin else {
                Log.cycle("rerank: skip '\(candidates[top])' margin \(String(format: "%.2f", margin)) < \(gRerankerMinMargin) (\(reading))")
                return
            }
            kHotkeyTapQueue.async {
                if applyRerankToIndex(top, forSession: id, verbose: false) {
                    Log.cycle("rerank: override → '\(candidates[top])' margin \(String(format: "%.2f", margin)) (\(reading))")
                }
            }
        }
    }

    // MARK: - Unix-socket round-trip (connect, send one line, read one line)

    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.keyEncodingStrategy = .convertToSnakeCase
        e.outputFormatting = .withoutEscapingSlashes
        return e
    }()
    private static let decoder = JSONDecoder()

    private static func roundtrip(_ req: Request) -> Response? {
        guard let fd = openSocket(path: gRerankerSocketPath) else {
            if !warnedUnavailable {
                warnedUnavailable = true
                Log.cycle("rerank: sidecar not reachable at \(gRerankerSocketPath) (run `make rerank-serve`); leaving Mozc's pick")
            }
            return nil
        }
        defer { close(fd) }
        warnedUnavailable = false
        do {
            var line = try encoder.encode(req)
            line.append(0x0A)
            guard writeAll(fd, line) else { return nil }
            guard let respLine = readLine(fd) else { return nil }
            return try decoder.decode(Response.self, from: respLine)
        } catch {
            Log.cycle("rerank: protocol error: \(String(describing: error))")
            return nil
        }
    }

    private static func openSocket(path: String) -> Int32? {
        guard !path.isEmpty else { return nil }
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(path.utf8)
        let cap = MemoryLayout.size(ofValue: addr.sun_path)
        guard pathBytes.count < cap else { close(fd); return nil }
        withUnsafeMutablePointer(to: &addr.sun_path) { raw in
            raw.withMemoryRebound(to: UInt8.self, capacity: cap) { dst in
                for (i, b) in pathBytes.enumerated() { dst[i] = b }
                dst[pathBytes.count] = 0
            }
        }
        var rcv = timeval(tv_sec: readTimeoutMs / 1000, tv_usec: Int32((readTimeoutMs % 1000) * 1000))
        var snd = timeval(tv_sec: connectTimeoutMs / 1000, tv_usec: Int32((connectTimeoutMs % 1000) * 1000))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, socklen_t(MemoryLayout<timeval>.size))
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, len) }
        }
        guard rc == 0 else { close(fd); return nil }
        return fd
    }

    private static func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) -> Bool in
            var sent = 0
            let base = buf.baseAddress!
            while sent < data.count {
                let n = write(fd, base + sent, data.count - sent)
                if n <= 0 { return false }
                sent += n
            }
            return true
        }
    }

    private static func readLine(_ fd: Int32) -> Data? {
        var out = Data()
        var byte: UInt8 = 0
        while out.count < 1 << 20 {
            let n = read(fd, &byte, 1)
            if n <= 0 { return out.isEmpty ? nil : out }
            if byte == 0x0A { return out }
            out.append(byte)
        }
        return out
    }
}
