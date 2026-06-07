// @testable
//
// CoreFoundation values returned by Accessibility APIs are supplied by other
// processes. Check their dynamic CF type before bridging to a concrete type.

import Foundation

func cfTypeMatches(_ ref: CFTypeRef?, _ expected: CFTypeID) -> Bool {
    guard let ref else { return false }
    return CFGetTypeID(ref) == expected
}
