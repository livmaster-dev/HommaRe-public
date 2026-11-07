// Theme.swift - the look the native screens share: dark leather panels with
// a gold edge over the game's own 3D scene, the main action in the game's
// orange (the "variant C" menus of the phone-menus branch), sized for a
// finger: nothing to press under 44 pt.

import SwiftUI

enum Theme {
    static let panel = Color(red: 0.16, green: 0.12, blue: 0.09).opacity(0.94)
    static let well = Color(red: 0.10, green: 0.08, blue: 0.06).opacity(0.9)
    static let row = Color(red: 0.22, green: 0.17, blue: 0.12)
    static let rowSelected = Color(red: 0.36, green: 0.27, blue: 0.16)
    static let gold = Color(red: 0.97, green: 0.87, blue: 0.55)      // #F7DE8C, plan C
    static let goldDim = Color(red: 0.55, green: 0.43, blue: 0.24)
    static let text = Color(red: 0.95, green: 0.90, blue: 0.81)      // #F1E6CF
    static let textDim = Color(red: 0.85, green: 0.80, blue: 0.68)   // #D9CBAE
    static let accent = Color(red: 0.85, green: 0.53, blue: 0.12)
    static let accentText = Color(red: 0.18, green: 0.10, blue: 0.03)
    /// Plan C's inks: a khaki button's text and headings on the slate, and a
    /// gold button's.
    static let khakiText = Color(red: 0.97, green: 0.87, blue: 0.55)   // #F7DE8C
    static let goldText = Color(red: 0.16, green: 0.10, blue: 0.05)    // #2A1A0C

    static let radius: CGFloat = 10
    static let touch: CGFloat = 44

    /// The earlier look (RECOMP_NATIVE_SCREENS_HEAVY=1, for comparing): the
    /// scene dimmed and blurred shadows under the panels. Both are redrawn by
    /// the compositor every frame the game draws under them, so the default
    /// leaves the scene as it is and edges the panels with a plain dark line.
    static let heavy = ProcessInfo.processInfo.environment["RECOMP_NATIVE_SCREENS_HEAVY"] == "1"

    /// The players' colours in the game's order (red, blue, green, yellow,
    /// orange, teal, purple, tan).
    static let players: [Color] = [
        Color(red: 0.80, green: 0.13, blue: 0.12), Color(red: 0.16, green: 0.33, blue: 0.85),
        Color(red: 0.20, green: 0.62, blue: 0.22), Color(red: 0.90, green: 0.78, blue: 0.16),
        Color(red: 0.93, green: 0.50, blue: 0.12), Color(red: 0.12, green: 0.63, blue: 0.63),
        Color(red: 0.55, green: 0.24, blue: 0.68), Color(red: 0.72, green: 0.58, blue: 0.40),
    ]

    /// The game's own lettering (Header and Default, GameFont.swift) when it
    /// could be built from the install; else plan C's type (OFL; the app
    /// carries them in native-screens-fonts/): headings and button labels in
    /// Cormorant Garamond Bold Italic, words in Alegreya Italic. Without them the upright Cormorant (SemiBold on its
    /// axis) and Alegreya Sans the menus had, then the system's.
    static func heading(_ size: CGFloat) -> Font {
        registerFonts
        // The game's own header lettering, when it was built from the install.
        if PlatformFont(name: GameFont.bold, size: size) != nil {
            return .custom(GameFont.bold, size: size)
        }
        if PlatformFont(name: "CormorantGaramond-BoldItalic", size: size) != nil {
            return .custom("CormorantGaramond-BoldItalic", size: size)
        }
        // A variable font: its SemiBold is a weight on its axis, not a face.
        let base = PlatformFontDescriptor(name: "CormorantGaramond-Regular", size: size)
        let wght = 0x7767_6874 // 'wght'
        let semibold = base.addingAttributes([PlatformFontDescriptor.AttributeName(rawValue: kCTFontVariationAttribute as String): [wght: 600]])
        guard let font = PlatformFont.with(semibold, size: size) else { return .system(size: size, weight: .semibold, design: .serif) }
        return Self.family(font).hasPrefix("Cormorant") ? Font(font as CTFont) : .system(size: size, weight: .semibold, design: .serif)
    }
    static func body(_ size: CGFloat, weight: Font.Weight = .regular) -> Font {
        registerFonts
        let game = weight == .bold ? GameFont.bold : GameFont.regular
        if PlatformFont(name: game, size: size) != nil {
            return .custom(game, size: size)
        }
        let italic = weight == .regular ? "Alegreya-Italic" : weight == .bold ? "AlegreyaItalic-Bold"
                                                                            : "AlegreyaItalic-Medium"
        if PlatformFont(name: italic, size: size) != nil {
            return .custom(italic, size: size)
        }
        return font(weight == .regular ? "AlegreyaSans-Regular" : weight == .bold ? "AlegreyaSans-Bold"
                                                                                : "AlegreyaSans-Medium",
                    size, design: .default, weight: weight)
    }
    private static func family(_ font: PlatformFont) -> String {
        #if os(iOS)
        font.familyName
        #else
        font.familyName ?? ""
        #endif
    }

    private static func font(_ name: String, _ size: CGFloat, design: Font.Design,
                             weight: Font.Weight) -> Font {
        registerFonts
        return PlatformFont(name: name, size: size) != nil
            ? .custom(name, size: size)
            : .system(size: size, weight: weight, design: design)
    }

    /// The bundled fonts, registered for this process once.
    private static let registerFonts: Void = {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("native-screens-fonts"),
              let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        else { return }
        let fonts = files.filter { $0.pathExtension.lowercased() == "ttf" } as CFArray
        CTFontManagerRegisterFontURLs(fonts, .process, true, nil)
    }()
}

/// A panel: the game's parchment sheet round its slate (plan C), or, where
/// the install lacks it or the earlier look is asked for, dark leather with a
/// thin gold edge.
struct PanelBackground: ViewModifier {
    var fill: Color = Theme.panel
    func body(content: Content) -> some View {
        if Theme.heavy {
            flat(content)
        } else {
            content.modifier(FramedBackground(frame: Kit.sheet, fill: Kit.sheetFill,
                                              inset: EdgeInsets(top: 21, leading: 22, bottom: 20, trailing: 19),
                                              flat: fill))
        }
    }

    private func flat(_ content: Content) -> some View {
        content
            .background(
                RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                    .fill(fill)
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                            .strokeBorder(Theme.goldDim, lineWidth: 1)
                    )
                    .modifier(PanelEdge())
            )
    }
}

/// Under a panel: a dark edge, or the earlier blurred shadow (Theme.heavy).
struct PanelEdge: ViewModifier {
    func body(content: Content) -> some View {
        if Theme.heavy {
            content.shadow(color: .black.opacity(0.5), radius: 8, y: 3)
        } else {
            content.background(
                RoundedRectangle(cornerRadius: Theme.radius + 1.5, style: .continuous)
                    .strokeBorder(Color.black.opacity(0.55), lineWidth: 1.5)
                    .padding(-1.5)
            )
        }
    }
}

extension View {
    func panel(_ fill: Color = Theme.panel) -> some View { modifier(PanelBackground(fill: fill)) }
}

/// The screen's main action ("Create", "Start the game"): the game's gold
/// button.
struct PrimaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.heading(20))
            .foregroundStyle(Theme.goldText)
            .padding(.horizontal, 10)
            .frame(maxWidth: .infinity, minHeight: Theme.touch + 6)
            .opacity(enabled ? 1 : 0.6)
            .background {
                GameNine(spec: Kit.gold(pressed: configuration.isPressed)) {
                    RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                        .fill(Theme.accent.opacity(configuration.isPressed ? 0.75 : 1))
                }
                .gameDisabled(!enabled)
            }
    }
}

/// Any other button: the game's khaki button (gold when it is the chosen one).
struct PlateButtonStyle: ButtonStyle {
    var selected = false
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.heading(18))
            .foregroundStyle(selected ? Theme.goldText : Theme.khakiText)
            .padding(.horizontal, 12)
            .frame(minHeight: Theme.touch)
            // Disabled: the content dimmed (a player's flag keeps its colour),
            // the button greyed.
            .opacity(enabled ? 1 : 0.6)
            .background {
                GameNine(spec: selected ? Kit.gold(pressed: configuration.isPressed)
                                        : Kit.khaki(pressed: configuration.isPressed)) {
                    RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                        .fill(selected ? Theme.rowSelected : Theme.row)
                        .opacity(configuration.isPressed ? 0.7 : 1)
                }
                .gameDisabled(!enabled)
            }
    }
}

extension View {
    /// The game's disabled buttons are black; ours stay the button, greyed.
    func gameDisabled(_ off: Bool) -> some View {
        saturation(off ? 0.15 : 1).opacity(off ? 0.55 : 1)
    }

    /// A field's look (text input, the chosen row): the game's ComboBox.
    func gameField() -> some View {
        background {
            GameNine(spec: Kit.field) {
                RoundedRectangle(cornerRadius: Theme.radius).fill(Theme.well)
                    .overlay(RoundedRectangle(cornerRadius: Theme.radius).strokeBorder(Theme.goldDim))
            }
        }
    }
}

/// The round Back button in the top left corner, as the other phone menus have.
struct BackButton: View {
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            // The game's round "leave the screen" button.
            GameImage(ref: Kit.exit(pressed: false), placeholder: "arrow.uturn.backward")
                .frame(width: Theme.touch + 4, height: Theme.touch + 4)
        }
        .buttonStyle(PressDim())
        .accessibilityLabel("Back")
    }
}

/// A picture button pressed: a little darker.
struct PressDim: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.brightness(configuration.isPressed ? -0.15 : 0)
    }
}

/// A screen's title row: the Back button and the game's title for it.
struct ScreenHeader: View {
    let title: String
    let back: () -> Void
    var body: some View {
        HStack(spacing: 12) {
            BackButton(action: back)
            Text(title)
                .font(Theme.heading(24))
                .foregroundStyle(Theme.gold)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                // Over the game's scene (a bright sky): a dark edge to read on.
                .shadow(color: .black.opacity(0.8), radius: 2)
            Spacer(minLength: 0)
        }
    }
}

/// A map's facts, label left and value right ("Players: 4 (free for all)").
struct FactsGrid: View {
    let facts: [Fact]
    var size: CGFloat = 14
    var spacing: CGFloat = 3
    var body: some View {
        VStack(alignment: .leading, spacing: spacing) {
            ForEach(Array(facts.enumerated()), id: \.offset) { _, f in
                HStack(alignment: .firstTextBaseline) {
                    Text(f.label).foregroundStyle(Theme.textDim)
                    Spacer(minLength: 8)
                    Text(f.value).foregroundStyle(Theme.text).multilineTextAlignment(.trailing)
                }
                .font(Theme.body(size))
                // Room for the italic's last letter, which leans past its box
                // (the facts sit in a scroll view, which cuts it off).
                .padding(.trailing, 3)
            }
        }
    }
}

/// The map's picture when the game gave one (a PNG it wrote), else a plate.
struct MapPreview: View {
    let path: String?
    var body: some View {
        Group {
            if let path, !path.isEmpty, path.contains(".xdb") || path.hasSuffix(".dds") {
                // The game's own thumbnail, by its path in the paks.
                Theme.well
                    .overlay { GameImage(ref: path, placeholder: "map", fill: true) }
                    .clipped()
            } else if let path, !path.isEmpty,
               let image = path.hasSuffix(".bgra") ? RawPicture.image(atPath: path) : PlatformImage(contentsOfFile: path) {
                Image(platformImage: image).resizable().scaledToFill()
            } else {
                ZStack {
                    Theme.well
                    Image(systemName: "map").font(.system(size: 30)).foregroundStyle(Theme.goldDim)
                }
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 3, style: .continuous))
        .padding(3)
        .overlay {
            // The game's picture frame (QuestName), its middle open.
            GameNine(spec: Kit.pictureFrame) {
                RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(Theme.goldDim, lineWidth: 1)
            }
        }
    }
}

/// A switch row: its label, then the game's check box; a tap asks for the other
/// value (a plain button: a SwiftUI Toggle inside the native views' scroll
/// views did not take taps).
struct SwitchRow: View {
    let label: String
    let on: Bool
    var size: CGFloat = 16
    let toggle: (Bool) -> Void
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button { toggle(!on) } label: {
            HStack(spacing: 10) {
                Text(label)
                    .font(Theme.body(size))
                    .foregroundStyle(Theme.text)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                Spacer(minLength: 8)
                // The game's check box (9-sliced to stay sharp) and its gold tick.
                ZStack {
                    GameNine(spec: Kit.checkBox) {
                        RoundedRectangle(cornerRadius: 6).fill(Theme.row)
                            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Theme.goldDim))
                    }
                    if on {
                        GameImage(ref: Kit.checkMark, placeholder: "checkmark")
                            .frame(width: 30, height: 24)
                            .offset(x: 2, y: -2)
                    }
                }
                .frame(width: 36, height: 34)
            }
            .frame(minHeight: Theme.touch)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .opacity(enabled ? 1 : 0.45)
        .accessibilityValue(on ? "1" : "0")
    }
}
