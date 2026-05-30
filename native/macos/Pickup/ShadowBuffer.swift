import ApplicationServices
import Cocoa

let gShadowBuffer = ShadowBufferHost()

final class ShadowBufferHost {
    private let ptr: OpaquePointer
    private let lock = NSLock()

    init() {
        guard let p = mdr_shadow_create() else { fatalError("mdr_shadow_create") }
        ptr = p
    }

    deinit { mdr_shadow_destroy(ptr) }

    func feed(_ event: CGEvent) {
        let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let coreFlags = event.flags.intersection(kCoreModifierFlags)
        let modsNoShift = coreFlags.subtracting(.maskShift)

        if modsNoShift.contains(.maskControl) {
            lock.lock(); mdr_shadow_reset(ptr); lock.unlock()
            return
        }

        if modsNoShift.contains(.maskCommand) {
            lock.lock()
            switch keyCode {
            case kVK_LeftArrow, kVK_Home:
                mdr_shadow_line_jump(ptr, 0)
            case kVK_RightArrow, kVK_End:
                mdr_shadow_line_jump(ptr, 1)
            default:
                mdr_shadow_reset(ptr)
            }
            lock.unlock()
            return
        }

        if modsNoShift.contains(.maskAlternate) {
            lock.lock()
            switch keyCode {
            case kVK_LeftArrow:
                mdr_shadow_word_jump(ptr, 0)
            case kVK_RightArrow:
                mdr_shadow_word_jump(ptr, 1)
            default:
                mdr_shadow_reset(ptr)
            }
            lock.unlock()
            return
        }

        switch keyCode {
        case kVK_LeftArrow:
            lock.lock(); mdr_shadow_move(ptr, -1); lock.unlock()
        case kVK_RightArrow:
            lock.lock(); mdr_shadow_move(ptr, 1); lock.unlock()
        case kVK_Home:
            lock.lock(); mdr_shadow_line_jump(ptr, 0); lock.unlock()
        case kVK_End:
            lock.lock(); mdr_shadow_line_jump(ptr, 1); lock.unlock()
        case kVK_Backspace:
            lock.lock(); mdr_shadow_backspace(ptr); lock.unlock()
        case kVK_ForwardDelete:
            lock.lock(); mdr_shadow_forward_delete(ptr); lock.unlock()
        case kVK_Return, kVK_Tab, kVK_Space, kVK_Escape,
             kVK_UpArrow, kVK_DownArrow, kVK_PageUp, kVK_PageDown:
            lock.lock(); mdr_shadow_reset(ptr); lock.unlock()
        default:
            guard let s = unicodeString(from: event), !s.isEmpty else { return }
            s.withCString { cStr in
                lock.lock()
                mdr_shadow_insert(ptr, cStr, s.utf8.count)
                lock.unlock()
            }
        }
    }

    // Build a snapshot of the current shadow buffer state as labeled rows for
    // the debug overlay. Does NOT reset the buffer. Safe from any queue.
    func debugRows() -> [DebugRow] {
        lock.lock()
        defer { lock.unlock() }
        let valid = mdr_shadow_is_valid(ptr) != 0
        var len = 0
        let text: String
        let cursor: Int
        if valid, let raw = mdr_shadow_text(ptr, &len) {
            text = String(cString: raw)
            cursor = Int(mdr_shadow_cursor_byte(ptr))
        } else {
            text = ""
            cursor = 0
        }
        let visual = Self.cursorVisual(text: text, cursorByte: cursor)
        return [
            DebugRow(label: "valid",  value: valid ? "yes" : "no"),
            DebugRow(label: "text",   value: text.isEmpty ? "(empty)" : text),
            DebugRow(label: "cursor", value: "\(cursor)B"),
            DebugRow(label: "view",   value: visual.isEmpty ? "(empty)" : visual),
        ]
    }

    private static func cursorVisual(text: String, cursorByte: Int) -> String {
        let bytes = Array(text.utf8)
        guard cursorByte <= bytes.count else { return text + "‸" }
        let before = String(bytes: Array(bytes[..<cursorByte]), encoding: .utf8) ?? ""
        let after  = String(bytes: Array(bytes[cursorByte...]), encoding: .utf8) ?? ""
        return before + "‸" + after
    }

    // Atomically returns (text, cursorByteOffset) and resets the buffer.
    // Returns nil when the buffer has no accumulated content.
    func takeSnapshot() -> (text: String, cursorByte: Int)? {
        lock.lock()
        defer { lock.unlock() }
        guard mdr_shadow_is_valid(ptr) != 0 else { return nil }
        var len: Int = 0
        guard let raw = mdr_shadow_text(ptr, &len), len > 0 else { return nil }
        let text = String(cString: raw)
        let cursor = Int(mdr_shadow_cursor_byte(ptr))
        mdr_shadow_reset(ptr)
        return text.isEmpty ? nil : (text, cursor)
    }
}
