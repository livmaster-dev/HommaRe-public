// GameKit.swift - the game's own interface pieces as SwiftUI backgrounds
// (plan C, research notes phone-menus/native/plan-c-native.md): frames, buttons,
// fields and picture frames cut from the player's install by 9-slice, as the
// game itself draws them on a phone (1 texel of the game's interface is
// about 0.56 pt). Where the install lacks a texture the flat Theme look
// stays (the views draw it under the texture until it is read).

import CoreGraphics
import SwiftUI

/// A texture made resizable: its frame in the texture, the strips packed
/// together where the game's art draws something in the middle (a gold
/// button's arrow), the border widths in texels, its scale, and whether its
/// middle is dropped (a frame over a picture, or over its own fill).
struct NineSpec: Hashable {
    let ref: String
    var crop: CGRect? = nil
    var cols: [Range<Int>]? = nil
    var rows: [Range<Int>]? = nil
    /// Left, top, right, bottom, in texels of the cut picture.
    var insets: [CGFloat]
    var ptPerPx: CGFloat = 0.56
    var clearCenter = false
    var tile = false

    /// The borders in points.
    var capInsets: EdgeInsets {
        EdgeInsets(top: insets[1] * ptPerPx, leading: insets[0] * ptPerPx,
                   bottom: insets[3] * ptPerPx, trailing: insets[2] * ptPerPx)
    }
}

/// The set, as plan C's table gives it (all three games share these files).
enum Kit {
    /// The ornate frame (TownInfo): main menu columns and the long lists.
    static let ornate = NineSpec(ref: "UI/TownInfo/Back.(Texture).xdb",
                                 crop: CGRect(x: 3, y: 1, width: 448, height: 725),
                                 insets: [48, 85, 50, 50], ptPerPx: 0.55, clearCenter: true)
    /// Its slate, under the frame's empty middle.
    static let ornateFill = NineSpec(ref: "UI/TownInfo/Back.(Texture).xdb",
                                     crop: CGRect(x: 63, y: 241, width: 290, height: 400),
                                     insets: [0, 0, 0, 0], ptPerPx: 0.55)
    /// The parchment sheet (ChangeProfile): panels, dialogs.
    static let sheet = NineSpec(ref: "UI/MainMenu2/ChangeProfile/ChangeProfile.(Texture).xdb",
                                crop: CGRect(x: 2, y: 1, width: 497, height: 463),
                                insets: [36, 36, 30, 34], ptPerPx: 0.55, clearCenter: true)
    static let sheetFill = NineSpec(ref: "UI/MainMenu2/ChangeProfile/ChangeProfile.(Texture).xdb",
                                    crop: CGRect(x: 52, y: 43, width: 280, height: 86),
                                    insets: [0, 0, 0, 0], ptPerPx: 0.55)
    /// The khaki button (most buttons), its three strips packed to 37x37.
    static func khaki(pressed: Bool) -> NineSpec {
        NineSpec(ref: "UI/Common/Button_136x49/\(pressed ? "Pushed" : "Active").(Texture).xdb",
                 cols: [0..<18, 60..<61, 118..<136], rows: [0..<18, 24..<25, 31..<49],
                 insets: [18, 18, 18, 18], ptPerPx: 0.556)
    }
    /// The gold button (a screen's main action), packed past its arrow.
    static func gold(pressed: Bool) -> NineSpec {
        NineSpec(ref: "Textures/Interface/FortHill/CreatureCountHolder/Button/Background"
                    + (pressed ? "YellowPressed" : "Yellow") + ".(Texture).xdb",
                 cols: [0..<14, 16..<17, 44..<58], rows: [0..<13, 16..<17, 21..<34],
                 insets: [14, 13, 14, 13], ptPerPx: 0.69)
    }
    /// A field: text input, the chosen row.
    static let field = NineSpec(ref: "UI/GameOptions/Templates/ComboBox/Combo.(Texture).xdb",
                                insets: [8, 8, 8, 8], ptPerPx: 0.625)
    /// A picture's frame (map preview, save picture), its middle open.
    static let pictureFrame = NineSpec(ref: "Textures/MissionObjectives/QuestName/QuestName/Normal.xdb",
                                       insets: [12, 12, 12, 12], ptPerPx: 0.5, clearCenter: true)
    /// The options' check box, empty, and the gold tick drawn over it.
    static let checkBox = NineSpec(ref: "UI/GameOptions/Templates/CheckBox/Button_0_Normal.(Texture).xdb",
                                   crop: CGRect(x: 0, y: 0, width: 25, height: 25),
                                   insets: [8, 8, 8, 8], ptPerPx: 0.7)
    static let checkMark = "Textures/Interface/MPCreate/Options/Check.(Texture).xdb"
    /// A round jog dial button: "Left", "Right", "Minus", "Plus".
    static func jog(_ name: String) -> String {
        "UI/Common/JogDial/Buttons/\(name)/Active.(Texture).xdb"
    }
    /// The round "leave the screen" button.
    static func exit(pressed: Bool) -> String {
        "UI/Common/JogDial/Buttons/Exit/\(pressed ? "Pushed" : "Active").(Texture).xdb"
    }

    /// Everything a screen may show first, read off the main thread when
    /// the first native screen comes up, so the frames are there at once.
    static func warm() {
        Task.detached(priority: .userInitiated) {
            for spec in [ornate, ornateFill, sheet, sheetFill, khaki(pressed: false), khaki(pressed: true),
                         gold(pressed: false), gold(pressed: true), field, pictureFrame, checkBox] {
                _ = GameArt.shared.nine(spec)
            }
            _ = GameArt.shared.image(exit(pressed: false))
            _ = GameArt.shared.image(exit(pressed: true))
            // The game's lettering as fonts (GameFont.swift), built from the install.
            GameFont.prepare()
            await MainActor.run { KitStore.shared.ready = true }
        }
    }
}

/// Whether the set has been read (views redraw once it is).
@MainActor
final class KitStore: ObservableObject {
    static let shared = KitStore()
    @Published var ready = false
}

extension GameArt {
    /// A spec's picture: cut, packed, its middle cleared, at its scale; nil
    /// when the install lacks the texture.
    func nine(_ spec: NineSpec) -> PlatformImage? {
        let key = "nine:\(spec.hashValue)"
        if let hit = cached(key) { return hit }
        guard let source = image(spec.ref)?.cgImage,
              let out = NineCut.cut(source, spec) else { return nil }
        let image = PlatformImage.of(out, ptPerPx: spec.ptPerPx)
        store(image, key, cost: out.bytesPerRow * out.height)
        return image
    }
}

/// The pixel work of a cut, on RGBA bytes (rows top first).
enum NineCut {
    static func cut(_ source: CGImage, _ spec: NineSpec) -> CGImage? {
        var img = source
        if let crop = spec.crop, let c = img.cropping(to: crop) { img = c }
        guard var (px, w, h) = rgba(img) else { return nil }
        if let cols = spec.cols, let rows = spec.rows {
            let nw = cols.reduce(0) { $0 + $1.count }, nh = rows.reduce(0) { $0 + $1.count }
            var out = [UInt8](repeating: 0, count: nw * nh * 4)
            var oy = 0
            for r in rows {
                for y in r where y < h {
                    var ox = 0
                    for c in cols {
                        for x in c where x < w {
                            let s = (y * w + x) * 4, d = (oy * nw + ox) * 4
                            out[d] = px[s]; out[d + 1] = px[s + 1]; out[d + 2] = px[s + 2]; out[d + 3] = px[s + 3]
                            ox += 1
                        }
                    }
                    oy += 1
                }
            }
            (px, w, h) = (out, nw, nh)
        }
        if spec.clearCenter {
            let l = Int(spec.insets[0]), t = Int(spec.insets[1]), r = Int(spec.insets[2]), b = Int(spec.insets[3])
            if w > l + r, h > t + b {
                for y in t..<(h - b) {
                    for x in l..<(w - r) {
                        let i = (y * w + x) * 4
                        px[i] = 0; px[i + 1] = 0; px[i + 2] = 0; px[i + 3] = 0
                    }
                }
            }
        }
        return image(px, w, h)
    }

    static func rgba(_ img: CGImage) -> ([UInt8], Int, Int)? {
        let w = img.width, h = img.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ok = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? (px, w, h) : nil
    }

    static func image(_ px: [UInt8], _ w: Int, _ h: Int) -> CGImage? {
        var copy = px
        return copy.withUnsafeMutableBytes { buf in
            CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpaceCreateDeviceRGB(),
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)?.makeImage()
        }
    }
}

/// A spec drawn resizable, or `fallback` while it is read or when the
/// install lacks it.
struct GameNine<Fallback: View>: View {
    let spec: NineSpec
    @ViewBuilder var fallback: () -> Fallback
    @ObservedObject private var kit = KitStore.shared

    var body: some View {
        if let image = GameArt.shared.cachedNine(spec) {
            Image(platformImage: image)
                .resizable(capInsets: spec.capInsets, resizingMode: spec.tile ? .tile : .stretch)
                .interpolation(.medium)
        } else {
            fallback()
                .task(id: spec) {
                    // Not warmed (a spec outside the set): read it here.
                    let found = await Task.detached(priority: .userInitiated) { GameArt.shared.nine(spec) != nil }.value
                    if found { kit.objectWillChange.send() }
                }
        }
    }
}

extension GameArt {
    /// A spec's picture if it was already cut (no reading on the main thread).
    func cachedNine(_ spec: NineSpec) -> PlatformImage? { cached("nine:\(spec.hashValue)") }
}

/// A framed panel: the frame, its fill under the open middle, the content
/// inside the frame's border (`inset` points in from each edge).
struct FramedBackground: ViewModifier {
    let frame: NineSpec
    let fill: NineSpec
    let inset: EdgeInsets
    let flat: Color

    func body(content: Content) -> some View {
        content
            .padding(inset)
            .background {
                ZStack {
                    // Under the frame's open middle, a little under its inner edge:
                    // the slate (the game's is see-through) over a dark base, so
                    // light text reads on it over a bright scene.
                    ZStack {
                        flat.opacity(0.7)
                        GameNine(spec: fill) { flat }
                    }
                        .padding(EdgeInsets(top: max(0, frame.capInsets.top - 2),
                                            leading: max(0, frame.capInsets.leading - 2),
                                            bottom: max(0, frame.capInsets.bottom - 2),
                                            trailing: max(0, frame.capInsets.trailing - 2)))
                    GameNine(spec: frame) {
                        RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                            .strokeBorder(Theme.goldDim, lineWidth: 1)
                    }
                }
            }
    }
}

/// The ornate frame round a column (the main menu's, the game's menu), its
/// title in blue ink on the frame's parchment strip, as the game's own
/// TownInfo panels carry theirs.
struct OrnateBackground: ViewModifier {
    let title: String
    static let ink = Color(red: 0.13, green: 0.22, blue: 0.35) // #22385A

    func body(content: Content) -> some View {
        // The content inside the slate, a few points in from its edges (the
        // frame's borders: 26 and 28 pt at the sides, 28 at the bottom).
        content
            .modifier(FramedBackground(frame: Kit.ornate, fill: Kit.ornateFill,
                                       inset: EdgeInsets(top: 50, leading: 29, bottom: 31, trailing: 30),
                                       flat: Theme.panel))
            .overlay(alignment: .top) {
                // Centred on the parchment strip, under the frame's band.
                Text(title)
                    .font(Theme.heading(22))
                    .foregroundStyle(OrnateBackground.ink)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    .padding(.horizontal, 30)
                    .frame(height: 30)
                    .padding(.top, 17)
            }
    }
}

extension View {
    func ornate(title: String) -> some View { modifier(OrnateBackground(title: title)) }
}

/// A list's drop-down mark: the game's ComboBox arrow.
struct DropIcon: View {
    var body: some View {
        GameImage(ref: "UI/GameOptions/Templates/ComboBox/ListIconBtn.(Texture).xdb",
                  placeholder: "chevron.up.chevron.down")
            .frame(width: 22, height: 22)
    }
}
