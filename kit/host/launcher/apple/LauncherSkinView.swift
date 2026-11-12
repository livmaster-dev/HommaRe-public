// LauncherSkinView.swift - the launcher in the game's own look: the main menu's
// sky, the campaign scroll for the game list, a parchment strip for the
// selected game, the game's buttons (their Highlight texture added on top on
// hover), the green-ornament dialog for installs and the options page for
// Settings. Drawn only while a Skin is loaded (LauncherSkin.swift).
import CoreText
import SwiftUI

#if canImport(AppKit)
import AppKit
#else
import UIKit
#endif

// MARK: - Type and colour

enum SkinFont {
    /// Fonts in the app's Resources/Fonts (free fonts such as Cormorant
    /// Garamond may be bundled there) are available by name.
    private static let registered: Void = {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Fonts"),
              let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return }
        for n in names where n.hasSuffix(".ttf") || n.hasSuffix(".otf") {
            CTFontManagerRegisterFontsForURL(dir.appendingPathComponent(n) as CFURL, .process, nil)
        }
    }()

    private static func exists(_ name: String) -> Bool {
        _ = registered
        #if canImport(AppKit)
        return NSFont(name: name, size: 12) != nil
        #else
        return UIFont(name: name, size: 12) != nil
        #endif
    }

    private static func first(_ names: [(String, CGFloat)]) -> (String, CGFloat)? {
        names.first { exists($0.0) }
    }

    /// The menu's italic serif: Cormorant Garamond as in the design, else the
    /// nearest system face (scaled, as those are wider).
    private static let menuBold = first([("CormorantGaramond-BoldItalic", 1), ("Baskerville-BoldItalic", 0.84),
                                         ("Georgia-BoldItalic", 0.8)])
    private static let menuMedium = first([("CormorantGaramond-MediumItalic", 1), ("Baskerville-Italic", 0.84),
                                           ("Georgia-Italic", 0.8)])
    private static let caps = first([("CormorantGaramond-SemiBold", 1), ("Baskerville-SemiBold", 0.84),
                                     ("Georgia-Bold", 0.8)])
    private static let label = first([("PTSans-Bold", 1)])
    private static let body = first([("PTSerif-Italic", 1), ("Georgia-Italic", 0.95)])

    private static func font(_ f: (String, CGFloat)?, _ size: CGFloat, fallback: Font) -> Font {
        guard let f else { return fallback }
        return .custom(f.0, fixedSize: (size * f.1).rounded())
    }

    static func menu(_ size: CGFloat) -> Font {
        font(menuBold, size, fallback: .system(size: size * 0.82, weight: .bold, design: .serif).italic())
    }

    static func menuLight(_ size: CGFloat) -> Font {
        font(menuMedium, size, fallback: .system(size: size * 0.82, weight: .medium, design: .serif).italic())
    }

    static func smallCaps(_ size: CGFloat) -> Font {
        font(caps, size, fallback: .system(size: size * 0.85, weight: .semibold, design: .serif))
    }

    static func button(_ size: CGFloat) -> Font {
        font(label, size, fallback: .system(size: size, weight: .bold))
    }

    static func text(_ size: CGFloat) -> Font {
        font(body, size, fallback: .system(size: size, design: .serif).italic())
    }
}

enum SkinColor {
    static func rgb(_ v: UInt32) -> Color {
        Color(red: Double(v >> 16 & 0xff) / 255, green: Double(v >> 8 & 0xff) / 255, blue: Double(v & 0xff) / 255)
    }

    static let navy = rgb(0x1d2a55)
    static let navyHover = rgb(0x3b57a8)
    static let gold = rgb(0xf6c94c)
    static let goldHover = rgb(0xffe08a)
    static let goldDisabled = rgb(0x9c8045)
    static let goldShadow = rgb(0x2a1a05)
    static let brown = rgb(0x6b3d12)
    static let ink = rgb(0x3a2a17)
    static let inkSoft = rgb(0x4a3a22)
    static let inkMuted = rgb(0x5a4529)
    static let soon = rgb(0x6f5d40)
    static let cream = rgb(0xf1e3c4)
    static let legal = rgb(0xd9c7a3)
    static let night = rgb(0x1a0f07)
    static let dialogText = rgb(0xf6ead0)
    static let dialogSoft = rgb(0xeadcbc)
}

/// A brown rule that fades out at both ends.
struct SkinDivider: View {
    var body: some View {
        LinearGradient(colors: [SkinColor.brown.opacity(0), SkinColor.brown.opacity(0.55), SkinColor.brown.opacity(0)],
                       startPoint: .leading, endPoint: .trailing)
            .frame(height: 2)
            .accessibilityHidden(true)
    }
}

// MARK: - Buttons

struct SkinButtonStyle: ButtonStyle {
    enum Kind {
        /// UI/Common/Button_136x49: Play.
        case large
        /// UI/Common/DlgButtons 122x34: secondary actions.
        case small
        /// CommonBoxes/button_ok: dialog buttons.
        case dialog
    }

    var skin: Skin
    var kind: Kind

    func makeBody(configuration: Configuration) -> some View {
        SkinButton(skin: skin, kind: kind, configuration: configuration)
    }
}

private struct SkinButton: View {
    var skin: Skin
    var kind: SkinButtonStyle.Kind
    var configuration: ButtonStyleConfiguration
    @Environment(\.isEnabled) private var enabled
    @State private var hovered = false

    private var textures: (SkinTexture, SkinTexture, SkinTexture, SkinTexture) {
        switch kind {
        case .large: return (.button, .buttonHighlight, .buttonPushed, .buttonDisabled)
        case .small: return (.smallButton, .smallButtonHighlight, .smallButtonPushed, .smallButtonDisabled)
        case .dialog: return (.okButton, .okButtonHighlight, .okButtonPushed, .okButtonDisabled)
        }
    }

    /// Cap insets (vertical, horizontal), the CSS border-image slices of the design.
    private var caps: (CGFloat, CGFloat) {
        switch kind {
        case .large: return (20, 24)
        case .small: return (14, 18)
        case .dialog: return (20, 22)
        }
    }

    private var metrics: (font: CGFloat, height: CGFloat, minWidth: CGFloat) {
        switch kind {
        case .large: return (21, 58, 184)
        case .small: return (14, 38, 130)
        case .dialog: return (16, 50, 140)
        }
    }

    var body: some View {
        let (normal, highlight, pushed, disabled) = textures
        let (v, h) = caps
        let pressed = configuration.isPressed
        configuration.label
            .font(SkinFont.button(metrics.font))
            .foregroundStyle(enabled ? (hovered ? SkinColor.goldHover : SkinColor.gold) : SkinColor.goldDisabled)
            .shadow(color: SkinColor.goldShadow, radius: 0, y: 1)
            .shadow(color: .black.opacity(kind == .large ? 0.6 : 0), radius: 3)
            .lineLimit(1)
            .padding(.horizontal, h)
            .frame(minWidth: metrics.minWidth, minHeight: metrics.height, maxHeight: metrics.height)
            .background {
                ZStack {
                    skin.sliced(!enabled ? disabled : pressed ? pushed : normal, v, h, v, h)
                    if enabled && hovered && !pressed {
                        // The game adds its Highlight texture over the face.
                        skin.sliced(highlight, v, h, v, h).blendMode(.plusLighter)
                    }
                }
            }
            .offset(y: pressed ? 1 : 0)
            .contentShape(Rectangle())
            .onHover { hovered = $0 }
    }
}

/// A line of the scroll's menu, like the game's main menu entries.
struct SkinMenuLink: View {
    var title: String
    var action: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(SkinFont.menu(25))
                .foregroundStyle(hovered ? SkinColor.navyHover : SkinColor.navy)
                .shadow(color: Color(red: 1, green: 0.925, blue: 0.667).opacity(hovered ? 0.9 : 0), radius: 5)
                .padding(.horizontal, 16)
                .padding(.vertical, 4)
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}

/// Views side by side, onto the next line when the width runs out.
struct SkinFlow: Layout {
    var spacing: CGFloat
    var lineSpacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let rows = arrange(width: proposal.width ?? .infinity, subviews)
        let w = rows.map { $0.width }.max() ?? 0
        let h = rows.reduce(0) { $0 + $1.height } + lineSpacing * CGFloat(max(0, rows.count - 1))
        return CGSize(width: w, height: h)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for row in arrange(width: bounds.width, subviews) {
            var x = bounds.minX
            for i in row.items {
                let size = subviews[i].sizeThatFits(.unspecified)
                subviews[i].place(at: CGPoint(x: x, y: y + (row.height - size.height) / 2),
                                  proposal: ProposedViewSize(size))
                x += size.width + spacing
            }
            y += row.height + lineSpacing
        }
    }

    private struct Row {
        var items: [Int] = []
        var width: CGFloat = 0
        var height: CGFloat = 0
    }

    private func arrange(width: CGFloat, _ subviews: Subviews) -> [Row] {
        var rows = [Row()]
        for i in subviews.indices {
            let size = subviews[i].sizeThatFits(.unspecified)
            let add = rows[rows.count - 1].items.isEmpty ? size.width : size.width + spacing
            if !rows[rows.count - 1].items.isEmpty && rows[rows.count - 1].width + add > width {
                rows.append(Row())
            }
            var row = rows[rows.count - 1]
            row.width += row.items.isEmpty ? size.width : size.width + spacing
            row.height = max(row.height, size.height)
            row.items.append(i)
            rows[rows.count - 1] = row
        }
        return rows
    }
}

struct SkinCheck: View {
    var skin: Skin
    var title: String
    var on: Bool
    var size: CGFloat = 22
    var action: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                skin.image(on ? .checkOn : .checkOff).frame(width: 27, height: 25, alignment: .leading)
                Text(title).font(SkinFont.menu(size)).lineLimit(1).fixedSize().foregroundStyle(hovered ? SkinColor.navyHover : SkinColor.navy)
            }
            .padding(.vertical, 4)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}

/// The launcher's language: the one chosen (or "As in the system") in the
/// options' own lettering, opening a menu of the languages in their own names.
struct SkinLanguageMenu: View {
    @Bindable var model: LauncherModel
    var size: CGFloat = 22

    private var current: String {
        model.languageChoice.flatMap { code in L10n.languages.first { $0.code == code }?.name } ?? L10n.languageSystem
    }

    var body: some View {
        Menu {
            Button {
                model.setLanguage(nil)
            } label: {
                if model.languageChoice == nil { Label(L10n.languageSystem, systemImage: "checkmark") } else { Text(L10n.languageSystem) }
            }
            Divider()
            ForEach(L10n.languages) { language in
                Button {
                    model.setLanguage(language.code)
                } label: {
                    if model.languageChoice == language.code { Label(language.name, systemImage: "checkmark") } else { Text(language.name) }
                }
            }
        } label: {
            HStack(spacing: 8) {
                Text(current).font(SkinFont.menu(size)).lineLimit(1).fixedSize()
                Image(systemName: "chevron.down").font(.system(size: size * 0.55, weight: .bold))
            }
            .foregroundStyle(SkinColor.navy)
            .padding(.vertical, 4)
            .contentShape(Rectangle())
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        #if os(macOS)
        .menuIndicator(.hidden)
        #endif
        .fixedSize()
    }
}

// MARK: - Pieces

struct SkinBackdrop: View {
    var skin: Skin
    var entry: GameEntry?

    var body: some View {
        ZStack {
            SkinColor.night
            if skin[.sky] != nil {
                // The sky behind the game's 3D main menu.
                Color.clear.overlay { skin.image(.sky).resizable().aspectRatio(contentMode: .fill) }.clipped()
            } else if let entry {
                ArtView(entry: entry, alignment: .center).blur(radius: 40, opaque: true).opacity(0.8)
            }
            LinearGradient(stops: [.init(color: Color(red: 0.08, green: 0.04, blue: 0.016).opacity(0), location: 0),
                                   .init(color: Color(red: 0.08, green: 0.04, blue: 0.016).opacity(0.25), location: 0.45),
                                   .init(color: Color(red: 0.07, green: 0.035, blue: 0.012).opacity(0.92), location: 1)],
                           startPoint: .top, endPoint: .bottom)
        }
        .ignoresSafeArea()
    }
}

struct SkinSoonTag: View {
    var skin: Skin
    var label: String

    var body: some View {
        Text(label)
            .font(SkinFont.button(11))
            .tracking(0.9)
            .textCase(.uppercase)
            .foregroundStyle(SkinColor.gold)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(skin.sliced(.darkBox, 6, 6, 6, 6))
    }
}

struct SkinGameRow: View {
    var skin: Skin
    var entry: GameEntry
    var selected: Bool
    var action: () -> Void
    @State private var hovered = false

    var body: some View {
        // Not installed (or, with its files here, not playable in this app).
        let soon = entry.state == .notFound || (!entry.playableHere && !entry.isInterrupted)
        Button(action: action) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 10) {
                    Text(entry.spec.title.text)
                        .font(SkinFont.menu(27))
                        .lineLimit(2)
                        .minimumScaleFactor(0.75)
                        .fixedSize(horizontal: false, vertical: true)
                    if soon { SkinSoonTag(skin: skin, label: entry.state == .notFound ? L10n.notFound : L10n.portInTheWorks) }
                }
                Text([entry.spec.subtitle?.text, entry.spec.year].compactMap { $0 }.joined(separator: " · "))
                    .font(SkinFont.menuLight(17))
                    .foregroundStyle(SkinColor.inkSoft)
            }
            .foregroundStyle(soon ? SkinColor.soon : SkinColor.navy)
            .padding(.horizontal, 16)
            .padding(.top, 10)
            .padding(.bottom, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background {
                // Both are light the game adds onto the scroll.
                if selected {
                    // Softer than in the game, whose list sits on a darker panel.
                    skin.image(.rowSelected).resizable().blendMode(.plusLighter).opacity(0.4)
                } else if hovered {
                    skin.image(.rowHover).resizable().blendMode(.plusLighter)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// The game's splash in the dark-box frame.
struct SkinArtFrame: View {
    var skin: Skin
    var entry: GameEntry

    var body: some View {
        ArtView(entry: entry)
            .padding(8)
            .background(SkinColor.rgb(0x120a05))
            .overlay(skin.sliced(.darkBox, 8, 8, 8, 8).allowsHitTesting(false).mask(FrameMask(width: 8)))
            .shadow(color: .black.opacity(0.55), radius: 15, y: 10)
            .id(entry.id)
            .transition(.opacity)
    }
}

/// The ring a CSS border-image without `fill` draws: the frame's edges only.
private struct FrameMask: View {
    var width: CGFloat

    var body: some View {
        Rectangle().fill(.white)
            .overlay(Rectangle().fill(.black).padding(width).blendMode(.destinationOut))
            .compositingGroup()
    }
}

struct SkinStatus {
    var label: String
    var color: Color

    init(_ e: GameEntry) {
        let info = StatusInfo(e)
        label = e.canPlay ? L10n.readyToPlay : info.label
        if !e.playableHere && !e.isInterrupted && e.state != .notFound {
            label = L10n.portInTheWorks
            color = SkinColor.rgb(0x7a4a12)
            return
        }
        switch e.state {
        case .ready: color = SkinColor.rgb(0x2f5a1c)
        case .notFound: color = SkinColor.inkMuted
        case .interrupted: color = SkinColor.brown
        case .wrongVersion, .incomplete: color = SkinColor.rgb(0x8a2e10)
        }
    }
}

/// The selected game on its parchment strip, with Play and the folder actions.
struct SkinDetails: View {
    var skin: Skin
    @Bindable var model: LauncherModel
    var entry: GameEntry
    @Binding var importerShown: Bool
    /// A column on an upright screen: the title on two lines where it needs
    /// them, the status over the buttons, which stack where they do not fit.
    var compact = false

    var body: some View {
        let info = StatusInfo(entry)
        let status = SkinStatus(entry)
        VStack(alignment: .leading, spacing: 8) {
            Text(meta)
                .font(SkinFont.button(12))
                .tracking(1.7)
                .textCase(.uppercase)
                .foregroundStyle(SkinColor.brown)
            Text(entry.spec.title.text)
                .font(SkinFont.menu(38))
                .foregroundStyle(SkinColor.navy)
                .lineLimit(compact ? 2 : 1)
                .minimumScaleFactor(0.6)
                .fixedSize(horizontal: false, vertical: compact)
            if let blurb = entry.spec.blurb?.text, !blurb.isEmpty {
                Text(blurb)
                    .font(SkinFont.text(15))
                    .foregroundStyle(SkinColor.ink)
                    .lineSpacing(3)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: 690, alignment: .leading)
            }
            if !entry.canPlay, !info.detail.isEmpty {
                Text(info.detail)
                    .font(SkinFont.text(13))
                    .foregroundStyle(SkinColor.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: 690, alignment: .leading)
            }
            Spacer(minLength: 6)
            if compact {
                VStack(alignment: .leading, spacing: 2) { statusLines(status) }
                    .padding(.bottom, 6)
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .center, spacing: 12) { primary; secondary }
                    VStack(alignment: .leading, spacing: 10) { primary; secondary }
                }
            } else {
                HStack(alignment: .center, spacing: 12) {
                    primary
                    secondary
                    Spacer(minLength: 8)
                    VStack(alignment: .trailing, spacing: 2) { statusLines(status) }
                }
            }
        }
        .padding(.horizontal, compact ? 30 : 40)
        .padding(.vertical, compact ? 28 : 32)
        .frame(maxWidth: .infinity, minHeight: compact ? 0 : 264, alignment: .topLeading)
        .background(skin.sliced(.parchment, 22, 22, 22, 22))
        .shadow(color: .black.opacity(0.5), radius: 14, y: 10)
    }

    @ViewBuilder private func statusLines(_ status: SkinStatus) -> some View {
        Text(status.label)
            .font(SkinFont.button(13))
            .foregroundStyle(status.color)
        if entry.canPlay, let root = entry.root {
            path(L10n.location(root))
        }
    }

    private var meta: String {
        var parts = [entry.spec.subtitle?.text, entry.spec.year].compactMap { $0 }
        if let v = entry.spec.version { parts.append("\(L10n.version) \(v)") }
        return parts.joined(separator: "  ·  ")
    }

    @ViewBuilder private func path(_ text: String) -> some View {
        let label = Text(text)
            .font(.system(size: 12, design: .monospaced))
            .foregroundStyle(SkinColor.inkMuted)
            .lineLimit(1)
            .truncationMode(.middle)
        #if os(macOS)
        Button { model.revealSelected() } label: { label }
            .buttonStyle(.plain)
            .help(L10n.showInFinder)
        #else
        label
        #endif
    }

    @ViewBuilder private var primary: some View {
        let style = SkinButtonStyle(skin: skin, kind: .large)
        if PrimaryButton.installs(entry) {
            install(style)
        } else if entry.isInterrupted {
            Button(L10n.resumeInstall) { model.resumeInstall() }
                .buttonStyle(style)
                .keyboardShortcut(.defaultAction)
        } else {
            play(style)
        }
    }

    /// The game is not here (or not whole): Install is the main button.
    private func install(_ style: SkinButtonStyle) -> some View {
        Button(L10n.installFromGog) { model.showGog() }
        .buttonStyle(style)
        .keyboardShortcut(.defaultAction)
    }

    private func play(_ style: SkinButtonStyle) -> some View {
        Button(entry.playableHere ? L10n.play : L10n.portInTheWorks) { model.play() }
            .buttonStyle(style)
            .disabled(!entry.canPlay)
            .keyboardShortcut(.defaultAction)
    }

    @ViewBuilder private var secondary: some View {
        let style = SkinButtonStyle(skin: skin, kind: .small)
        #if os(macOS)
        Button(L10n.chooseFolder) { model.pickFolder() }
            .buttonStyle(style)
            .keyboardShortcut("o", modifiers: .command)
        if model.profileDir != nil {
            Button(L10n.saves) { model.revealSaves() }.buttonStyle(style)
        }
        #endif
        // iOS: the files of any game come in through the menu's Add game
        // files and, for this one when it is not here, through Install.
    }
}

/// The campaign scroll: the games, then the menu.
struct SkinGameScroll: View {
    var skin: Skin
    @Bindable var model: LauncherModel
    @Binding var importerShown: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(L10n.chooseGame)
                .font(SkinFont.smallCaps(15))
                .tracking(3)
                .textCase(.uppercase)
                .foregroundStyle(SkinColor.brown)
                .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 10)
            ForEach(model.entries) { e in
                SkinGameRow(skin: skin, entry: e, selected: e.id == model.selectedID) {
                    withAnimation(.easeOut(duration: 0.25)) { model.selectedID = e.id }
                }
            }
            Spacer(minLength: 0)
            SkinDivider().padding(.horizontal, 16).padding(.vertical, 10)
            #if os(macOS)
            SkinMenuLink(title: L10n.installFromGog) { model.showGog() }
            SkinMenuLink(title: L10n.settings) { withAnimation(.easeOut(duration: 0.2)) { model.settingsShown = true } }
            #else
            // A finger wants a button, not a line of the menu.
            VStack(spacing: 12) {
                Button(L10n.importFolder) { importerShown = true }
                    .buttonStyle(SkinButtonStyle(skin: skin, kind: .large))
                Button(L10n.settings) { withAnimation(.easeOut(duration: 0.2)) { model.settingsShown = true } }
                    .buttonStyle(SkinButtonStyle(skin: skin, kind: .large))
            }
            .frame(maxWidth: .infinity)
            .padding(.horizontal, 12)
            #endif
            #if os(macOS)
            SkinMenuLink(title: L10n.quit) { model.quit() }
            #endif
        }
        .padding(.top, 40)
        .padding(.horizontal, 28)
        .padding(.bottom, 50)
        .background(skin.sliced(.scroll, 34, 14, 46, 14))
        .shadow(color: .black.opacity(0.55), radius: 16, y: 12)
    }
}

// MARK: - Screens

struct SkinWideLayout: View {
    var skin: Skin
    @Bindable var model: LauncherModel
    @Binding var importerShown: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 36) {
            VStack(alignment: .leading, spacing: 0) {
                skin.image(.logo)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: 384, maxHeight: 101, alignment: .leading)
                    .shadow(color: .black.opacity(0.6), radius: 8, y: 4)
                    .accessibilityLabel(model.title)
                    .padding(.bottom, 21)
                if let e = model.selected {
                    SkinArtFrame(skin: skin, entry: e)
                        .frame(minHeight: 120, maxHeight: .infinity)
                        .padding(.horizontal, 8)
                        .padding(.bottom, 14)
                    SkinDetails(skin: skin, model: model, entry: e, importerShown: $importerShown)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    Spacer()
                }
                footer.padding(.top, 14).padding(.horizontal, 8)
            }
            .frame(maxWidth: 900)
            SkinGameScroll(skin: skin, model: model, importerShown: $importerShown)
                .frame(width: 392)
                .padding(.top, 6)
        }
        .padding(.leading, 40)
        .padding(.trailing, 36)
        .padding(.top, 28)
        .padding(.bottom, 16)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .animation(.easeOut(duration: 0.3), value: model.selectedID)
    }

    private var footer: some View {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let tagline = [model.config.platformTagline?.text, version].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")
        return HStack(alignment: .lastTextBaseline, spacing: 24) {
            Text(tagline)
                .font(SkinFont.menu(19))
                .foregroundStyle(SkinColor.cream)
                .lineLimit(1)
                .layoutPriority(1)
            if model.scanning {
                // Only while a search runs: it finishes in a second or two.
                Text(L10n.searching)
                    .font(SkinFont.text(13))
                    .foregroundStyle(SkinColor.legal)
                    .lineLimit(1)
                    .layoutPriority(-1)
                    .transition(.opacity)
            }
            Spacer(minLength: 0)
            Text(model.config.legal?.text ?? L10n.defaultLegal)
                .font(SkinFont.button(11))
                .foregroundStyle(SkinColor.legal)
                .multilineTextAlignment(.trailing)
                .lineSpacing(1)
                .frame(minWidth: 220, maxWidth: 420, alignment: .trailing)
                .fixedSize(horizontal: false, vertical: true)
        }
        .shadow(color: .black, radius: 1, y: 1)
    }
}

/// An iPhone or iPad held upright: the same pieces in one column that
/// scrolls - the selected game's splash (swiped sideways for the next game),
/// its parchment strip with Play, then the campaign scroll with the games,
/// Add game files and Settings.
struct SkinTallLayout: View {
    var skin: Skin
    @Bindable var model: LauncherModel
    @Binding var importerShown: Bool
    var size: CGSize

    /// Laid out at no less than 500 pt wide and scaled down to fit, as the
    /// sideways layout is; an iPad's at its own size, no wider than 720.
    static func scale(_ width: CGFloat) -> CGFloat {
        min(1, width / 500)
    }

    var body: some View {
        let scale = Self.scale(size.width)
        let width = min(720, size.width / scale)
        ScrollView {
            ScaledColumn(scale: scale) {
                column(width: width - 40)
                    .padding(.horizontal, 20)
                    .padding(.top, 10)
                    .padding(.bottom, 24)
                    .frame(width: width)
                    .scaleEffect(scale, anchor: .topLeading)
            }
            .frame(maxWidth: .infinity)
        }
        .scrollIndicators(.never)
        .animation(.easeOut(duration: 0.3), value: model.selectedID)
    }

    private func column(width: CGFloat) -> some View {
        VStack(spacing: 0) {
            skin.image(.logo)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(maxWidth: 384, maxHeight: 101)
                .shadow(color: .black.opacity(0.6), radius: 8, y: 4)
                .accessibilityLabel(model.title)
                .padding(.bottom, 16)
            if let e = model.selected {
                SkinArtFrame(skin: skin, entry: e)
                    .frame(height: (width * 0.52).rounded())
                    .padding(.horizontal, 6)
                    .contentShape(Rectangle())
                    // Sideways swipes only: the column itself scrolls up and down.
                    .simultaneousGesture(DragGesture(minimumDistance: 24).onEnded { v in
                        let dx = v.translation.width
                        guard abs(dx) > 60, abs(dx) > abs(v.translation.height) * 1.5 else { return }
                        step(dx < 0 ? 1 : -1)
                    })
                dots.padding(.vertical, 12)
                SkinDetails(skin: skin, model: model, entry: e, importerShown: $importerShown, compact: true)
            }
            SkinGameScroll(skin: skin, model: model, importerShown: $importerShown)
                .frame(maxWidth: 440)
                .padding(.top, 26)
            footer.padding(.top, 22).padding(.horizontal, 8)
        }
        .frame(width: width)
    }

    /// Which of the games the splash shows; a tap picks one.
    @ViewBuilder private var dots: some View {
        if model.entries.count > 1 {
            HStack(spacing: 4) {
                ForEach(model.entries) { e in
                    let on = e.id == model.selectedID
                    Button {
                        withAnimation(.easeOut(duration: 0.25)) { model.selectedID = e.id }
                    } label: {
                        Circle()
                            .fill(on ? SkinColor.gold : SkinColor.cream.opacity(0.4))
                            .frame(width: on ? 10 : 8, height: on ? 10 : 8)
                            .shadow(color: .black.opacity(0.7), radius: 2, y: 1)
                            .frame(width: 26, height: 22)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(e.spec.title.text)
                    .accessibilityAddTraits(on ? .isSelected : [])
                }
            }
        } else {
            Color.clear.frame(height: 4)
        }
    }

    private var footer: some View {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let tagline = [model.config.platformTagline?.text, version].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")
        return VStack(spacing: 10) {
            if !tagline.isEmpty {
                Text(tagline)
                    .font(SkinFont.menu(19))
                    .foregroundStyle(SkinColor.cream)
            }
            if model.scanning {
                Text(L10n.searching)
                    .font(SkinFont.text(13))
                    .foregroundStyle(SkinColor.legal)
                    .transition(.opacity)
            }
            Text(model.config.legal?.text ?? L10n.defaultLegal)
                .font(SkinFont.button(11))
                .foregroundStyle(SkinColor.legal)
                .lineSpacing(1)
                .fixedSize(horizontal: false, vertical: true)
        }
        .multilineTextAlignment(.center)
        .shadow(color: .black, radius: 1, y: 1)
    }

    private func step(_ d: Int) {
        guard let i = model.entries.firstIndex(where: { $0.id == model.selectedID }) else { return }
        let n = model.entries.count
        withAnimation(.easeOut(duration: 0.25)) { model.selectedID = model.entries[(i + d + n) % n].id }
    }
}

/// Lays its one view out `1 / scale` times as large as offered and takes the
/// room it fills once scaled (the view draws itself scaled from its top
/// leading corner): unlike scaleEffect alone, the scaled size is what a
/// ScrollView around it sees.
private struct ScaledColumn: Layout {
    var scale: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        guard let view = subviews.first else { return .zero }
        let size = view.sizeThatFits(ProposedViewSize(width: proposal.width.map { $0 / scale },
                                                      height: proposal.height.map { $0 / scale }))
        return CGSize(width: size.width * scale, height: size.height * scale)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        subviews.first?.place(at: bounds.origin, anchor: .topLeading,
                              proposal: ProposedViewSize(width: bounds.width / scale, height: bounds.height / scale))
    }
}

/// Copying or installing, in the game's ornate dialog.
struct SkinProgressDialog: View {
    var skin: Skin
    var progress: ImportProgress
    var cancel: () -> Void

    var body: some View {
        ZStack(alignment: .topLeading) {
            skin.image(.dialog).resizable()
            Text(progress.phase == .copying ? L10n.importTitle : L10n.installTitle)
                .font(SkinFont.menu(28))
                .foregroundStyle(SkinColor.navy)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
                .frame(width: 518, height: 48)
                .offset(x: 56, y: 50)
            HStack(spacing: 14) {
                if skin[.messageIcon] != nil {
                    skin.image(.messageIcon)
                        .resizable()
                        .frame(width: 104, height: 107)
                        .shadow(color: .black.opacity(0.5), radius: 3, y: 3)
                        .accessibilityHidden(true)
                }
                VStack(alignment: .leading, spacing: 8) {
                    Text(phase)
                        .font(SkinFont.menu(22))
                        .foregroundStyle(SkinColor.dialogText)
                        .lineLimit(1)
                    Text(progress.current.isEmpty ? " " : progress.current)
                        .font(SkinFont.text(13))
                        .foregroundStyle(SkinColor.dialogSoft)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    SkinTrack(skin: skin, fraction: progress.fraction)
                    HStack {
                        Text(L10n.fileOf(progress.filesDone, progress.filesTotal) + " · "
                             + L10n.progressOf(progress.bytesDone, progress.bytesTotal))
                        Spacer()
                        Text("\(Int((progress.fraction * 100).rounded()))%")
                    }
                    .font(SkinFont.button(12).monospacedDigit())
                    .foregroundStyle(SkinColor.gold)
                }
            }
            .padding(.leading, 10)
            .padding(.trailing, 20)
            .frame(width: 510, height: 158)
            .offset(x: 60, y: 104)
            Button(L10n.cancel, action: cancel)
                .buttonStyle(SkinButtonStyle(skin: skin, kind: .dialog))
                .keyboardShortcut(.cancelAction)
                .frame(width: 518)
                .offset(x: 56, y: 280)
        }
        .frame(width: 630, height: 394)
        .shadow(color: .black.opacity(0.6), radius: 15, y: 18)
    }

    private var phase: String {
        switch progress.phase {
        case .copying: return L10n.phaseCopying
        case .checking: return L10n.phaseChecking
        case .extracting: return L10n.phaseUnpacking
        case .verifying: return L10n.phaseVerifying
        case .patching: return L10n.phasePatching
        case .downloading: return L10n.phaseDownloading
        }
    }
}

/// The dialog with the installer's or folder's name under it.
struct SkinProgressScreen: View {
    var skin: Skin
    var progress: ImportProgress
    var cancel: () -> Void

    var body: some View {
        VStack(spacing: 20) {
            SkinProgressDialog(skin: skin, progress: progress, cancel: cancel)
            Text(progress.name)
                .font(SkinFont.menuLight(19))
                .foregroundStyle(SkinColor.cream)
                .shadow(color: .black, radius: 1, y: 1)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 900)
        }
    }
}

/// The options screen's scroll bar track, filled with gold up to `fraction`.
struct SkinTrack: View {
    var skin: Skin
    var fraction: Double

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                skin.sliced(.track, 10, 12, 10, 12)
                RoundedRectangle(cornerRadius: 3)
                    .fill(LinearGradient(colors: [SkinColor.rgb(0xffe08a), SkinColor.rgb(0xe7a93a), SkinColor.rgb(0x9b5d14)],
                                         startPoint: .top, endPoint: .bottom))
                    .shadow(color: SkinColor.rgb(0xffc85a).opacity(0.55), radius: 4)
                    .frame(width: max(8, (geo.size.width - 12) * min(1, max(0, fraction))))
                    .padding(.vertical, 7)
                    .padding(.leading, 6)
                    .animation(.easeOut(duration: 0.25), value: fraction)
            }
        }
        .frame(height: 24)
    }
}

/// The launcher's settings on the game's options page.
struct SkinSettings: View {
    var skin: Skin
    @Bindable var model: LauncherModel
    /// A phone held upright: one column rather than two.
    var tall = false
    /// A phone-sized window (SkinSettings.phone(for:)): the phone's layout.
    var phone = false

    var body: some View {
        #if os(iOS)
        if phone {
            phoneBody
        } else {
            wideBody
        }
        #else
        wideBody
        #endif
    }

    #if os(iOS)
    /// A phone held sideways: the dialog at its full size over most of the
    /// screen, the settings in two columns, scrolled when they do not fit.
    /// Upright, the columns one under the other.
    @ViewBuilder private var phoneBody: some View {
        let columns = tall ? AnyLayout(VStackLayout(alignment: .leading, spacing: 14))
                           : AnyLayout(HStackLayout(alignment: .top, spacing: 28))
        VStack(alignment: .leading, spacing: 6) {
            Text(L10n.settings)
                .font(SkinFont.menu(30))
                .foregroundStyle(SkinColor.navy)
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    cell(L10n.languageLabel) {
                        SkinLanguageMenu(model: model, size: 20)
                    }
                    SkinDivider()
                    columns {
                        VStack(alignment: .leading, spacing: 14) {
                            cell(L10n.frameLimit) {
                                choices([L10n.frameLimitGame, "40", "60", "120"], model.frameLimit, size: 20, model.setFrameLimit)
                            }
                            cell(L10n.energySaver) {
                                choices([L10n.auto, L10n.always, L10n.never], model.energySaver, size: 20, model.setEnergySaver)
                            }
                            cell(L10n.renderQuality) {
                                choices(SkinSettings.renderChoices, model.renderQuality, size: 20, model.setRenderQuality)
                            }
                            cell(L10n.idleFrames) {
                                choices([L10n.auto, L10n.on, L10n.off], model.idleFrames, size: 20, model.setIdleFrames)
                            }
                            cell(L10n.startupVideos) {
                                choices([L10n.showVideos, L10n.skipVideos], model.skipIntro ? 1 : 0, size: 20) { model.setSkipIntro($0 == 1) }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        VStack(alignment: .leading, spacing: 14) {
                            cell(L10n.cameraButtons) {
                                choices([L10n.showVideos, L10n.hideButtons], model.cameraButtons ? 0 : 1, size: 20) { model.setCameraButtons($0 == 0) }
                            }
                            cell(L10n.commandPanel) {
                                choices([L10n.showVideos, L10n.hideButtons], model.commandPanel ? 0 : 1, size: 20) { model.setCommandPanel($0 == 0) }
                            }
                            cell(L10n.battleCells) {
                                choices([L10n.off, L10n.on], model.battleCells ? 1 : 0, size: 20) { model.setBattleCells($0 == 1) }
                            }
                            cell(L10n.touchMode) {
                                choices([L10n.touchCursor, L10n.touchNative], model.nativeTouch ? 1 : 0, size: 20) { model.setNativeTouch($0 == 1) }
                            }
                            cell(L10n.silentMode) {
                                choices([L10n.respect, L10n.ignore], model.ignoreSilentMode ? 1 : 0, size: 20) { model.setIgnoreSilentMode($0 == 1) }
                            }
                            cell(L10n.phoneInterface) {
                                choices([L10n.auto, L10n.always, L10n.never], Self.phoneChoice(model.phoneInterface), size: 20) {
                                    model.setPhoneInterface(Self.phoneSetting($0))
                                }
                            }
                            // The game's menus drawn natively, from the game's next start.
                            cell(L10n.menus) {
                                choices([L10n.menusNative, L10n.menusGame], model.nativeMenus ? 0 : 1, size: 20) {
                                    model.setNativeMenus($0 == 0)
                                }
                            }
                            cell(L10n.metalHud) {
                                SkinCheck(skin: skin, title: L10n.metalHudShow, on: model.metalHud, size: 20) { model.setMetalHud(!model.metalHud) }
                                if model.metalHudPending {
                                    Text(L10n.nextStart).font(SkinFont.text(15)).foregroundStyle(SkinColor.inkMuted)
                                }
                            }
                            cell(L10n.gpuSkin) {
                                SkinCheck(skin: skin, title: L10n.gpuSkinOn, on: model.gpuSkin, size: 20) { model.setGpuSkin(!model.gpuSkin) }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    SkinDivider()
                    VStack(alignment: .leading, spacing: 4) {
                        label(L10n.gameFolders)
                        let roots = model.rememberedRoots
                        if roots.isEmpty {
                            Text(L10n.noFolders).font(SkinFont.text(15)).foregroundStyle(SkinColor.inkMuted)
                        }
                        ForEach(roots.prefix(4), id: \.path) { mono(L10n.location($0)) }
                        if let saves = model.profileDir {
                            label(L10n.savesFolder).padding(.top, 6)
                            mono(L10n.location(saves))
                        }
                        label(L10n.cloudSaves).padding(.top, 6)
                        CloudSavesRow(model: model, skin: skin, size: 20)
                    }
                }
                .padding(.vertical, 4)
            }
            HStack {
                Spacer()
                Button(L10n.done) { withAnimation(.easeOut(duration: 0.2)) { model.settingsShown = false } }
                    .buttonStyle(SkinButtonStyle(skin: skin, kind: .dialog))
                Spacer()
            }
        }
        .padding(.top, tall ? 74 : 30)
        .padding(.leading, tall ? 56 : 44)
        .padding(.trailing, tall ? 58 : 48)
        .padding(.bottom, tall ? 50 : 26)
        .background {
            if tall {
                // Twice as tall as wide: its torn edges and the curl at the
                // top right kept at their size, the parchment stretched.
                skin.sliced(.page, 64, 50, 50, 100)
            } else {
                skin.image(.page).resizable()
            }
        }
        .shadow(color: .black.opacity(0.6), radius: 15, y: 18)
    }

    /// A setting on a phone: its name over its choices, which wrap onto a
    /// second line where the column is too narrow for them.
    private func cell<Choices: View>(_ title: String, @ViewBuilder _ choices: () -> Choices) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            label(title)
            SkinFlow(spacing: 14, lineSpacing: 2) { choices() }
        }
    }
    #endif

    private var wideBody: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(L10n.settings)
                .font(SkinFont.menu(40))
                .foregroundStyle(SkinColor.navy)
                .padding(.bottom, 4)
            // Scrolled where the rows outgrow the page (an opened foldable iPhone):
            // fixed in place they ran over the page's top and bottom edges and
            // pushed Done under it.
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    row(L10n.languageLabel) {
                        SkinLanguageMenu(model: model)
                    }
                    SkinDivider().padding(.vertical, 6)
                    // The game's pacing (mods/settings_file.h): read when it starts.
                    row(L10n.frameLimit) {
                        choices([L10n.frameLimitGame, "40", "60", "120"], model.frameLimit, model.setFrameLimit)
                    }
                    row(L10n.energySaver) {
                        choices([L10n.auto, L10n.always, L10n.never], model.energySaver, model.setEnergySaver)
                    }
                    row(L10n.idleFrames) {
                        choices([L10n.auto, L10n.on, L10n.off], model.idleFrames, model.setIdleFrames)
                    }
                    // On a Mac a choice under the native size draws smaller and MetalFX
                    // upscales it (d3d9_metal.mm wanted_scale); automatic supersamples.
                    row(L10n.renderQuality) {
                        choices(SkinSettings.renderChoices, model.renderQuality, model.setRenderQuality)
                    }
                    row(L10n.uiBounds) {
                        choices([L10n.uiBoundsScreen, "21:9", "16:9", "4:3"], model.uiBounds, model.setUiBounds)
                    }
                    row(L10n.startupVideos) {
                        choices([L10n.showVideos, L10n.skipVideos], model.skipIntro ? 1 : 0) { model.setSkipIntro($0 == 1) }
                    }
                    // Apple's Metal HUD over the game, from the next start (sdl/main.cpp).
                    row(L10n.metalHud) {
                        SkinCheck(skin: skin, title: L10n.metalHudShow, on: model.metalHud) { model.setMetalHud(!model.metalHud) }
                        if model.metalHudPending {
                            Text(L10n.nextStart).font(SkinFont.text(14)).foregroundStyle(SkinColor.inkMuted)
                        }
                    }
                    // Animated models on the GPU (native/gpu_skin.cpp), read when the game starts.
                    row(L10n.gpuSkin) {
                        SkinCheck(skin: skin, title: L10n.gpuSkinOn, on: model.gpuSkin) { model.setGpuSkin(!model.gpuSkin) }
                    }
                    #if os(iOS)
                    row(L10n.cameraButtons) {
                        choices([L10n.showVideos, L10n.hideButtons], model.cameraButtons ? 0 : 1) { model.setCameraButtons($0 == 0) }
                    }
                    row(L10n.commandPanel) {
                        choices([L10n.showVideos, L10n.hideButtons], model.commandPanel ? 0 : 1) { model.setCommandPanel($0 == 0) }
                    }
                    row(L10n.battleCells) {
                        choices([L10n.off, L10n.on], model.battleCells ? 1 : 0) { model.setBattleCells($0 == 1) }
                    }
                    row(L10n.touchMode) {
                        choices([L10n.touchCursor, L10n.touchNative], model.nativeTouch ? 1 : 0) { model.setNativeTouch($0 == 1) }
                    }
                    row(L10n.silentMode) {
                        choices([L10n.respect, L10n.ignore], model.ignoreSilentMode ? 1 : 0) { model.setIgnoreSilentMode($0 == 1) }
                    }
                    if SkinSettings.phoneDevice {
                        row(L10n.phoneInterface) {
                            choices([L10n.auto, L10n.always, L10n.never], Self.phoneChoice(model.phoneInterface)) {
                                model.setPhoneInterface(Self.phoneSetting($0))
                            }
                        }
                    }
                    // The game's menus drawn natively, from the game's next start.
                    row(L10n.menus) {
                        choices([L10n.menusNative, L10n.menusGame], model.nativeMenus ? 0 : 1) { model.setNativeMenus($0 == 0) }
                    }
                    #else
                    // Two fingers on a Mac trackpad over the 3D map (sdl/main.cpp install_trackpad).
                    row(L10n.trackpad) {
                        choices([L10n.trackpadHeight, L10n.trackpadTablet, L10n.trackpadGame],
                                model.trackpadCamera, model.setTrackpadCamera)
                    }
                    #endif
                    SkinDivider().padding(.vertical, 6)
                    HStack(alignment: .top, spacing: 14) {
                        VStack(alignment: .leading, spacing: 4) {
                            label(L10n.gameFolders)
                            let roots = model.rememberedRoots
                            if roots.isEmpty {
                                Text(L10n.noFolders).font(SkinFont.text(14)).foregroundStyle(SkinColor.inkMuted)
                            }
                            ForEach(roots.prefix(4), id: \.path) { mono(L10n.location($0)) }
                            if let base = LauncherModel.importBase() {
                                Text("\(L10n.installsGoTo): \(L10n.location(base))")
                                    .font(SkinFont.text(13))
                                    .foregroundStyle(SkinColor.inkMuted)
                                    .lineLimit(2)
                                    .truncationMode(.middle)
                                    .padding(.top, 2)
                            }
                        }
                        Spacer(minLength: 0)
                        #if os(macOS)
                        Button(L10n.addFolder) { model.pickFolder() }
                            .buttonStyle(SkinButtonStyle(skin: skin, kind: .small))
                        #endif
                    }
                    if let saves = model.profileDir {
                        SkinDivider().padding(.vertical, 6)
                        HStack(alignment: .center, spacing: 14) {
                            VStack(alignment: .leading, spacing: 2) {
                                label(L10n.savesFolder)
                                mono(L10n.location(saves))
                            }
                            Spacer(minLength: 0)
                            #if os(macOS)
                            Button(L10n.show) { model.revealSaves() }
                                .buttonStyle(SkinButtonStyle(skin: skin, kind: .small))
                            #endif
                        }
                    }
                    // The saves in step with iCloud Drive (LauncherCloudSaves.swift).
                    HStack(alignment: .firstTextBaseline, spacing: 16) {
                        label(L10n.cloudSaves).frame(width: 220, alignment: .leading)
                        CloudSavesRow(model: model, skin: skin)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .scrollBounceBehavior(.basedOnSize)
            Spacer(minLength: 0)
            HStack {
                Spacer()
                Button(L10n.done) { withAnimation(.easeOut(duration: 0.2)) { model.settingsShown = false } }
                    .buttonStyle(SkinButtonStyle(skin: skin, kind: .dialog))
                    .keyboardShortcut(.cancelAction)
                Spacer()
            }
        }
        .padding(.top, 64)
        .padding(.leading, 70)
        .padding(.trailing, 78)
        .padding(.bottom, 60)
        .frame(width: SkinSettings.size.width, height: SkinSettings.size.height)
        .background(skin.image(.page).resizable())
        .shadow(color: .black.opacity(0.6), radius: 15, y: 18)
    }

    #if os(iOS)
    /// An iPhone: the phone interface's row.
    static let phoneDevice = UIDevice.current.userInterfaceIdiom == .phone
    // Wider than the Mac's: the render quality's five choices are one row (about 930
    // points with its name), which at 860 pushed the whole column past the page's edges.
    static let size = CGSize(width: 1080, height: 990) // with the touch rows, render quality and the interface bounds
    #else
    static let size = CGSize(width: 860, height: 1020) // with the interface bounds and trackpad rows, render quality on two lines
    static let phoneDevice = false
    #endif

    /// A phone-sized window on a phone, as the game's host decides it
    /// (sdl/main.cpp note_phone_form): the shorter side under 600 points. A
    /// foldable iPhone's opened screen is a tablet's.
    static func phone(for size: CGSize) -> Bool {
        phoneDevice && min(size.width, size.height) < 600
    }

    /// host.ui/phone_interface (0 never, 1 by the screen, 2 always) and the
    /// row's choices (auto, always, never).
    static func phoneChoice(_ setting: Int) -> Int {
        setting == 2 ? 1 : setting == 0 ? 2 : 0
    }
    static func phoneSetting(_ choice: Int) -> Int {
        choice == 1 ? 2 : choice == 2 ? 0 : 1
    }

    /// host.display/render_quality's choices, in its order (0 automatic ..
    /// 4 performance).
    static var renderChoices: [String] {
        [L10n.auto, L10n.renderNative, L10n.renderQualityChoice, L10n.renderBalanced, L10n.renderPerformance]
    }

    private func label(_ text: String) -> some View {
        Text(text).font(SkinFont.menu(22)).foregroundStyle(SkinColor.navy)
    }

    /// A setting: its name, then its choices side by side, wrapping onto a
    /// second line where a language's words are too long for the page.
    private func row<Choices: View>(_ title: String, @ViewBuilder _ choices: () -> Choices) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 16) {
            label(title).frame(width: 220, alignment: .leading)
            SkinFlow(spacing: 22, lineSpacing: 4) { choices() }
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func choices(_ titles: [String], _ chosen: Int, size: CGFloat = 22,
                         _ choose: @escaping (Int) -> Void) -> some View {
        ForEach(titles.indices, id: \.self) { i in
            SkinCheck(skin: skin, title: titles[i], on: chosen == i, size: size) { choose(i) }
        }
    }

    private func mono(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 13, design: .monospaced))
            .foregroundStyle(SkinColor.rgb(0x4a3518))
            .lineLimit(1)
            .truncationMode(.middle)
    }
}

/// A dialog laid over the launcher, scaled down when the window is small.
struct SkinModal<Content: View>: View {
    var size: CGSize
    var natural: CGSize
    @ViewBuilder var content: Content

    var body: some View {
        let scale = min(1, (size.width - 32) / natural.width, (size.height - 32) / natural.height)
        ZStack {
            Color(red: 0.063, green: 0.031, blue: 0.012).opacity(0.62).ignoresSafeArea()
            // Scaled in its own natural box, then laid out at the scaled size:
            // left at its natural size it would stretch the launcher's stack past
            // a small screen and pull everything off centre.
            content
                .frame(width: natural.width, height: natural.height)
                .scaleEffect(scale)
                .frame(width: natural.width * scale, height: natural.height * scale)
        }
        .frame(width: size.width, height: size.height)
        .transition(.opacity)
    }
}
