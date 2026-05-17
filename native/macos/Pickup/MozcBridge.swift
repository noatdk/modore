// Thin Swift facade over the C ABI in bridge/include/mozc_bridge.h.

import Foundation

enum MozcBridgeError: Error {
    case notInitialized
    case conversionFailed(String)
    case stringEncoding
}

func shellConvertSocketPath() -> String {
    "\(MOZC_PROFILE_DIR)/shell-convert.sock"
}

enum MozcBridge {
    struct Candidate: Equatable {
        enum Group: Int {
            case unknown = 0
            case conversion = 1
            case transliteration = 2
            case english = 3
            case hiragana = 4
            case katakana = 5
            case input = 6

            var sectionTitle: String {
                switch self {
                case .conversion: return "Words"
                case .transliteration, .hiragana, .katakana: return "Reading"
                case .english: return "English"
                case .input: return "Input"
                case .unknown: return "Other"
                }
            }

            var sortRank: Int {
                switch self {
                case .conversion: return 0
                case .hiragana, .katakana, .transliteration: return 1
                case .english: return 2
                case .input: return 3
                case .unknown: return 4
                }
            }
        }

        let value: String
        let description: String?
        let prefix: String?
        let suffix: String?
        let id: Int
        let windowCategory: UInt32
        let group: Group

        var detailLabel: String? {
            if let description, !description.isEmpty {
                return description
            }
            switch group {
            case .hiragana: return "Hiragana"
            case .katakana: return "Katakana"
            case .english: return "EN"
            case .input: return "Input"
            default: return nil
            }
        }
    }

    static func initialize(userProfileDir: String? = nil) throws {
        let cstr = userProfileDir.flatMap { $0.cString(using: .utf8) }
        let rc = cstr.withUnsafeBufferPointerOrNil { ptr in
            mozc_bridge_init(ptr)
        }
        if rc != 0 {
            throw MozcBridgeError.conversionFailed(lastError() ?? "init failed (rc=\(rc))")
        }
    }

    enum ConvertTarget {
        case kanji
        case katakana

        var bridgeFlags: UInt32 {
            switch self {
            case .kanji: return 0
            case .katakana: return UInt32(MOZC_CONVERT_FLAG_KATAKANA)
            }
        }
    }

    struct ConversionResult {
        let committed: String
        let candidates: [Candidate]
    }

    static func convert(_ romaji: String, target: ConvertTarget = .kanji) throws -> String {
        return try convertWithCandidates(romaji, target: target, maxCandidates: 0).committed
    }

    static func convertWithCandidates(
        _ romaji: String,
        target: ConvertTarget = .kanji,
        maxCandidates: Int = 8
    ) throws -> ConversionResult {
        guard let utf8 = romaji.cString(using: .utf8) else {
            throw MozcBridgeError.stringEncoding
        }
        let utf8Bytes = utf8.dropLast()
        let inputLen = utf8Bytes.count
        let flags = target.bridgeFlags

        let stringCap = maxCandidates > 0 ? max(maxCandidates * 256, 2048) : 0
        let recordCap = maxCandidates > 0 ? maxCandidates : 0
        var stringBuf = [CChar](repeating: 0, count: stringCap)
        var records = [mozc_bridge_candidate_record_t](
            repeating: mozc_bridge_candidate_record_t(),
            count: recordCap)

        var cap = max(inputLen * 4 + 64, 256)
        while true {
            var commitLen = 0
            var stringsLen = 0
            var candidateCount: Int32 = 0
            var buf = [CChar](repeating: 0, count: cap)
            let rc: Int32 = utf8.withUnsafeBufferPointer { inPtr -> Int32 in
                buf.withUnsafeMutableBufferPointer { outPtr -> Int32 in
                    if stringCap > 0 && recordCap > 0 {
                        return records.withUnsafeMutableBufferPointer { recordPtr in
                            stringBuf.withUnsafeMutableBufferPointer { stringPtr in
                                Int32(mozc_bridge_convert_with_candidate_details_ex(
                                    inPtr.baseAddress,
                                    inputLen,
                                    outPtr.baseAddress,
                                    cap,
                                    &commitLen,
                                    recordPtr.baseAddress,
                                    recordCap,
                                    stringPtr.baseAddress,
                                    stringCap,
                                    &stringsLen,
                                    Int32(maxCandidates),
                                    &candidateCount,
                                    flags
                                ))
                            }
                        }
                    }
                    return Int32(mozc_bridge_convert_with_candidate_details_ex(
                        inPtr.baseAddress,
                        inputLen,
                        outPtr.baseAddress,
                        cap,
                        &commitLen,
                        nil,
                        0,
                        nil,
                        0,
                        nil,
                        0,
                        nil,
                        flags
                    ))
                }
            }
            if rc == 0 {
                let committedData = Data(bytes: buf, count: commitLen)
                guard let committed = String(data: committedData, encoding: .utf8) else {
                    throw MozcBridgeError.stringEncoding
                }
                let candidates = decodeCandidates(
                    records: records,
                    count: Int(candidateCount),
                    stringBuf: stringBuf,
                    originalInput: romaji)
                return ConversionResult(committed: committed, candidates: candidates)
            }
            if rc < 0 {
                throw MozcBridgeError.conversionFailed(lastError() ?? "convert rc=\(rc)")
            }
            cap = Int(rc) + 1
            if cap > 1 << 20 {
                throw MozcBridgeError.conversionFailed("convert wants \(cap) bytes (unreasonably large)")
            }
        }
    }

    static func lastError() -> String? {
        guard let cstr = mozc_bridge_last_error() else { return nil }
        return String(cString: cstr)
    }

    static func shutdown() {
        mozc_bridge_shutdown()
    }

    private static func decodeCandidates(
        records: [mozc_bridge_candidate_record_t],
        count: Int,
        stringBuf: [CChar],
        originalInput: String
    ) -> [Candidate] {
        guard count > 0 else { return [] }
        return stringBuf.withUnsafeBufferPointer { ptr in
            guard let base = ptr.baseAddress else { return [] }
            var out: [Candidate] = []
            out.reserveCapacity(count)
            for i in 0..<count {
                let record = records[i]
                guard let value = decodeField(
                    base: base,
                    offset: Int(record.value_offset),
                    length: Int(record.value_len)) else {
                    continue
                }
                let description = decodeField(
                    base: base,
                    offset: Int(record.description_offset),
                    length: Int(record.description_len))
                let prefix = decodeField(
                    base: base,
                    offset: Int(record.prefix_offset),
                    length: Int(record.prefix_len))
                let suffix = decodeField(
                    base: base,
                    offset: Int(record.suffix_offset),
                    length: Int(record.suffix_len))
                let group = Candidate.Group(rawValue: Int(record.group))
                    ?? (value == originalInput ? .input : .unknown)
                out.append(Candidate(
                    value: value,
                    description: description,
                    prefix: prefix,
                    suffix: suffix,
                    id: Int(record.id),
                    windowCategory: UInt32(record.window_category),
                    group: group
                ))
            }
            return out
        }
    }

    static func convertLine(
        _ text: String,
        caretUTF16: Int,
        target: ConvertTarget = .kanji
    ) throws -> String {
        guard let utf8 = text.cString(using: .utf8) else {
            throw MozcBridgeError.stringEncoding
        }
        let utf8Bytes = utf8.dropLast()
        let inputLen = utf8Bytes.count
        let caretByte = text.utf8ByteOffset(forUTF16Offset: caretUTF16)

        var cap = max(inputLen * 4 + 64, 256)
        while true {
            var outLen: size_t = 0
            var buf = [CChar](repeating: 0, count: cap)
            let rc: Int32 = utf8.withUnsafeBufferPointer { inPtr -> Int32 in
                buf.withUnsafeMutableBufferPointer { outPtr -> Int32 in
                    Int32(mozc_bridge_convert_line(
                        inPtr.baseAddress,
                        inputLen,
                        caretByte,
                        outPtr.baseAddress,
                        cap,
                        &outLen,
                        target.bridgeFlags))
                }
            }
            if rc == 0 {
                let committedData = Data(bytes: buf, count: outLen)
                guard let committed = String(data: committedData, encoding: .utf8) else {
                    throw MozcBridgeError.stringEncoding
                }
                return committed
            }
            if rc > 0 {
                cap = Int(rc)
                continue
            }
            throw MozcBridgeError.conversionFailed(lastError() ?? "shell convert failed")
        }
    }

    static func shellConvertViaLiveHost(_ text: String, caretUTF16: Int) throws -> String {
        try shellConvertViaLiveHost(text, caretUTF16: caretUTF16, target: .kanji)
    }

    static func shellConvertViaLiveHost(
        _ text: String,
        caretUTF16: Int,
        target: ConvertTarget
    ) throws -> String {
        guard let utf8 = text.cString(using: .utf8) else {
            throw MozcBridgeError.stringEncoding
        }
        let utf8Bytes = utf8.dropLast()
        let inputLen = utf8Bytes.count
        let caretByte = text.utf8ByteOffset(forUTF16Offset: caretUTF16)
        let socketPath = shellConvertSocketPath()
        let sessionID = ProcessInfo.processInfo.environment["MODORE_SHELL_SESSION"] ?? ""
        let sessionCString = sessionID.cString(using: .utf8)
        let sessionLabel = sessionID.isEmpty ? "<none>" : sessionID
        let modeLabel = target == .katakana ? "katakana" : "primary"
        Log.shell("client convert request session=\(sessionLabel) mode=\(modeLabel) caret=\(caretUTF16) caretByte=\(caretByte) bytes=\(inputLen) socket=\(socketPath)")
        let modeCString = (target == .katakana ? "katakana" : "primary").cString(using: .utf8)

        var cap = max(inputLen * 4 + 64, 256)
        while true {
            var outLen: size_t = 0
            var buf = [CChar](repeating: 0, count: cap)
            let rc: Int32 = utf8.withUnsafeBufferPointer { inPtr -> Int32 in
                buf.withUnsafeMutableBufferPointer { outPtr -> Int32 in
                    sessionCString.withUnsafeBufferPointerOrNil { sessionPtr in
                        modeCString.withUnsafeBufferPointerOrNil { modePtr in
                            Int32(mozc_bridge_shell_convert_remote(
                                socketPath,
                                sessionPtr,
                                modePtr,
                                inPtr.baseAddress,
                                inputLen,
                                caretByte,
                                outPtr.baseAddress,
                                cap,
                                &outLen))
                        }
                    }
                }
            }
            if rc == 0 {
                let data = Data(bytes: buf, count: outLen)
                guard let converted = String(data: data, encoding: .utf8) else {
                    Log.shell("client convert decode failed bytes=\(outLen)")
                    throw MozcBridgeError.stringEncoding
                }
                Log.shell("client convert ok bytes=\(converted.utf8.count)")
                return converted
            }
            if rc > 0 {
                Log.shell("client convert resize needed=\(rc)")
                cap = Int(rc)
                continue
            }
            Log.shell("client convert failed err=\(lastError() ?? "unknown")")
            throw MozcBridgeError.conversionFailed(lastError() ?? "shell convert failed")
        }
    }

    static func startShellConvertServer(socketPath: String = shellConvertSocketPath()) throws {
        Log.shell("start server path=\(socketPath)")
        let rc = mozc_bridge_shell_server_start(socketPath)
        if rc != 0 {
            Log.shell("start server failed err=\(lastError() ?? "unknown")")
            throw MozcBridgeError.conversionFailed(lastError() ?? "shell server start failed")
        }
        Log.shell("start server ok")
    }

    static func stopShellConvertServer() {
        Log.shell("stop server")
        mozc_bridge_shell_server_stop()
    }

    static func shellBootstrap(
        hotkeyDisplayName: String,
        hostExecutablePath: String? = nil
    ) throws -> String {
        guard let utf8 = hotkeyDisplayName.cString(using: .utf8) else {
            throw MozcBridgeError.stringEncoding
        }
        let hostPathCString = hostExecutablePath.flatMap { $0.cString(using: .utf8) }

        var cap = 1024
        while true {
            var outLen: size_t = 0
            var buf = [CChar](repeating: 0, count: cap)
            let rc: Int32 = utf8.withUnsafeBufferPointer { inPtr -> Int32 in
                hostPathCString.withUnsafeBufferPointerOrNil { hostPtr in
                    buf.withUnsafeMutableBufferPointer { outPtr -> Int32 in
                        Int32(mozc_bridge_shell_bootstrap(
                            inPtr.baseAddress,
                            hostPtr,
                            outPtr.baseAddress,
                            cap,
                            &outLen))
                    }
                }
            }
            if rc == 0 {
                let data = Data(bytes: buf, count: outLen)
                guard let bootstrap = String(data: data, encoding: .utf8) else {
                    throw MozcBridgeError.stringEncoding
                }
                Log.shell("bootstrap ok bytes=\(bootstrap.utf8.count)")
                return bootstrap
            }
            if rc > 0 {
                Log.shell("bootstrap resize needed=\(rc)")
                cap = Int(rc)
                continue
            }
            Log.shell("bootstrap failed err=\(lastError() ?? "unknown")")
            throw MozcBridgeError.conversionFailed(lastError() ?? "shell bootstrap failed")
        }
    }
}

private func decodeField(
    base: UnsafePointer<CChar>,
    offset: Int,
    length: Int
) -> String? {
    guard length > 0 else { return nil }
    let start = UnsafeRawPointer(base.advanced(by: offset))
    let data = Data(bytes: start, count: length)
    return String(data: data, encoding: .utf8)
}

fileprivate extension Optional where Wrapped == [CChar] {
    func withUnsafeBufferPointerOrNil<R>(_ body: (UnsafePointer<CChar>?) -> R) -> R {
        if let arr = self {
            return arr.withUnsafeBufferPointer { body($0.baseAddress) }
        }
        return body(nil)
    }
}
