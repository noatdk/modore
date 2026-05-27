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
        let coreFlags = event.flags.intersection([
            .maskCommand, .maskShift, .maskControl, .maskAlternate
        ])
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
            var actualLen = 0
            event.keyboardGetUnicodeString(
                maxStringLength: 0, actualStringLength: &actualLen, unicodeString: nil)
            guard actualLen > 0 else { return }
            var buf = [UniChar](repeating: 0, count: actualLen)
            var copiedLen = 0
            buf.withUnsafeMutableBufferPointer { bptr in
                event.keyboardGetUnicodeString(
                    maxStringLength: actualLen, actualStringLength: &copiedLen,
                    unicodeString: bptr.baseAddress)
            }
            guard copiedLen > 0 else { return }
            let s = String(utf16CodeUnits: buf, count: copiedLen)
            guard !s.isEmpty else { return }
            s.withCString { cStr in
                lock.lock()
                mdr_shadow_insert(ptr, cStr, s.utf8.count)
                lock.unlock()
            }
        }
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
