// Platform.swift - what the views need from UIKit on iOS and from AppKit on
// the Mac, under one name: pictures, fonts and the few iOS-only modifiers.
// The Mac shows only the skill wheel (native/native_screens.c), but the
// module builds whole.

import SwiftUI

#if os(iOS)
import UIKit

typealias PlatformImage = UIImage
typealias PlatformFont = UIFont
typealias PlatformFontDescriptor = UIFontDescriptor

extension UIImage {
    /// A picture of `cg` drawn `ptPerPx` points per pixel.
    static func of(_ cg: CGImage, ptPerPx: CGFloat = 1) -> UIImage {
        UIImage(cgImage: cg, scale: 1 / ptPerPx, orientation: .up)
    }
}

extension UIFont {
    static func with(_ descriptor: UIFontDescriptor, size: CGFloat) -> UIFont? {
        UIFont(descriptor: descriptor, size: size)
    }
}
#else
import AppKit

typealias PlatformImage = NSImage
typealias PlatformFont = NSFont
typealias PlatformFontDescriptor = NSFontDescriptor

extension NSImage {
    /// A picture of `cg` drawn `ptPerPx` points per pixel.
    static func of(_ cg: CGImage, ptPerPx: CGFloat = 1) -> NSImage {
        NSImage(cgImage: cg, size: NSSize(width: CGFloat(cg.width) * ptPerPx, height: CGFloat(cg.height) * ptPerPx))
    }

    var cgImage: CGImage? {
        var rect = CGRect(origin: .zero, size: size)
        return cgImage(forProposedRect: &rect, context: nil, hints: nil)
    }
}

extension NSFont {
    static func with(_ descriptor: NSFontDescriptor, size: CGFloat) -> NSFont? {
        NSFont(descriptor: descriptor, size: size)
    }
}

/// iOS's text fields' capitals: nothing to do with a Mac keyboard.
struct TextInputAutocapitalization {
    static let words = Self(), sentences = Self(), never = Self(), characters = Self()
}

extension View {
    func textInputAutocapitalization(_: TextInputAutocapitalization?) -> some View { self }
}
#endif

extension Image {
    init(platformImage image: PlatformImage) {
        #if os(iOS)
        self.init(uiImage: image)
        #else
        self.init(nsImage: image)
        #endif
    }
}
