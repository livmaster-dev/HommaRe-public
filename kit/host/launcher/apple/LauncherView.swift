// LauncherView.swift - the native launcher's screens. One SwiftUI tree for the
// Mac, iPad and iPhone: a game list beside a large card where there is room,
// a swipeable row of cards on a narrow screen.
import ImageIO
import SwiftUI
#if os(iOS)
import UniformTypeIdentifiers
#endif

// MARK: - Look

enum Theme {
    static let ink = Color(red: 0.035, green: 0.04, blue: 0.055)
    static let gold = Color(red: 0.89, green: 0.73, blue: 0.43)
    static let goldDeep = Color(red: 0.70, green: 0.50, blue: 0.22)
    static let text = Color(white: 0.96)
    static let dim = Color(white: 0.70)
    static let faint = Color(white: 0.46)
    static let panel = Color.white.opacity(0.06)
    static let stroke = Color.white.opacity(0.12)

    static func accent(_ hex: String?) -> Color {
        guard let hex, hex.count == 7, hex.hasPrefix("#"), let v = UInt32(hex.dropFirst(), radix: 16) else { return gold }
        return Color(red: Double((v >> 16) & 0xff) / 255, green: Double((v >> 8) & 0xff) / 255,
                     blue: Double(v & 0xff) / 255)
    }
}

extension Image {
    init(platform: PlatformImage) {
        #if canImport(AppKit)
        self.init(nsImage: platform)
        #else
        self.init(uiImage: platform)
        #endif
    }
}

/// The player's own splash art, cropped from the top (the bottom of every
/// splash carries rating badges and legal lines); a painted placeholder when
/// the game is not on this device.
struct ArtView: View {
    var entry: GameEntry
    var alignment: Alignment = .top

    var body: some View {
        GeometryReader { geo in
            ZStack {
                if let art = entry.art {
                    Image(platform: art)
                        .resizable()
                        .interpolation(.high)
                        .aspectRatio(contentMode: .fill)
                        .frame(width: geo.size.width, height: geo.size.height, alignment: alignment)
                        .clipped()
                } else {
                    Placeholder(accent: Theme.accent(entry.spec.accent), title: entry.spec.title.text)
                }
            }
        }
    }
}

struct Placeholder: View {
    var accent: Color
    var title: String

    var body: some View {
        ZStack {
            LinearGradient(colors: [accent.opacity(0.35), Theme.ink], startPoint: .top, endPoint: .bottom)
            RadialGradient(colors: [accent.opacity(0.45), .clear], center: .init(x: 0.5, y: 0.25),
                           startRadius: 2, endRadius: 260)
            Image(systemName: "shield.lefthalf.filled")
                .font(.system(size: 54, weight: .light))
                .foregroundStyle(accent.opacity(0.55))
        }
    }
}

/// The selected game's art, blurred and darkened behind everything.
struct Backdrop: View {
    var entry: GameEntry?

    var body: some View {
        ZStack {
            Theme.ink
            if let entry, entry.art != nil {
                ArtView(entry: entry, alignment: .center)
                    .blur(radius: 46, opaque: true)
                    .saturation(1.15)
                    .opacity(0.85)
                    .id(entry.id)
                    .transition(.opacity)
            } else if let entry {
                // No art before the game's files are here: only the game's
                // colour as a glow from above (a blurred placeholder is a
                // smear of the shield and its gradient).
                RadialGradient(colors: [Theme.accent(entry.spec.accent).opacity(0.28), .clear],
                               center: .init(x: 0.5, y: 0), startRadius: 10, endRadius: 700)
                    .id(entry.id)
                    .transition(.opacity)
            }
            LinearGradient(colors: [Theme.ink.opacity(0.35), Theme.ink.opacity(0.80), Theme.ink.opacity(0.96)],
                           startPoint: .top, endPoint: .bottom)
            RadialGradient(colors: [.clear, Theme.ink.opacity(0.7)], center: .center, startRadius: 200, endRadius: 900)
        }
        .ignoresSafeArea()
        .animation(.easeInOut(duration: 0.45), value: entry?.id)
    }
}

// MARK: - Status

struct StatusInfo {
    var label: String
    var detail: String
    var symbol: String
    var color: Color

    init(_ e: GameEntry) {
        // The game's files are here but this app cannot play it yet.
        if !e.playableHere, !e.isInterrupted, e.state != .notFound {
            label = L10n.portInTheWorks
            detail = L10n.comingSoonDetail
            symbol = "hammer.fill"
            color = Color(red: 0.62, green: 0.66, blue: 0.95)
            return
        }
        switch e.state {
        case .ready:
            label = L10n.ready
            detail = e.root.map(L10n.location) ?? ""
            symbol = "checkmark.seal.fill"
            color = Color(red: 0.45, green: 0.85, blue: 0.55)
        case .notFound:
            label = L10n.notFound
            detail = L10n.notFoundDetail
            symbol = "questionmark.folder.fill"
            color = Theme.dim
        case .wrongVersion(let digest):
            label = L10n.wrongVersion
            detail = L10n.wrongVersionDetail(digest)
            symbol = "exclamationmark.triangle.fill"
            color = Color(red: 0.98, green: 0.66, blue: 0.30)
        case .incomplete(let missing):
            label = L10n.incomplete
            detail = L10n.incompleteDetail(missing)
            symbol = "exclamationmark.triangle.fill"
            color = Color(red: 0.98, green: 0.66, blue: 0.30)
        case .interrupted(let setup):
            label = L10n.interrupted
            detail = L10n.interruptedDetail(setup)
            symbol = "arrow.clockwise.circle.fill"
            color = Theme.gold
        }
    }
}

struct StatusChip: View {
    var info: StatusInfo

    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: info.symbol).font(.system(size: 10, weight: .bold))
            Text(info.label).font(.system(size: 11, weight: .semibold)).textCase(.uppercase).tracking(0.6)
        }
        .foregroundStyle(info.color)
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Capsule().fill(info.color.opacity(0.14)))
        .overlay(Capsule().strokeBorder(info.color.opacity(0.35), lineWidth: 0.5))
    }
}

// MARK: - Buttons

struct GoldButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 19, weight: .semibold, design: .serif))
            .tracking(1.2)
            .foregroundStyle(Theme.ink.opacity(enabled ? 0.92 : 0.5))
            .padding(.horizontal, 34)
            .frame(height: 52)
            .background(
                Capsule().fill(LinearGradient(colors: enabled ? [Color(red: 0.98, green: 0.86, blue: 0.58), Theme.gold, Theme.goldDeep]
                                                              : [Color(white: 0.42), Color(white: 0.30)],
                                              startPoint: .top, endPoint: .bottom))
            )
            .overlay(Capsule().strokeBorder(Color.white.opacity(enabled ? 0.45 : 0.12), lineWidth: 0.8).blendMode(.overlay))
            .shadow(color: enabled ? Theme.gold.opacity(configuration.isPressed ? 0.15 : 0.40) : .clear, radius: 18, y: 4)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.spring(duration: 0.18), value: configuration.isPressed)
    }
}

struct GhostButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(Theme.text.opacity(configuration.isPressed ? 0.6 : 0.88))
            .padding(.horizontal, 14)
            .frame(height: 34)
            .background(Capsule().fill(Color.white.opacity(configuration.isPressed ? 0.14 : 0.07)))
            .overlay(Capsule().strokeBorder(Theme.stroke, lineWidth: 0.6))
    }
}

// MARK: - Pieces

struct Wordmark: View {
    var title: String
    var tagline: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.system(size: 22, weight: .semibold, design: .serif))
                .tracking(2.5)
                .textCase(.uppercase)
                .foregroundStyle(LinearGradient(colors: [Color(red: 0.99, green: 0.90, blue: 0.68), Theme.gold],
                                                startPoint: .top, endPoint: .bottom))
            if let tagline, !tagline.isEmpty {
                Text(tagline)
                    .font(.system(size: 11, weight: .medium))
                    .tracking(1.4)
                    .textCase(.uppercase)
                    .foregroundStyle(Theme.faint)
            }
        }
    }
}

struct GameRow: View {
    var entry: GameEntry
    var selected: Bool

    var body: some View {
        let accent = Theme.accent(entry.spec.accent)
        HStack(spacing: 14) {
            ArtView(entry: entry)
                .frame(width: 76, height: 56)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(selected ? accent.opacity(0.8) : Theme.stroke, lineWidth: selected ? 1.2 : 0.6))
                .saturation(entry.state == .notFound ? 0.35 : 1)
            VStack(alignment: .leading, spacing: 5) {
                Text(entry.spec.title.text)
                    .font(.system(size: 15, weight: .semibold, design: .serif))
                    .foregroundStyle(Theme.text)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
                StatusChip(info: StatusInfo(entry))
            }
            Spacer(minLength: 0)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(selected ? Color.white.opacity(0.09) : Color.clear)
        )
        .overlay(alignment: .leading) {
            Capsule()
                .fill(accent)
                .frame(width: 3, height: selected ? 34 : 0)
                .offset(x: -1)
                .shadow(color: accent.opacity(0.8), radius: 6)
        }
        .contentShape(Rectangle())
        .animation(.easeOut(duration: 0.2), value: selected)
    }
}

struct HeroCard: View {
    var entry: GameEntry
    var height: CGFloat

    var body: some View {
        let accent = Theme.accent(entry.spec.accent)
        ZStack(alignment: .bottomLeading) {
            ArtView(entry: entry)
            LinearGradient(stops: [.init(color: .clear, location: 0), .init(color: .clear, location: 0.35),
                                   .init(color: Theme.ink.opacity(0.6), location: 0.68),
                                   .init(color: Theme.ink.opacity(0.94), location: 1)],
                           startPoint: .top, endPoint: .bottom)
            VStack(alignment: .leading, spacing: 6) {
                if let sub = entry.spec.subtitle?.text, !sub.isEmpty {
                    Text(sub)
                        .font(.system(size: 12, weight: .semibold))
                        .tracking(2)
                        .textCase(.uppercase)
                        .foregroundStyle(accent)
                }
                Text(entry.spec.title.text)
                    .font(.system(size: 34, weight: .bold, design: .serif))
                    .foregroundStyle(Theme.text)
                    .shadow(color: .black.opacity(0.6), radius: 8, y: 2)
                    .lineLimit(2)
                    .minimumScaleFactor(0.6)
                HStack(spacing: 8) {
                    if let year = entry.spec.year { Tag(text: year) }
                    if let version = entry.spec.version { Tag(text: version) }
                    StatusChip(info: StatusInfo(entry))
                }
            }
            .padding(24)
        }
        .frame(height: height)
        .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous)
            .strokeBorder(LinearGradient(colors: [accent.opacity(0.65), Theme.stroke, Theme.stroke],
                                         startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1))
        .shadow(color: .black.opacity(0.55), radius: 30, y: 16)
    }
}

struct Tag: View {
    var text: String

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .semibold).monospacedDigit())
            .foregroundStyle(Theme.dim)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(RoundedRectangle(cornerRadius: 5).fill(Color.white.opacity(0.08)))
    }
}

struct Details: View {
    @Bindable var model: LauncherModel
    var entry: GameEntry
    #if os(macOS)
    static let pathStyle = true
    #else
    static let pathStyle = false
    #endif

    var body: some View {
        let info = StatusInfo(entry)
        VStack(alignment: .leading, spacing: 16) {
            if let blurb = entry.spec.blurb?.text, !blurb.isEmpty {
                Text(blurb)
                    .font(.system(size: 15))
                    .foregroundStyle(Theme.dim)
                    .lineSpacing(3)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: info.symbol).foregroundStyle(info.color)
                Text(info.detail)
                    .font(Self.pathStyle && entry.canPlay ? .system(size: 12, design: .monospaced) : .system(size: 13))
                    .foregroundStyle(entry.canPlay ? Theme.faint : Theme.dim)
                    // A path is cut in the middle; advice is read whole.
                    .lineLimit(entry.canPlay ? 3 : nil)
                    .truncationMode(.middle)
                    .fixedSize(horizontal: false, vertical: !entry.canPlay)
                    .textSelection(.enabled)
            }
        }
    }
}

struct Actions: View {
    @Bindable var model: LauncherModel
    var entry: GameEntry
    @Binding var importerShown: Bool

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 12) { play; secondary }
            VStack(alignment: .leading, spacing: 12) { play; HStack(spacing: 10) { secondary } }
        }
    }

    private var play: some View {
        PrimaryButton(model: model, entry: entry, importerShown: $importerShown)
            .keyboardShortcut(.defaultAction)
    }

    @ViewBuilder private var secondary: some View {
        #if os(macOS)
        Button(L10n.chooseFolder) { model.pickFolder() }
            .buttonStyle(GhostButtonStyle())
            .keyboardShortcut("o", modifiers: .command)
        if entry.root != nil {
            Button(L10n.showInFinder) { model.revealSelected() }.buttonStyle(GhostButtonStyle())
        }
        if model.profileDir != nil {
            Button(L10n.saves) { model.revealSaves() }.buttonStyle(GhostButtonStyle())
        }
        #else
        // The game's files come in through Install when it is not here yet.
        if !PrimaryButton.installs(entry) {
            Button(L10n.importFolder) { importerShown = true }.buttonStyle(GhostButtonStyle())
        }
        #endif
    }
}

/// Play; Install while the selected game is not installed (the GOG.com
/// panel: a download from the player's GOG library, or their installer
/// files); Resume install while its install is unfinished.
struct PrimaryButton: View {
    @Bindable var model: LauncherModel
    var entry: GameEntry
    var fill = false
    /// iOS: the Files app's picker (LauncherRootView's fileImporter).
    var importerShown: Binding<Bool>?

    /// Whether the main button installs: the game is not here, or not whole.
    static func installs(_ entry: GameEntry) -> Bool {
        guard !entry.isInterrupted else { return false }
        switch entry.state {
        case .notFound, .incomplete, .wrongVersion: return true
        case .ready, .interrupted: return false
        }
    }

    var body: some View {
        if Self.installs(entry) {
            Button {
                model.showGog()
            } label: {
                Label(L10n.installFromGog, systemImage: "arrow.down.circle.fill").labelStyle(PlayLabel())
                    .frame(maxWidth: fill ? .infinity : nil)
            }
            .buttonStyle(GoldButtonStyle())
        } else {
            playOrResume
        }
    }

    @ViewBuilder private var playOrResume: some View {
        if entry.isInterrupted {
            Button {
                model.resumeInstall()
            } label: {
                Label(L10n.resumeInstall, systemImage: "arrow.down.circle.fill").labelStyle(PlayLabel())
                    .frame(maxWidth: fill ? .infinity : nil)
            }
            .buttonStyle(GoldButtonStyle())
        } else {
            playButton
        }
    }

    private var playButton: some View {
        Button {
            model.play()
        } label: {
            Label(L10n.play, systemImage: "play.fill").labelStyle(PlayLabel())
                .frame(maxWidth: fill ? .infinity : nil)
        }
        .buttonStyle(GoldButtonStyle())
        .disabled(!entry.canPlay)
    }
}

struct PlayLabel: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 10) {
            configuration.icon.font(.system(size: 15, weight: .bold))
            configuration.title
        }
    }
}

/// A thin gold bar (drawn by SwiftUI itself, so launcher snapshots show it too).
/// What a drag over the window shows: where to let go and what happens.
struct DropOverlay: View {
    var body: some View {
        ZStack {
            Theme.ink.opacity(0.72)
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .strokeBorder(Theme.gold.opacity(0.85), style: StrokeStyle(lineWidth: 2, dash: [10, 7]))
                .padding(18)
            VStack(spacing: 14) {
                Image(systemName: "square.and.arrow.down.on.square")
                    .font(.system(size: 44, weight: .light))
                    .foregroundStyle(Theme.gold)
                Text(L10n.dropHint)
                    .font(.system(size: 20, weight: .semibold, design: .serif))
                    .foregroundStyle(Theme.text)
                    .multilineTextAlignment(.center)
            }
            .padding(40)
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .transition(.opacity)
    }
}

struct ProgressBar: View {
    var fraction: Double

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.10))
                Capsule()
                    .fill(LinearGradient(colors: [Color(red: 0.98, green: 0.86, blue: 0.58), Theme.gold, Theme.goldDeep],
                                         startPoint: .top, endPoint: .bottom))
                    .frame(width: max(6, geo.size.width * min(1, max(0, fraction))))
                    .animation(.easeOut(duration: 0.25), value: fraction)
            }
        }
        .frame(height: 6)
    }
}

struct ImportPanel: View {
    var progress: ImportProgress
    var cancel: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(headline)
                .font(.system(size: 17, weight: .semibold, design: .serif))
                .foregroundStyle(Theme.text)
            ProgressBar(fraction: progress.fraction)
            HStack {
                Text("\(progress.filesDone) / \(progress.filesTotal) · "
                     + L10n.progressOf(progress.bytesDone, progress.bytesTotal))
                    .font(.system(size: 12).monospacedDigit())
                    .foregroundStyle(Theme.faint)
                Spacer()
                Button(L10n.cancel, action: cancel).buttonStyle(GhostButtonStyle())
            }
            Text(progress.current)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Theme.faint)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .padding(22)
        .frame(maxWidth: 460)
        .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Theme.ink.opacity(0.92)))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(Theme.stroke))
        .shadow(color: .black.opacity(0.6), radius: 40)
    }

    private var headline: String {
        switch progress.phase {
        case .copying: return L10n.importing(progress.name)
        case .checking: return L10n.checking(progress.name)
        case .extracting: return L10n.installing(progress.name)
        case .verifying: return L10n.verifying(progress.name)
        case .patching: return L10n.patching(progress.name)
        case .downloading: return L10n.downloading(progress.name)
        }
    }
}

// MARK: - Screens

/// The app's launcher-splash.jpg (copied from the game repository) fitted on
/// black; plain black when the app has none.
struct LauncherSplash: View {
    private static let picture: CGImage? = {
        guard let url = Bundle.main.url(forResource: "launcher-splash", withExtension: "jpg"),
              let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(source, 0, nil)
    }()

    var body: some View {
        ZStack {
            Color.black
            if let picture = Self.picture {
                Image(decorative: picture, scale: 1).resizable().interpolation(.high).scaledToFit()
            }
        }
        .ignoresSafeArea()
    }
}

struct LauncherRootView: View {
    /// The settings dialog's size before it is scaled to the window: a phone's
    /// fills most of the screen at its full size; held upright, all of it at
    /// the scale of the launcher behind it.
    private func settingsSize(_ window: CGSize, tall: Bool) -> CGSize {
        guard SkinSettings.phone(for: window) else { return SkinSettings.size }
        if tall {
            let scale = SkinTallLayout.scale(window.width)
            return CGSize(width: (window.width - 32) / scale, height: max(480, (window.height - 32) / scale))
        }
        return CGSize(width: max(600, window.width - 40), height: max(320, window.height - 24))
    }

    @Bindable var model: LauncherModel
    @State private var importerShown = false
    @State private var dropTargeted = false

    var body: some View {
        GeometryReader { geo in
            let wide = geo.size.width >= 760 && geo.size.height >= 480
                && (geo.size.width > geo.size.height || geo.size.width >= 1000)
            // The game's own look where its textures are on this device and
            // there is room for it - a phone held sideways too, as the game
            // itself plays there: the skinned layout scales to fit. Upright on
            // an iPhone or iPad, a column of the same pieces. The flat look
            // otherwise.
            let landscape = geo.size.width > geo.size.height && geo.size.width >= 640
                && geo.size.height >= 320
            #if os(iOS)
            // An iPhone or iPad held upright: the game's look in one column.
            let tall = !wide && !landscape
            #else
            let tall = false
            #endif
            let skin = wide || landscape || tall ? model.skin : nil
            ZStack {
                if let skin {
                    SkinBackdrop(skin: skin, entry: model.selected)
                    if tall {
                        SkinTallLayout(skin: skin, model: model, importerShown: $importerShown, size: geo.size)
                    } else {
                        // Laid out at no less than 1200 x 760 and scaled down to
                        // fit, as the game draws its own screens; a phone's at
                        // 1100 x 620, a little larger.
                        let scale = SkinSettings.phone(for: geo.size)
                            ? min(1, geo.size.width / 1100, geo.size.height / 620)
                            : min(1, geo.size.width / 1200, geo.size.height / 760)
                        SkinWideLayout(skin: skin, model: model, importerShown: $importerShown)
                            .frame(width: geo.size.width / scale, height: geo.size.height / scale)
                            .scaleEffect(scale)
                            .frame(width: geo.size.width, height: geo.size.height)
                    }
                } else {
                    Backdrop(entry: model.selected)
                    if wide {
                        WideLayout(model: model, importerShown: $importerShown, size: geo.size)
                    } else {
                        NarrowLayout(model: model, importerShown: $importerShown, size: geo.size)
                    }
                }
                if let p = model.importing {
                    if let skin {
                        // Upright, no wider than the dialog, which then fills the width.
                        SkinModal(size: geo.size, natural: CGSize(width: tall ? 660 : 900, height: 460)) {
                            SkinProgressScreen(skin: skin, progress: p) { model.cancelImport() }
                        }
                    } else {
                        Color.black.opacity(0.45).ignoresSafeArea()
                        ImportPanel(progress: p) { model.cancelImport() }.padding(24)
                    }
                } else if model.settingsShown, let skin {
                    SkinModal(size: geo.size, natural: settingsSize(geo.size, tall: tall)) {
                        SkinSettings(skin: skin, model: model, tall: tall, phone: SkinSettings.phone(for: geo.size))
                    }
                } else if model.gogShown {
                    if let skin {
                        SkinModal(size: geo.size, natural: gogSize(geo.size, tall: tall)) {
                            GogPanel(model: model, skin: skin, tall: tall, useInstallers: useInstallers)
                        }
                    } else {
                        Color.black.opacity(0.45).ignoresSafeArea()
                        GogPanel(model: model, skin: nil, useInstallers: useInstallers).padding(24)
                    }
                }
                if model.gogLoginShown {
                    GogLoginOverlay(model: model)
                }
                // Arrow keys move through the list.
                Group {
                    Button("") { step(-1) }.keyboardShortcut(.upArrow, modifiers: [])
                    Button("") { step(1) }.keyboardShortcut(.downArrow, modifiers: [])
                }
                .opacity(0)
                .frame(width: 0, height: 0)
                .accessibilityHidden(true)
                // Until the first search has found the games (and with them
                // the textures of the game's own look), a loading picture
                // rather than a moment of the flat look.
                if !model.hasScanned {
                    LauncherSplash().transition(.opacity)
                }
            }
            .animation(.easeOut(duration: 0.25), value: model.hasScanned)
        }
        .id(model.language)
        .preferredColorScheme(.dark)
        .tint(Theme.gold)
        .alert(model.notice ?? "", isPresented: Binding(get: { model.notice != nil }, set: { if !$0 { model.notice = nil } })) {
            Button(L10n.ok) { model.notice = nil }
        }
        #if os(iOS)
        // Folders and any file: the Files app greys out what the types leave
        // out, and a GOG installer's .exe and .bin have no type iOS knows.
        .fileImporter(isPresented: $importerShown, allowedContentTypes: [.folder, .data],
                      allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.usePicked(urls) }
        }
        // Always from the app's own folder in Files: left to itself the picker
        // reopens where the last pick was, and on a later launch it could hang
        // there with nothing drawn (an iPhone, after installing from a folder
        // of installers).
        .fileDialogDefaultDirectory(FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first)
        #else
        // Installers, their folders or a game folder dropped from Finder.
        .dropDestination(for: URL.self) { urls, _ in
            let files = urls.filter(\.isFileURL)
            guard model.importing == nil, !files.isEmpty else { return false }
            model.accept(files)
            return true
        } isTargeted: { dropTargeted = $0 && model.importing == nil }
        .overlay {
            if dropTargeted { DropOverlay() }
        }
        #endif
        // The first search; snapshots (ImageRenderer) run this task again and
        // must not start another one.
        .task { if !model.hasScanned { model.refresh() } }
    }

    /// The GOG.com panel's size before it is scaled to the window: a phone's
    /// as the settings', a page of the options screen elsewhere.
    private func gogSize(_ window: CGSize, tall: Bool) -> CGSize {
        SkinSettings.phone(for: window) || tall ? settingsSize(window, tall: tall) : CGSize(width: 900, height: 680)
    }

    /// The GOG panel's way to the player's own installer files.
    private func useInstallers() {
        model.gogShown = false
        #if os(macOS)
        model.pickInstaller()
        #else
        importerShown = true
        #endif
    }

    private func step(_ d: Int) {
        guard let i = model.entries.firstIndex(where: { $0.id == model.selectedID }) else { return }
        let n = model.entries.count
        withAnimation { model.selectedID = model.entries[(i + d + n) % n].id }
    }
}

struct WideLayout: View {
    @Bindable var model: LauncherModel
    @Binding var importerShown: Bool
    var size: CGSize

    var body: some View {
        let rail = min(340, max(270, size.width * 0.28))
        HStack(alignment: .top, spacing: 0) {
            VStack(alignment: .leading, spacing: 0) {
                Wordmark(title: model.title, tagline: model.config.platformTagline?.text)
                    .padding(.bottom, 30)
                Text(L10n.games)
                    .font(.system(size: 11, weight: .semibold))
                    .tracking(1.6)
                    .textCase(.uppercase)
                    .foregroundStyle(Theme.faint)
                    .padding(.leading, 10)
                    .padding(.bottom, 8)
                VStack(spacing: 6) {
                    ForEach(model.entries) { e in
                        GameRow(entry: e, selected: e.id == model.selectedID)
                            .onTapGesture { withAnimation { model.selectedID = e.id } }
                    }
                }
                Spacer(minLength: 12)
                HStack(spacing: 10) {
                    if model.scanning {
                        ProgressView().controlSize(.small)
                        Text(L10n.searching).font(.system(size: 12)).foregroundStyle(Theme.faint)
                    } else {
                        Button { model.refresh() } label: {
                            Label(L10n.refresh, systemImage: "arrow.clockwise")
                        }
                        .buttonStyle(.plain)
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.faint)
                    }
                }
                .padding(.leading, 10)
                .padding(.bottom, 10)
                Text(model.config.legal?.text ?? L10n.defaultLegal)
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.faint.opacity(0.8))
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.leading, 10)
            }
            .frame(width: rail)
            .padding(.vertical, 34)
            .padding(.leading, 30)
            .padding(.trailing, 18)
            .background(
                LinearGradient(colors: [Theme.ink.opacity(0.75), Theme.ink.opacity(0.35)], startPoint: .leading, endPoint: .trailing)
                    .ignoresSafeArea()
            )

            if let e = model.selected {
                VStack(alignment: .leading, spacing: 24) {
                    HeroCard(entry: e, height: min(size.height * 0.58, min(980, size.width - rail - 120) / 1.5))
                    Details(model: model, entry: e)
                    Actions(model: model, entry: e, importerShown: $importerShown)
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 34)
                .padding(.horizontal, 36)
                .frame(maxWidth: 980, maxHeight: .infinity, alignment: .topLeading)
                .id(e.id)
                .transition(.opacity.combined(with: .offset(y: 8)))
                .animation(.easeOut(duration: 0.3), value: model.selectedID)
            }
        }
    }
}

struct NarrowLayout: View {
    @Bindable var model: LauncherModel
    @Binding var importerShown: Bool
    var size: CGSize

    private var cardHeight: CGFloat {
        min(size.width - 36, size.height * 0.52, 640)
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    Wordmark(title: model.title, tagline: model.config.platformTagline?.text)
                        .padding(.horizontal, 22)
                        .padding(.top, 12)
                    TabView(selection: $model.selectedID) {
                        ForEach(model.entries) { e in
                            HeroCard(entry: e, height: cardHeight)
                                .padding(.horizontal, 18)
                                .padding(.bottom, 40)
                                .tag(e.id)
                        }
                    }
                    #if os(iOS)
                    .tabViewStyle(.page(indexDisplayMode: .always))
                    #endif
                    .frame(height: cardHeight + 44)
                    if let e = model.selected {
                        Details(model: model, entry: e).padding(.horizontal, 22)
                    }
                    Text(model.config.legal?.text ?? L10n.defaultLegal)
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.faint.opacity(0.8))
                        .padding(.horizontal, 22)
                        .padding(.bottom, 12)
                }
            }
            .scrollIndicators(.never)
            if let e = model.selected {
                HStack(spacing: 10) {
                    PrimaryButton(model: model, entry: e, fill: true, importerShown: $importerShown)
                    #if os(iOS)
                    // Other games' files, beside Play; Install is the same picker.
                    if !PrimaryButton.installs(e) {
                    Button {
                        importerShown = true
                    } label: {
                        Image(systemName: "folder.badge.plus").font(.system(size: 18, weight: .medium))
                    }
                    .buttonStyle(GhostButtonStyle())
                    .frame(height: 52)
                    .accessibilityLabel(L10n.importFolder)
                    }
                    #endif
                }
                .frame(maxWidth: 560)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 18)
                .padding(.top, 10)
                .padding(.bottom, 8)
                .background(LinearGradient(colors: [Theme.ink.opacity(0), Theme.ink.opacity(0.9)], startPoint: .top, endPoint: .center).ignoresSafeArea())
            }
        }
    }
}
