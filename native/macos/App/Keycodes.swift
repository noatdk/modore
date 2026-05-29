// Virtual key codes used across the host. CGKeyCode-typed so call sites
// need no casts. Values match Carbon HIToolbox/Events.h.

import CoreGraphics

// MARK: - ANSI keys
let kVK_ANSI_C: CGKeyCode = 0x08
let kVK_ANSI_V: CGKeyCode = 0x09

// MARK: - Editing
let kVK_Backspace: CGKeyCode = 0x33    // main-keyboard Delete
let kVK_ForwardDelete: CGKeyCode = 0x75
let kVK_Return: CGKeyCode = 0x24
let kVK_Tab: CGKeyCode = 0x30
let kVK_Space: CGKeyCode = 0x31
let kVK_Escape: CGKeyCode = 0x35

// MARK: - Navigation
let kVK_LeftArrow: CGKeyCode = 0x7B
let kVK_RightArrow: CGKeyCode = 0x7C
let kVK_UpArrow: CGKeyCode = 0x7E
let kVK_DownArrow: CGKeyCode = 0x7D
let kVK_Home: CGKeyCode = 0x73
let kVK_End: CGKeyCode = 0x77
let kVK_PageUp: CGKeyCode = 0x74
let kVK_PageDown: CGKeyCode = 0x79
