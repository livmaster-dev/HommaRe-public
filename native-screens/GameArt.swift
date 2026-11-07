// GameArt.swift - the game's own pictures (faction icons, hero portraits,
// bonus icons, map previews...) read from the player's install, as the
// launcher reads its skin (kit/host/launcher/apple/LauncherSkin.swift: the
// pak reader and the DDS decoder below are copies of its, which a game module
// does not link). The game side names a texture by its path inside the paks
// ("/Textures/Icons/Heroes/Godric.(Texture).xdb", or the .dds itself).

import Compression
import CoreGraphics
import Foundation
import SwiftUI

/// The paks of the install the game runs from (RECOMP_GAME_ROOT, set by the
/// host): the folders the game mounts (data/, DataLS/; HoF data/, dataa1/),
/// then the user maps in Maps/, a later one
/// overriding an earlier one as the game mounts them, and the decoded
/// pictures, kept while the app runs.
final class GameArt {
    static let shared = GameArt()
    private var paks: [PakArchive]?
    /// Decoded pictures by reference (and crop), dropped oldest-first past
    /// 48 MB of pixels: a hero list's portraits stay, campaign art and map
    /// previews seen once do not pile up for the rest of the session.
    private let cache: NSCache<NSString, PlatformImage> = {
        let c = NSCache<NSString, PlatformImage>()
        c.totalCostLimit = 48 << 20
        return c
    }()
    /// A texture reference's picture path (its .xdb read once).
    private var paths: [String: String] = [:]
    /// The interface pieces cut from textures (GameKit.swift): a few MB,
    /// on every screen, so never dropped.
    private var kit: [String: PlatformImage] = [:]
    private let lock = NSLock()

    func cached(_ key: String) -> PlatformImage? {
        lock.lock()
        defer { lock.unlock() }
        return kit[key]
    }

    func store(_ image: PlatformImage, _ key: String, cost: Int) {
        lock.lock()
        kit[key] = image
        lock.unlock()
    }

    private func archives() -> [PakArchive] {
        // Under the lock: its callers hold it.
        if let paks { return paks }
        var found: [PakArchive] = []
        if let root = ProcessInfo.processInfo.environment["RECOMP_GAME_ROOT"] {
            let base = URL(fileURLWithPath: root)
            // The game names its own folders (native_screens.c NS_PAK_DIRS).
            let dirs = ProcessInfo.processInfo.environment["RECOMP_NATIVE_PAK_DIRS"]?
                .split(separator: ",").map(String.init) ?? ["data", "DataLS"]
            for dir in dirs {
                let d = base.appendingPathComponent(dir)
                guard let names = try? FileManager.default.contentsOfDirectory(atPath: d.path) else { continue }
                let sorted = names.filter { $0.lowercased().hasSuffix(".pak") }.sorted { a, b in
                    let la = a.lowercased(), lb = b.lowercased()
                    if la == "data.pak" || lb == "data.pak" { return la == "data.pak" && lb != "data.pak" }
                    return la < lb
                }
                found += sorted.compactMap { PakArchive(url: d.appendingPathComponent($0)) }
            }
            // User maps (Maps/*.h5m, zips like the paks), mounted after them:
            // their thumbnails are read from inside.
            let maps = base.appendingPathComponent("Maps")
            if let names = try? FileManager.default.contentsOfDirectory(atPath: maps.path) {
                found += names.filter { $0.lowercased().hasSuffix(".h5m") }.sorted()
                    .compactMap { PakArchive(url: maps.appendingPathComponent($0)) }
            }
        }
        paks = found
        return found
    }

    /// A file of the paks (a description, a text), the last pak's copy.
    func file(_ ref: String) -> Data? {
        var path = ref
        if let hash = path.firstIndex(of: "#") { path = String(path[..<hash]) }
        while path.hasPrefix("/") { path.removeFirst() }
        lock.lock()
        defer { lock.unlock() }
        for pak in archives().reversed() {
            if let data = pak.read(path) { return data }
        }
        return nil
    }

    /// A text file of the game (UTF-16 with its BOM), its markup left out.
    func text(_ ref: String) -> String? {
        guard let data = file(ref) else { return nil }
        let s = String(data: data, encoding: .utf16) ?? String(decoding: data, as: UTF8.self)
        return GameArt.plain(s)
    }

    /// The game's markup taken out: tags (<h1>, <color=...>) go, line breaks close up.
    static func plain(_ s: String) -> String {
        var out = ""
        var inTag = false
        for ch in s {
            if ch == "<" { inTag = true; continue }
            if ch == ">" { inTag = false; continue }
            if !inTag && ch != "\r" { out.append(ch) }
        }
        return out.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// A texture reference's picture in the paks: a Texture description
    /// (.xdb) names its picture in <DestName href>, relative to its folder,
    /// as the game reads it; else the .dds beside it (pakPath).
    func picturePath(_ ref: String) -> String {
        var p = ref
        if let hash = p.firstIndex(of: "#") { p = String(p[..<hash]) }
        while p.hasPrefix("/") { p.removeFirst() }
        guard p.lowercased().hasSuffix(".xdb"), let data = file(p),
              let xml = String(data: data, encoding: .utf8),
              let r = xml.range(of: "<DestName href=\"") else { return Self.pakPath(ref) }
        let rest = xml[r.upperBound...]
        guard let end = rest.firstIndex(of: "\""), end > rest.startIndex else { return Self.pakPath(ref) }
        let dest = String(rest[..<end])
        if dest.hasPrefix("/") { return String(dest.drop(while: { $0 == "/" })) }
        let folder = p.split(separator: "/").dropLast().joined(separator: "/")
        return folder.isEmpty ? dest : folder + "/" + dest
    }

    /// The pak path of a texture reference: no leading slash, no xpointer,
    /// a Texture description's picture beside it (X.(Texture).xdb -> X.(Texture).dds).
    static func pakPath(_ ref: String) -> String {
        var p = ref
        if let hash = p.firstIndex(of: "#") { p = String(p[..<hash]) }
        while p.hasPrefix("/") { p.removeFirst() }
        if p.lowercased().hasSuffix(".xdb") { p = String(p.dropLast(4)) + ".dds" }
        return p
    }

    /// The picture, cropped to (x, y, w, h) when given; nil when the install
    /// has no such texture.
    func image(_ ref: String, crop: (Int, Int, Int, Int)? = nil) -> PlatformImage? {
        let key = (crop.map { "\(ref)@\($0.0),\($0.1),\($0.2),\($0.3)" } ?? ref) as NSString
        if let hit = cache.object(forKey: key) { return hit }
        lock.lock()
        let known = paths[ref]
        lock.unlock()
        let path = known ?? picturePath(ref)
        lock.lock()
        defer { lock.unlock() }
        paths[ref] = path
        if let hit = cache.object(forKey: key) { return hit }
        for pak in archives().reversed() {
            guard let data = pak.read(path) else { continue }
            if let cg = DDSImage.decode(data, crop: crop ?? (0, 0, Int.max, Int.max)) {
                let image = PlatformImage.of(cg)
                cache.setObject(image, forKey: key, cost: cg.bytesPerRow * cg.height)
                return image
            }
            print("[screens] art: \(path) does not decode (\(data.count) bytes)"); fflush(stdout)
            return nil
        }
        print("[screens] art: \(path) is in none of \(archives().count) archives"); fflush(stdout)
        return nil
    }
}

/// A game picture by its texture reference, read off the main thread; a
/// placeholder symbol until then, or when the install lacks it.
struct GameImage: View {
    let ref: String?
    var crop: (Int, Int, Int, Int)? = nil
    var placeholder = "photo"
    /// Fill the space (cropping) instead of fitting in it.
    var fill = false
    /// Tried when the install has no `ref`.
    var fallback: String? = nil
    @State private var image: PlatformImage?

    var body: some View {
        Group {
            if let image {
                Image(platformImage: image).resizable().aspectRatio(contentMode: fill ? .fill : .fit)
            } else {
                Image(systemName: placeholder).foregroundStyle(Theme.goldDim)
            }
        }
        .task(id: ref) {
            guard let ref, !ref.isEmpty else { image = nil; return }
            let crop = crop, fallback = fallback
            let loaded = await Task.detached(priority: .userInitiated) {
                GameArt.shared.image(ref, crop: crop) ?? fallback.flatMap { GameArt.shared.image($0, crop: crop) }
            }.value
            image = loaded
        }
    }
}

/// A game picture as a banner `height` tall, filling its width; nothing at
/// all, taking no room, when the install has neither `ref` nor `fallback`.
struct GameBanner: View {
    let ref: String?
    var fallback: String? = nil
    var height: CGFloat
    var radius: CGFloat = 8
    @State private var image: PlatformImage?

    var body: some View {
        Group {
            if let image {
                Color.clear
                    .frame(height: height)
                    .frame(maxWidth: .infinity)
                    .overlay { Image(platformImage: image).resizable().aspectRatio(contentMode: .fill) }
                    .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
            } else {
                // Takes no room, but is a view: an empty one never loads.
                Color.clear.frame(height: 0)
            }
        }
        .task(id: ref) {
            let ref = ref, fallback = fallback
            image = await Task.detached(priority: .userInitiated) {
                ref.flatMap { GameArt.shared.image($0) } ?? fallback.flatMap { GameArt.shared.image($0) }
            }.value
        }
    }
}

/// A game picture filling whatever space it is given (a card's background),
/// cropped; clear until it loads or when the install lacks it.
struct GameBackdrop: View {
    let ref: String?
    @State private var image: PlatformImage?

    var body: some View {
        Color.clear
            .overlay {
                if let image {
                    Image(platformImage: image).resizable().aspectRatio(contentMode: .fill)
                }
            }
            .clipped()
            .task(id: ref) {
                let ref = ref
                image = await Task.detached(priority: .userInitiated) { ref.flatMap { GameArt.shared.image($0) } }.value
            }
    }
}

// MARK: - Pak archives

/// Reads single entries of a game pak: a plain zip (stored or deflated, no
/// zip64), indexed by its central directory with names compared without case.
struct PakArchive {
    private struct Entry {
        var method: UInt16
        var compressedSize: Int
        var size: Int
        var localHeader: UInt64
    }

    let url: URL
    private var entries: [String: Entry] = [:]

    init?(url: URL) {
        self.url = url
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let end = try? h.seekToEnd(), end >= 22 else { return nil }
        // The end-of-central-directory record: within the last 64 KiB + 22.
        let tailLen = min(end, 65_557)
        try? h.seek(toOffset: end - tailLen)
        guard let tail = try? h.read(upToCount: Int(tailLen)), tail.count == Int(tailLen) else { return nil }
        var eocd = -1
        var i = tail.count - 22
        while i >= 0 {
            if tail.u32(i) == 0x0605_4b50 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { return nil }
        let cdSize = Int(tail.u32(eocd + 12)), cdOffset = UInt64(tail.u32(eocd + 16))
        guard cdOffset != 0xffff_ffff, cdOffset + UInt64(cdSize) <= end else { return nil }
        try? h.seek(toOffset: cdOffset)
        guard let cd = try? h.read(upToCount: cdSize), cd.count == cdSize else { return nil }
        var p = 0
        while p + 46 <= cd.count, cd.u32(p) == 0x0201_4b50 {
            let nameLen = Int(cd.u16(p + 28)), extraLen = Int(cd.u16(p + 30)), commentLen = Int(cd.u16(p + 32))
            guard p + 46 + nameLen <= cd.count else { break }
            let nameData = cd.subdata(in: (cd.startIndex + p + 46)..<(cd.startIndex + p + 46 + nameLen))
            let name = String(decoding: nameData, as: UTF8.self).lowercased()
            entries[name] = Entry(method: cd.u16(p + 10), compressedSize: Int(cd.u32(p + 20)), size: Int(cd.u32(p + 24)),
                                  localHeader: UInt64(cd.u32(p + 42)))
            p += 46 + nameLen + extraLen + commentLen
        }
        if entries.isEmpty { return nil }
    }

    func read(_ path: String) -> Data? {
        guard let e = entries[path.lowercased()], let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        try? h.seek(toOffset: e.localHeader)
        guard let local = try? h.read(upToCount: 30), local.count == 30, local.u32(0) == 0x0403_4b50 else { return nil }
        try? h.seek(toOffset: e.localHeader + 30 + UInt64(local.u16(26)) + UInt64(local.u16(28)))
        guard let raw = try? h.read(upToCount: e.compressedSize), raw.count == e.compressedSize else { return nil }
        switch e.method {
        case 0:
            return raw
        case 8:
            // Compression's ZLIB is raw deflate, the zip method 8 stream.
            var out = Data(count: e.size)
            let n = out.withUnsafeMutableBytes { dst in
                raw.withUnsafeBytes { src in
                    compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, e.size,
                                              src.bindMemory(to: UInt8.self).baseAddress!, raw.count, nil, COMPRESSION_ZLIB)
                }
            }
            return n == e.size ? out : nil
        default:
            return nil
        }
    }
}

private extension Data {
    func u16(_ at: Int) -> UInt16 {
        UInt16(self[startIndex + at]) | UInt16(self[startIndex + at + 1]) << 8
    }

    func u32(_ at: Int) -> UInt32 {
        UInt32(u16(at)) | UInt32(u16(at + 2)) << 16
    }
}

// MARK: - DDS

/// DirectDraw Surface textures as the game ships them: DXT1, DXT3 and DXT5
/// blocks, or uncompressed pixels described by channel masks.
enum DDSImage {
    /// The texture cut to `crop` (x, y, width, height), or to its opaque
    /// pixels without one.
    static func decode(_ d: Data, crop: (Int, Int, Int, Int)?, light: (Double, Double, Double)? = nil) -> CGImage? {
        guard d.count >= 128, d.u32(0) == 0x2053_4444 /* "DDS " */ else { return nil }
        let height = Int(d.u32(12)), width = Int(d.u32(16))
        let pfFlags = d.u32(80), fourCC = d.u32(84), bits = Int(d.u32(88))
        guard width > 0, height > 0, width <= 8192, height <= 8192 else { return nil }
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        let ok: Bool = d.withUnsafeBytes { raw in
            let src = raw.bindMemory(to: UInt8.self)
            let body = UnsafeBufferPointer(rebasing: src[128...])
            if pfFlags & 0x4 != 0 {
                switch fourCC {
                case 0x3154_5844: return decodeBlocks(body, width, height, kind: 1, into: &rgba)
                case 0x3354_5844: return decodeBlocks(body, width, height, kind: 3, into: &rgba)
                case 0x3554_5844: return decodeBlocks(body, width, height, kind: 5, into: &rgba)
                default: return false
                }
            }
            let masks = (d.u32(92), d.u32(96), d.u32(100), pfFlags & 0x1 != 0 ? d.u32(104) : 0)
            return decodeMasked(body, width, height, bits: bits, masks: masks, into: &rgba)
        }
        guard ok else { return nil }
        if let light {
            for i in stride(from: 0, to: rgba.count, by: 4) {
                let v = max(rgba[i], rgba[i + 1], rgba[i + 2])
                rgba[i] = UInt8(255 * light.0)
                rgba[i + 1] = UInt8(255 * light.1)
                rgba[i + 2] = UInt8(255 * light.2)
                rgba[i + 3] = v
            }
        }
        // An additive texture keeps its whole size (its dark parts are clear now).
        var (cx, cy, cw, ch) = crop ?? (light != nil ? (0, 0, width, height) : opaqueBounds(rgba, width, height))
        cx = min(cx, width - 1); cy = min(cy, height - 1)
        cw = min(cw, width - cx); ch = min(ch, height - cy)
        var out = [UInt8](repeating: 0, count: cw * ch * 4)
        for y in 0..<ch {
            for x in 0..<cw {
                let s = ((y + cy) * width + x + cx) * 4, o = (y * cw + x) * 4
                // Premultiplied, the form Core Graphics draws fastest.
                let a = UInt16(rgba[s + 3])
                out[o] = UInt8((UInt16(rgba[s]) * a + 127) / 255)
                out[o + 1] = UInt8((UInt16(rgba[s + 1]) * a + 127) / 255)
                out[o + 2] = UInt8((UInt16(rgba[s + 2]) * a + 127) / 255)
                out[o + 3] = UInt8(a)
            }
        }
        guard let provider = CGDataProvider(data: Data(out) as CFData) else { return nil }
        return CGImage(width: cw, height: ch, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: cw * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    private static func opaqueBounds(_ px: [UInt8], _ w: Int, _ h: Int) -> (Int, Int, Int, Int) {
        var minX = w, minY = h, maxX = -1, maxY = -1
        for y in 0..<h {
            for x in 0..<w where px[(y * w + x) * 4 + 3] != 0 {
                minX = min(minX, x); maxX = max(maxX, x)
                minY = min(minY, y); maxY = max(maxY, y)
            }
        }
        return maxX < 0 ? (0, 0, w, h) : (minX, minY, maxX - minX + 1, maxY - minY + 1)
    }

    private static func rgb565(_ c: UInt16) -> (Int, Int, Int) {
        let r = Int(c >> 11 & 31), g = Int(c >> 5 & 63), b = Int(c & 31)
        return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    }

    private static func decodeBlocks(_ s: UnsafeBufferPointer<UInt8>, _ w: Int, _ h: Int, kind: Int,
                                     into px: inout [UInt8]) -> Bool {
        let blockBytes = kind == 1 ? 8 : 16
        let bw = (w + 3) / 4, bh = (h + 3) / 4
        guard s.count >= bw * bh * blockBytes else { return false }
        func u16(_ i: Int) -> UInt16 { UInt16(s[i]) | UInt16(s[i + 1]) << 8 }
        var off = 0
        for by in 0..<bh {
            for bx in 0..<bw {
                var alpha = [UInt8](repeating: 255, count: 16)
                var c = off
                if kind == 3 {
                    for i in 0..<16 {
                        let nib = (s[off + i / 2] >> (UInt8(i % 2) * 4)) & 0xf
                        alpha[i] = nib << 4 | nib
                    }
                    c += 8
                } else if kind == 5 {
                    let a0 = Int(s[off]), a1 = Int(s[off + 1])
                    var table = [a0, a1, 0, 0, 0, 0, 0, 0]
                    if a0 > a1 {
                        for i in 1...6 { table[i + 1] = ((7 - i) * a0 + i * a1) / 7 }
                    } else {
                        for i in 1...4 { table[i + 1] = ((5 - i) * a0 + i * a1) / 5 }
                        table[6] = 0; table[7] = 255
                    }
                    var bitsA: UInt64 = 0
                    for i in 0..<6 { bitsA |= UInt64(s[off + 2 + i]) << (8 * UInt64(i)) }
                    for i in 0..<16 { alpha[i] = UInt8(table[Int(bitsA >> (3 * UInt64(i)) & 7)]) }
                    c += 8
                }
                let c0 = u16(c), c1 = u16(c + 2)
                let (r0, g0, b0) = rgb565(c0), (r1, g1, b1) = rgb565(c1)
                var pal = [(r0, g0, b0, 255), (r1, g1, b1, 255), (0, 0, 0, 255), (0, 0, 0, 255)]
                if c0 > c1 || kind != 1 {
                    pal[2] = ((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255)
                    pal[3] = ((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255)
                } else {
                    pal[2] = ((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255)
                    pal[3] = (0, 0, 0, 0)
                }
                let idx = UInt32(s[c + 4]) | UInt32(s[c + 5]) << 8 | UInt32(s[c + 6]) << 16 | UInt32(s[c + 7]) << 24
                for i in 0..<16 {
                    let x = bx * 4 + i % 4, y = by * 4 + i / 4
                    guard x < w, y < h else { continue }
                    let p = pal[Int(idx >> (2 * UInt32(i)) & 3)]
                    let o = (y * w + x) * 4
                    px[o] = UInt8(p.0); px[o + 1] = UInt8(p.1); px[o + 2] = UInt8(p.2)
                    px[o + 3] = kind == 1 ? UInt8(p.3) : alpha[i]
                }
                off += blockBytes
            }
        }
        return true
    }

    private static func decodeMasked(_ s: UnsafeBufferPointer<UInt8>, _ w: Int, _ h: Int, bits: Int,
                                     masks: (UInt32, UInt32, UInt32, UInt32), into px: inout [UInt8]) -> Bool {
        guard bits == 16 || bits == 24 || bits == 32 else { return false }
        let bpp = bits / 8
        guard s.count >= w * h * bpp else { return false }
        func channel(_ v: UInt32, _ mask: UInt32) -> UInt8 {
            guard mask != 0 else { return 255 }
            let shift = UInt32(mask.trailingZeroBitCount)
            let maxV = mask >> shift
            return UInt8((UInt64((v & mask) >> shift) * 255 + UInt64(maxV) / 2) / UInt64(maxV))
        }
        for i in 0..<(w * h) {
            var v: UInt32 = 0
            for b in 0..<bpp { v |= UInt32(s[i * bpp + b]) << (8 * UInt32(b)) }
            px[i * 4] = channel(v, masks.0)
            px[i * 4 + 1] = channel(v, masks.1)
            px[i * 4 + 2] = channel(v, masks.2)
            px[i * 4 + 3] = channel(v, masks.3)
        }
        return true
    }
}
