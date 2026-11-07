// LauncherModel.swift - the native launcher's state on macOS and iOS: which of
// the games the app knows (launcher-games.json from the game repository) are
// on this device, whether each is the supported build, and the folder the
// player picked or imported. No game files ship with the app: the art and the
// executables are read from the player's own copy.
import CryptoKit
import Foundation
import ImageIO
import Observation
import RecompSettingsFile
import UniformTypeIdentifiers

#if canImport(AppKit)
import AppKit
typealias PlatformImage = NSImage
#else
import UIKit
typealias PlatformImage = UIImage
#endif

// MARK: - Configuration

/// Text in the languages the config carries ("ru", "en"); the player's first
/// preferred language wins, then English, then whatever is there.
struct LocalizedText: Decodable, Hashable {
    var values: [String: String]

    init(_ values: [String: String]) {
        self.values = values
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let s = try? c.decode(String.self) {
            values = ["en": s]
        } else {
            values = try c.decode([String: String].self)
        }
    }

    var text: String {
        values[L10n.language] ?? values["en"] ?? values.values.first ?? ""
    }
}

struct LauncherConfig: Decodable {
    var title: LocalizedText?
    var tagline: LocalizedText?
    /// The tagline of each platform ("macos", "ios", "linux"...), over `tagline`.
    var taglines: [String: LocalizedText]?
    var legal: LocalizedText?
    /// Folder names worth looking for under the usual places.
    var installNames: [String]?
    /// Top-level names an import leaves out ([bundle] exclude in game.toml).
    var exclude: [String]?
    var games: [GameSpec]
    /// GOG offline installers the Mac app installs from (LauncherInstaller.swift).
    var installers: [InstallerSpec]?

    /// This platform's tagline, else the common one.
    var platformTagline: LocalizedText? {
        #if os(iOS)
        let platform = "ios"
        #else
        let platform = "macos"
        #endif
        return taglines?[platform] ?? tagline
    }
}

struct GameSpec: Decodable, Identifiable, Hashable {
    var id: String
    /// The game build that plays it: the app's own RECOMP_GAME_ID, or another
    /// module that may not be in this app yet.
    var module: String
    var title: LocalizedText
    var subtitle: LocalizedText?
    var blurb: LocalizedText?
    var year: String?
    var version: String?
    /// goggame-<id>.info marks a GOG install of this game.
    var gogId: String?
    /// Where the executable sits under the install root, first match wins.
    var exe: [String]
    var sha256: [String]
    var requiredDirs: [String]?
    /// Splash images in the player's install, relative to the root.
    var art: [String]?
    /// The part of the splash worth showing, as x, y, width, height in 0...1
    /// (splashes carry a logo and rating badges the launcher draws around).
    var artCrop: [Double]?
    /// Accent colour as #RRGGBB.
    var accent: String?
}

// MARK: - Status

enum GameState: Equatable {
    case notFound
    case ready
    case wrongVersion(digest: String)
    case incomplete(missing: [String])
    /// An install from `setup` stopped before every file checked out.
    case interrupted(setup: String)
}

struct GameEntry: Identifiable {
    let spec: GameSpec
    var state: GameState = .notFound
    var root: URL?
    var exe: URL?
    var art: PlatformImage?
    /// False while the port of this game is not part of the app.
    var playableHere: Bool

    var id: String { spec.id }
    var canPlay: Bool { playableHere && state == .ready }
    var isInterrupted: Bool {
        if case .interrupted = state { return true }
        return false
    }
}

struct ImportProgress: Equatable {
    enum Phase {
        case copying, checking, extracting, verifying, patching, downloading
    }

    var name: String
    var phase = Phase.copying
    var filesDone = 0, filesTotal = 0
    var bytesDone: Int64 = 0, bytesTotal: Int64 = 0
    var current = ""

    var fraction: Double {
        bytesTotal > 0 ? min(1, Double(bytesDone) / Double(bytesTotal)) : 0
    }
}

// MARK: - Model

@MainActor
@Observable
final class LauncherModel {
    let config: LauncherConfig
    /// This app's own game module (RECOMP_GAME_ID).
    let thisModule: String
    /// Every game module the app can run: its own and those beside it.
    let modules: Set<String>
    let profileDir: URL?
    /// Where the install manifests are (launcher-manifests/ beside the config).
    var manifestDir: URL?
    private(set) var entries: [GameEntry]
    var selectedID: String
    private(set) var scanning = false
    /// A search has finished at least once.
    private(set) var hasScanned = false
    private(set) var importing: ImportProgress?
    /// The game's own textures from an install on this device (LauncherSkin.swift).
    private(set) var skin: Skin?
    /// The launcher's language; changing it redraws every screen.
    var language = L10n.language
    /// The choice in Settings: a code of L10n.languages, or nil for the system's.
    private(set) var languageChoice = L10n.chosen
    var settingsShown = false
    /// The GOG.com panel (LauncherGogView.swift): sign in, the account's
    /// games, download one straight into the app.
    var gogShown = false
    /// GOG's sign-in page is up.
    var gogLoginShown = false
    /// The signed-in GOG account's name ("" when GOG gave none), nil when no one is.
    private(set) var gogAccount: String? = GogSession.account
    /// The GOG ids of the account's games, nil until known.
    private(set) var gogOwned: Set<String>?
    /// A sign-in or a look at the library is under way.
    private(set) var gogBusy = false
    /// What went wrong in the GOG panel, shown there.
    var gogError: String?
    /// The language of the game's files a download brings (a code of `gogLanguages`).
    var gogLanguage = L10n.language {
        didSet { if gogLanguage != oldValue { refreshGogSizes() } }
    }
    /// The languages a download from GOG.com is offered in: those every
    /// installer has a manifest for ("gog-<id>-<code>.json"), whose files are
    /// checked against it. GOG has all ten; the others wait for manifests.
    var gogLanguages: [L10n.Language] {
        let installers = config.installers ?? []
        return L10n.languages.filter { language in
            !installers.isEmpty && installers.allSatisfy { $0.manifests.contains { $0.hasSuffix("-\(language.code).json") } }
        }
    }
    /// About how much each installer's download is in `gogLanguage` (InstallerSpec.id).
    private(set) var gogSizes: [String: Int64] = [:]
    /// The game's frame and energy choices (mods/settings_file.h): stored in
    /// the profile's mod-settings.json, read by the game when it starts.
    private(set) var frameLimit = LauncherModel.displaySetting(LauncherModel.frameLimitKey, 2)
    private(set) var energySaver = LauncherModel.displaySetting(LauncherModel.energySaverKey, 0)
    /// A touch device's render quality: the share of the screen's resolution
    /// the 3D is drawn at, MetalFX upscaling it (mods_render_fraction). 0
    /// automatic, 1 the screen's own, 2 quality, 3 balanced, 4 performance.
    private(set) var renderQuality = LauncherModel.displaySetting(LauncherModel.renderQualityKey, 0)
    private(set) var idleFrames = LauncherModel.displaySetting(LauncherModel.idleFramesKey, 0)
    /// The camera buttons over the game's 3D map (touch devices).
    private(set) var cameraButtons = LauncherModel.displaySetting(LauncherModel.cameraButtonsKey, 1) != 0
    /// The game's commands beside them (its keys a touch screen cannot press).
    private(set) var commandPanel = LauncherModel.displaySetting(LauncherModel.commandPanelKey, 1) != 0
    /// "Touch-friendly battle": a melee by two taps, the enemy, then the cell
    /// to strike it from (a game that supports it; read when the game starts).
    /// On by default on an iPhone or iPad (kit/host/sdl/main.cpp).
    #if os(iOS)
    private(set) var battleCells = LauncherModel.displaySetting(LauncherModel.battleCellsKey, 1) != 0
    #else
    private(set) var battleCells = LauncherModel.displaySetting(LauncherModel.battleCellsKey, 0) != 0
    #endif
    /// "Touch": native gestures, the game acting on each at its point with no
    /// cursor (a game that supports it; read when the game starts), or the
    /// cursor the fingers move. Off by default (kit/host/sdl/main.cpp).
    private(set) var nativeTouch = LauncherModel.displaySetting(LauncherModel.nativeTouchKey, 0) != 0
    /// A phone's own interface: larger panels and buttons, the corner menus as
    /// rows of buttons (the game reads it when it starts). 0 never, 1 where
    /// the screen is a phone's (a foldable's closed one, not its opened one),
    /// 2 always.
    private(set) var phoneInterface = LauncherModel.displaySetting(LauncherModel.phoneInterfaceKey, 1)
    /// The game's menus drawn natively (SwiftUI over the game, the game's
    /// native/native_screens.c): 1 on, 2 off (the default by device, sdl/main.cpp).
    /// Read when the game starts.
    private(set) var nativeMenus = LauncherModel.displaySetting(LauncherModel.nativeMenusKey, 0) == 1
    /// The device's silent mode: 0 the game is silent in it, 1 plays anyway.
    private(set) var ignoreSilentMode = LauncherModel.displaySetting(LauncherModel.silentModeKey, 0) != 0
    /// The box the interface's edge-anchored screens stay in (Heroes V's
    /// native/widescreen.c): 0 the whole screen, 1 21:9, 2 16:9, 3 4:3.
    private(set) var uiBounds = LauncherModel.displaySetting(LauncherModel.uiBoundsKey, 0)
    /// What a Mac trackpad's two fingers do on the 3D map: 0 the camera's
    /// height (a vertical scroll), 1 as on an iPad (scroll pans, pinch zooms,
    /// turn rotates), 2 the game's own wheel (sdl/main.cpp install_trackpad).
    private(set) var trackpadCamera = LauncherModel.displaySetting(LauncherModel.trackpadCameraKey, 0)
    /// Apple's Metal performance HUD over the game (frame time, GPU, memory).
    /// The host sets MTL_HUD_ENABLED from it before Metal starts (sdl/main.cpp),
    /// so a change shows from the app's next start.
    private(set) var metalHud = LauncherModel.displaySetting(LauncherModel.metalHudKey, 0) != 0
    var metalHudPending: Bool {
        metalHud != (ProcessInfo.processInfo.environment["MTL_HUD_ENABLED"].map { $0 != "0" } ?? false)
    }
    /// Animated models skinned and lit on the GPU (the game's native
    /// gpu_skin.cpp): 0 never chosen (on), 1 on, 2 off. Read when the game starts.
    private(set) var gpuSkin = LauncherModel.displaySetting(LauncherModel.gpuSkinKey, 0) != 2
    /// The startup videos are skipped: the profile shadows their sequence files.
    private(set) var skipIntro = LauncherModel.introSkipped()
    /// The saves kept in step with a folder every device sees (iCloud Drive):
    /// LauncherCloudSaves.swift, the game's host/cloud_saves.h.
    var cloudSaves = UserDefaults.standard.bool(forKey: LauncherModel.cloudSavesKey)
    /// That folder: iCloud Drive's HommaRe on a Mac unless the player chose
    /// another, the one the player picked on iOS; nil when there is none.
    var cloudFolder: URL? = LauncherModel.savedCloudFolder()
    /// iOS: the Files app's folder picker for it is up.
    var cloudPickerShown = false
    var notice: String? {
        // Unattended runs see the alerts in the log.
        didSet { if let notice { FileHandle.standardError.write(Data("[launcher] \(notice)\n".utf8)) } }
    }

    /// Set by the presenter: the player pressed Play or closed the launcher.
    var onPlay: ((GameEntry) -> Void)?
    var onQuit: (() -> Void)?
    /// Called after each search (the presenter's test hooks).
    @ObservationIgnored var onScanned: (() -> Void)?

    @ObservationIgnored private var knownExe: URL?
    @ObservationIgnored private var importTask: Task<Void, Never>?
    /// GOG installers waiting for the one being installed.
    @ObservationIgnored private var installQueue: [URL] = []
    /// GOG updates queued with installers: they also fill in what an older
    /// build of an installer lacks, and say nothing when there is nothing to do.
    @ObservationIgnored private var quietPatches: Set<URL> = []
    /// What the player picked in the Files app, readable until the installs
    /// from it are done (iOS).
    @ObservationIgnored private var picked: [URL] = []
    @ObservationIgnored private let defaults = UserDefaults.standard
    private static let rootsKey = "launcher.roots"
    private static let lastKey = "launcher.lastGame"

    /// `modules` is the app's own game module, then any others it carries,
    /// comma-separated ("homm5,homm5-hof").
    init(config: LauncherConfig, thisModule modules: String, knownExe: String?, profileDir: String?) {
        self.config = config
        let list = modules.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        let thisModule = list.first ?? ""
        let available = Set(list)
        self.thisModule = thisModule
        self.modules = available
        self.knownExe = knownExe.flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0) }
        self.profileDir = profileDir.flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0) }
        entries = config.games.map { GameEntry(spec: $0, playableHere: available.contains($0.module)) }
        // RECOMP_LAUNCHER_PICK=<game id>: an unattended run's choice.
        let picked = ProcessInfo.processInfo.environment["RECOMP_LAUNCHER_PICK"]
        let last = picked ?? UserDefaults.standard.string(forKey: Self.lastKey)
        selectedID = config.games.first(where: { $0.id == last })?.id
            ?? config.games.first(where: { $0.module == thisModule })?.id
            ?? config.games.first?.id ?? ""
    }

    var selected: GameEntry? {
        entries.first { $0.id == selectedID }
    }

    var title: String {
        config.title?.text ?? (Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "")
    }

    // MARK: Detection

    func refresh() {
        guard !scanning else { return }
        scanning = true
        let specs = config.games
        let candidates = candidateRoots()
        let module = thisModule
        let current = skin
        Task.detached(priority: .userInitiated) {
            let found = Detector.scan(specs: specs, roots: candidates)
            let skin = Self.skin(for: found, specs: specs, module: module, current: current)
            await MainActor.run {
                self.apply(found)
                self.skin = skin
                self.scanning = false
                self.onScanned?()
                #if os(iOS)
                self.importBundledGameIfNeeded()
                #endif
            }
        }
    }

    /// The same search on the calling thread, for previews and tests.
    func refreshNow() {
        let found = Detector.scan(specs: config.games, roots: candidateRoots())
        apply(found)
        skin = Self.skin(for: found, specs: config.games, module: thisModule, current: skin)
    }

    /// The textures of the install the launcher would play first: this app's
    /// own game when it is ready, else any other found copy. RECOMP_LAUNCHER_SKIN=0
    /// keeps the flat look.
    nonisolated static func skin(for found: [String: Detector.Found], specs: [GameSpec], module: String,
                                 current: Skin?) -> Skin? {
        if ProcessInfo.processInfo.environment["RECOMP_LAUNCHER_SKIN"] == "0" { return nil }
        let ranked = specs.compactMap { spec -> (Int, URL)? in
            guard let f = found[spec.id] else { return nil }
            switch f.state {
            case .interrupted: return nil
            case .ready: return (spec.module == module ? 0 : 1, f.root)
            default: return (2, f.root)
            }
        }.sorted { $0.0 < $1.0 }
        for (_, root) in ranked {
            if let current, current.root == root { return current }
            if let skin = Skin.load(root: root) { return skin }
        }
        return nil
    }

    func setLanguage(_ code: String?) {
        L10n.chosen = code
        languageChoice = code
        language = L10n.language
    }

    static let frameLimitKey = "host.display/frame_limit"
    static let energySaverKey = "host.display/energy_saver"
    static let renderQualityKey = "host.display/render_quality"
    static let idleFramesKey = "host.display/idle_frames"
    static let cameraButtonsKey = "host.touch/camera_buttons"
    static let commandPanelKey = "host.touch/hotkeys"
    static let battleCellsKey = "host.touch/battle_cells"
    static let nativeTouchKey = "host.touch/native"
    static let phoneInterfaceKey = "host.ui/phone_interface"
    static let silentModeKey = "host.audio/silent_mode"
    static let uiBoundsKey = "homm5.wide/ui_bounds"
    static let trackpadCameraKey = "host.input/trackpad_camera"
    static let metalHudKey = "host.display/metal_hud"
    static let gpuSkinKey = "homm5.perf/gpu_skin"
    static let nativeMenusKey = "host.ui/native_menus"

    private static func displaySetting(_ key: String, _ fallback: Int) -> Int {
        var value: Int64 = 0
        return mods_settings_file_get(key, &value) != 0 ? Int(value) : fallback
    }

    /// 0 the game's own pacing, 1 = 40, 2 = 60, 3 = 120 fps.
    func setFrameLimit(_ value: Int) {
        if store(Self.frameLimitKey, value) { frameLimit = value }
    }
    /// 0 automatic, 1 always, 2 never.
    func setRenderQuality(_ value: Int) {
        if store(Self.renderQualityKey, value) { renderQuality = value }
    }
    func setEnergySaver(_ value: Int) {
        if store(Self.energySaverKey, value) { energySaver = value }
    }
    /// 0 automatic, 1 on, 2 off.
    func setIdleFrames(_ value: Int) {
        if store(Self.idleFramesKey, value) { idleFrames = value }
    }
    func setCameraButtons(_ on: Bool) {
        if store(Self.cameraButtonsKey, on ? 1 : 0) { cameraButtons = on }
    }
    func setCommandPanel(_ on: Bool) {
        if store(Self.commandPanelKey, on ? 1 : 0) { commandPanel = on }
    }
    func setBattleCells(_ on: Bool) {
        if store(Self.battleCellsKey, on ? 1 : 0) { battleCells = on }
    }
    func setNativeTouch(_ on: Bool) {
        if store(Self.nativeTouchKey, on ? 1 : 0) { nativeTouch = on }
    }
    func setIgnoreSilentMode(_ on: Bool) {
        if store(Self.silentModeKey, on ? 1 : 0) { ignoreSilentMode = on }
    }
    func setPhoneInterface(_ value: Int) {
        if store(Self.phoneInterfaceKey, value) { phoneInterface = value }
    }
    /// 0 the whole screen, 1 21:9, 2 16:9, 3 4:3.
    func setUiBounds(_ value: Int) {
        if store(Self.uiBoundsKey, value) { uiBounds = value }
    }
    func setTrackpadCamera(_ value: Int) {
        if store(Self.trackpadCameraKey, value) { trackpadCamera = value }
    }
    func setMetalHud(_ on: Bool) {
        if store(Self.metalHudKey, on ? 1 : 0) { metalHud = on }
    }
    func setGpuSkin(_ on: Bool) {
        if store(Self.gpuSkinKey, on ? 1 : 2) { gpuSkin = on }
    }
    func setNativeMenus(_ on: Bool) {
        if store(Self.nativeMenusKey, on ? 1 : 2) { nativeMenus = on }
    }

    /// The startup videos (video/intro.xml, and intro_a1.xml for the
    /// expansions) are lists the game plays in turn. An empty list in the
    /// profile, which shadows the game's files, skips them. The marker tells
    /// these files from a mod's own.
    private static let introFiles = ["video/intro.xml", "video/intro_a1.xml"]
    private static let introMarker = "<!-- launcher: skip the startup videos -->"
    private static var overlayRoot: URL? {
        guard let dir = mods_settings_file_dir() else { return nil }
        let path = String(cString: dir)
        return path.isEmpty ? nil : URL(fileURLWithPath: path, isDirectory: true)
    }
    private static func introSkipped() -> Bool {
        guard let root = overlayRoot,
              let text = try? String(contentsOf: root.appendingPathComponent(introFiles[0]), encoding: .utf8)
        else { return false }
        return text.contains(introMarker)
    }
    func setSkipIntro(_ on: Bool) {
        guard let root = Self.overlayRoot else {
            notice = L10n.settingNotSaved
            return
        }
        let empty = "<?xml version=\"1.0\"?>\n\(Self.introMarker)\n<Base>\n\t<Movies/>\n</Base>\n"
        do {
            for name in Self.introFiles {
                let url = root.appendingPathComponent(name)
                if on {
                    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                            withIntermediateDirectories: true)
                    try empty.write(to: url, atomically: true, encoding: .utf8)
                } else if let text = try? String(contentsOf: url, encoding: .utf8), text.contains(Self.introMarker) {
                    try FileManager.default.removeItem(at: url)
                }
            }
            skipIntro = on
        } catch {
            notice = L10n.settingNotSaved
        }
    }

    private func store(_ key: String, _ value: Int) -> Bool {
        if mods_settings_file_set(key, Int64(value)) != 0 { return true }
        notice = L10n.settingNotSaved
        return false
    }

    /// Folders the player picked or installed into, newest first.
    var rememberedRoots: [URL] {
        Self.liveRoots(defaults.stringArray(forKey: Self.rootsKey) ?? []).map { URL(fileURLWithPath: $0) }
    }

    /// The remembered folders that still exist, once each. An iOS app's
    /// container moves with every reinstall, which left the old paths of the
    /// same Documents/Games folder behind ("On this device: game" four times).
    nonisolated static func liveRoots(_ paths: [String]) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        let fm = FileManager.default
        for p in paths {
            let canonical = URL(fileURLWithPath: p).resolvingSymlinksInPath().standardizedFileURL.path
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: canonical, isDirectory: &isDir), isDir.boolValue,
                  seen.insert(canonical).inserted else { continue }
            out.append(p)
        }
        return out
    }

    private func apply(_ found: [String: Detector.Found]) {
        hasScanned = true
        for i in entries.indices {
            let id = entries[i].id
            if let f = found[id] {
                entries[i].state = f.state
                entries[i].root = f.root
                entries[i].exe = f.exe
                entries[i].art = f.art
            } else {
                entries[i].state = .notFound
                entries[i].root = nil
                entries[i].exe = nil
                entries[i].art = nil
            }
        }
        // A first launch lands on a game that is actually there.
        if let sel = selected, !sel.canPlay, defaults.string(forKey: Self.lastKey) == nil,
           let ready = entries.first(where: { $0.canPlay }) {
            selectedID = ready.id
        }
    }

    /// Where a copy may be: folders the player picked, the import folder, the
    /// executable the host already knows, and a few well-known places. The
    /// privacy-protected folders (Desktop, Documents, Downloads) are only read
    /// when the player picks something there.
    private func candidateRoots() -> [URL] {
        var roots: [URL] = []
        func add(_ u: URL?) {
            guard let u else { return }
            let s = u.standardizedFileURL
            if !roots.contains(s) { roots.append(s) }
        }
        // A test run's own Games folder (RECOMP_LAUNCHER_GAMES_DIR) is the
        // only place it looks.
        if ProcessInfo.processInfo.environment["RECOMP_LAUNCHER_GAMES_DIR"].map({ !$0.isEmpty }) == true {
            for base in Self.importBase().map({ [$0] }) ?? [] {
                for child in Detector.children(of: base) { add(child) }
            }
            return roots
        }
        for p in Self.liveRoots(defaults.stringArray(forKey: Self.rootsKey) ?? []) {
            add(URL(fileURLWithPath: p))
        }
        if let exe = knownExe {
            // The flat developer view keeps the executable at the root; a GOG
            // install keeps it one folder down (bin/, bina1/).
            let dir = exe.deletingLastPathComponent()
            add(dir)
            add(dir.deletingLastPathComponent())
        }
        for base in Self.importBase().map({ [$0] }) ?? [] {
            for child in Detector.children(of: base) { add(child) }
        }
        #if os(macOS)
        let home = FileManager.default.homeDirectoryForCurrentUser
        var bases = [home, home.appendingPathComponent("Games"), home.appendingPathComponent("Applications"),
                     URL(fileURLWithPath: "/Applications")]
        let bottles = home.appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        for bottle in Detector.children(of: bottles) {
            bases.append(bottle.appendingPathComponent("drive_c/GOG Games"))
            bases.append(bottle.appendingPathComponent("drive_c/Program Files (x86)/GOG Galaxy/Games"))
        }
        let names = Set((config.installNames ?? []).map { $0.lowercased() })
        // Looking inside Desktop, Documents, Downloads and the media folders
        // asks the player for each of them (and again for every newly signed
        // build), so the home folder's own are passed over.
        let guarded: Set<String> = ["Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures",
                                    "Library", "Public"]
        for base in bases {
            for child in Detector.children(of: base) {
                if base == home && guarded.contains(child.lastPathComponent) { continue }
                let n = child.lastPathComponent.lowercased()
                if names.contains(where: { n.contains($0) }) || Detector.hasGogInfo(child) {
                    add(child)
                    for grandchild in Detector.children(of: child) { add(grandchild) }
                }
            }
        }
        #endif
        return roots
    }

    /// Where imports live: Documents/Games on iOS (the Files app shows it),
    /// Application Support/<app>/Games on the Mac.
    nonisolated static func importBase() -> URL? {
        let fm = FileManager.default
        // Test runs keep their installs away from the player's own folder.
        if let dir = ProcessInfo.processInfo.environment["RECOMP_LAUNCHER_GAMES_DIR"], !dir.isEmpty {
            return URL(fileURLWithPath: dir, isDirectory: true)
        }
        #if os(iOS)
        guard let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first else { return nil }
        return docs.appendingPathComponent("Games", isDirectory: true)
        #else
        guard let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return nil }
        let app = Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "Recomp"
        return support.appendingPathComponent(app).appendingPathComponent("Games", isDirectory: true)
        #endif
    }

    func remember(root: URL) {
        let canonical = root.resolvingSymlinksInPath().standardizedFileURL.path
        var list = Self.liveRoots(defaults.stringArray(forKey: Self.rootsKey) ?? [])
        list.removeAll { URL(fileURLWithPath: $0).resolvingSymlinksInPath().standardizedFileURL.path == canonical }
        list.insert(root.path, at: 0)
        defaults.set(Array(list.prefix(8)), forKey: Self.rootsKey)
    }

    /// A renamed app (game.toml previous_*, Info.plist RecompPrevious*) takes the
    /// remembered folders, the last game and the language from the first former
    /// bundle id that has any, once: only while its own defaults have none. A
    /// remembered folder inside a former Application Support/<name> that is gone
    /// becomes the same folder under the app's own name, where the host has moved
    /// it (host_migrate_own_user_data). macOS only: an iOS app cannot read another's.
    static func adoptFormerDefaults() {
        #if os(macOS)
        let languageKey = "launcher.language"
        let keys = [rootsKey, lastKey, languageKey]
        let defaults = UserDefaults.standard
        guard keys.allSatisfy({ defaults.object(forKey: $0) == nil }) else { return }
        let info = Bundle.main.infoDictionary ?? [:]
        let formerIDs = info["RecompPreviousBundleIDs"] as? [String] ?? []
        guard let former = formerIDs.lazy.compactMap({ UserDefaults(suiteName: $0) })
                .first(where: { old in keys.contains { old.object(forKey: $0) != nil } }) else { return }
        let fm = FileManager.default
        let app = info["CFBundleName"] as? String ?? ""
        let formerNames = info["RecompPreviousAppNames"] as? [String] ?? []
        // By the folder's name rather than the whole Application Support path,
        // which can be spelled two ways (/tmp and /private/tmp).
        func moved(_ path: String) -> String {
            guard !app.isEmpty, !fm.fileExists(atPath: path) else { return path }
            for name in formerNames {
                guard let at = path.range(of: "/Application Support/" + name + "/") else { continue }
                let renamed = path.replacingCharacters(in: at, with: "/Application Support/" + app + "/")
                if fm.fileExists(atPath: renamed) { return renamed }
            }
            return path
        }
        if let roots = former.stringArray(forKey: rootsKey) {
            defaults.set(roots.map(moved), forKey: rootsKey)
        }
        for key in [lastKey, languageKey] {
            if let value = former.object(forKey: key) { defaults.set(value, forKey: key) }
        }
        #endif
    }

    // MARK: Actions

    func play() {
        guard let entry = selected, entry.canPlay else { return }
        defaults.set(entry.id, forKey: Self.lastKey)
        if let root = entry.root { remember(root: root) }
        onPlay?(entry)
    }

    func quit() {
        importTask?.cancel()
        #if os(macOS)
        GogInstaller.stopRunning()
        #endif
        onQuit?()
    }

    /// A folder the player picked: played in place on the Mac, copied into the
    /// app on iOS (an app there cannot keep reading a folder it was shown).
    func use(folder: URL) {
        #if os(iOS)
        importFolder(folder)
        #else
        accept([folder])
        #endif
    }

    func importFolder(_ source: URL) {
        guard importing == nil, let base = Self.importBase() else { return }
        let scoped = source.startAccessingSecurityScopedResource()
        let roots = Detector.rootsBelow(source, specs: config.games)
        guard let root = roots.first else {
            if scoped { source.stopAccessingSecurityScopedResource() }
            notice = L10n.noGameIn(source.lastPathComponent)
            return
        }
        let dest = base.appendingPathComponent(root.lastPathComponent, isDirectory: true)
        let exclude = config.exclude ?? []
        importing = ImportProgress(name: root.lastPathComponent)
        let update = { @MainActor [weak self] (p: ImportProgress) in
            if self?.importing != nil { self?.importing = p }
        }
        let done = { @MainActor [weak self] (result: Result<Void, Error>) in
            guard let self else { return }
            self.importing = nil
            switch result {
            case .success:
                self.remember(root: dest)
                self.refresh()
            case .failure(let error) where !(error is CancellationError):
                self.notice = L10n.importFailed(error.localizedDescription)
            case .failure:
                break
            }
        }
        importTask = Task.detached(priority: .userInitiated) {
            let result = Importer.copy(from: root, to: dest, exclude: exclude) { p in
                Task { @MainActor in update(p) }
                return !Task.isCancelled
            }
            if scoped { source.stopAccessingSecurityScopedResource() }
            await done(result)
        }
    }

    func cancelImport() {
        importTask?.cancel()
    }

    #if os(iOS)
    /// What the player picked in the Files app: a folder holding an installed
    /// game is imported; GOG installers (setup_*.exe, its .bin slices, folders
    /// they are in) are installed one after another, as on the Mac. A build
    /// without innoextract tells the player to unpack the installer on a Mac.
    func usePicked(_ urls: [URL]) {
        guard importing == nil, !urls.isEmpty else { return }
        func isFolder(_ url: URL) -> Bool {
            var isDir: ObjCBool = false
            return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue
        }
        releasePicked()
        // The picked items stay readable while their installs run.
        picked = urls.filter { $0.startAccessingSecurityScopedResource() }
        if let folder = urls.first(where: { isFolder($0) && !Detector.rootsBelow($0, specs: config.games).isEmpty }) {
            releasePicked()
            importFolder(folder)
            return
        }
        #if RECOMP_INNOEXTRACT_LIB
        installFound(in: urls)
        if importing == nil { releasePicked() }
        #else
        releasePicked()
        let file = urls[0]
        let ext = file.pathExtension.lowercased()
        notice = ext == "exe" || ext == "bin" ? L10n.installerOnPhone : L10n.noGameIn(file.lastPathComponent)
        #endif
    }

    private func releasePicked() {
        for url in picked { url.stopAccessingSecurityScopedResource() }
        picked.removeAll()
    }

    /// Bookmarks to the picked items `setup` is read through: its folder (or
    /// one above it) or the installer's own files.
    private func bookmarks(for setup: URL) -> [Data] {
        let dir = setup.deletingLastPathComponent().standardizedFileURL.path
        let stem = setup.deletingPathExtension().lastPathComponent.lowercased()
        return picked.filter { url in
            let path = url.standardizedFileURL.path
            if dir == path || dir.hasPrefix(path.hasSuffix("/") ? path : path + "/") { return true }
            let name = url.lastPathComponent.lowercased()
            return url.deletingLastPathComponent().standardizedFileURL.path == dir
                && (name == stem + ".exe" || InstallerFinder.slice(name)?.base == stem)
        }.compactMap { try? $0.bookmarkData() }
    }

    /// Reads an unfinished install's bookmarks again (they outlive the launch
    /// the player picked the installer in).
    private func reopenAccess(_ marker: InstallMarker) {
        releasePicked()
        for data in marker.access ?? [] {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale),
               url.startAccessingSecurityScopedResource() {
                picked.append(url)
            }
        }
    }

    /// A developer build carries the game in the app bundle (game/, staged by
    /// tools/stage_game_files.py). With nothing playable yet, import it the
    /// way a picked folder is imported, once per launch.
    private var bundledImportTried = false
    private func importBundledGameIfNeeded() {
        guard !bundledImportTried, importing == nil, !entries.contains(where: { $0.canPlay }),
              let bundled = Bundle.main.resourceURL?.appendingPathComponent("game", isDirectory: true),
              FileManager.default.fileExists(atPath: bundled.path) else { return }
        bundledImportTried = true
        importFolder(bundled)
    }
    #endif

    #if os(macOS)
    func pickFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = L10n.choose
        panel.message = L10n.pickMessage
        if panel.runModal() == .OK, let url = panel.url {
            use(folder: url)
        }
    }

    /// The player's GOG offline installers: setup_*.exe, any of its .bin
    /// slices, the folders they are in; several at once.
    func pickInstaller(message: String = L10n.pickInstallerMessage) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = ["exe", "bin"].compactMap { UTType(filenameExtension: $0) } + [.folder]
        panel.prompt = L10n.install
        panel.message = message
        if panel.runModal() == .OK, !panel.urls.isEmpty {
            accept(panel.urls)
        }
    }

    /// Files or folders the player picked or dropped on the launcher. A folder
    /// (or a game's executable) inside an installed game is played where it
    /// is; otherwise the GOG installers found in them are installed one after
    /// another, and what keeps one from installing is told in the player's words.
    func accept(_ urls: [URL]) {
        guard importing == nil, !urls.isEmpty else { return }
        let specs = config.games
        func holdsGame(_ dir: URL) -> Bool { specs.contains { Detector.probe($0, root: dir) != nil } }
        var roots: [URL] = [], rest: [URL] = []
        for url in urls {
            var isDir: ObjCBool = false
            FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
            if isDir.boolValue {
                let found = Detector.rootsBelow(url, specs: specs)
                if found.isEmpty { rest.append(url) } else { roots += found }
            } else if url.pathExtension.lowercased() == "exe", !InstallerFinder.isSetupName(url.lastPathComponent),
                      !InstallerFinder.isPatchName(url.lastPathComponent),
                      let root = [url.deletingLastPathComponent(), url.deletingLastPathComponent().deletingLastPathComponent()]
                          .first(where: holdsGame) {
                // The game's own H5_Game.exe: its install, not an installer.
                roots.append(root)
            } else {
                rest.append(url)
            }
        }
        for r in roots { remember(root: r) }
        if !roots.isEmpty { refresh() }
        let found = InstallerFinder.find(in: rest)
        var messages = found.problems.map(InstallerFinder.describe)
        if found.setups.isEmpty, found.patches.isEmpty, roots.isEmpty, messages.isEmpty {
            messages = found.emptyFolders.map { url in
                var isDir: ObjCBool = false
                FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
                return isDir.boolValue ? L10n.noInstallerIn(url.lastPathComponent) : L10n.notInstaller(url.lastPathComponent)
            }
        }
        if !messages.isEmpty { notice = messages.joined(separator: "\n\n") }
        installQueue = found.setups + found.patches
        quietPatches = found.setups.isEmpty ? [] : Set(found.patches)
        installNext()
    }
    #endif

    // MARK: GOG.com

    func showGog() {
        guard importing == nil else { return }
        let offered = gogLanguages.map(\.code)
        if !offered.contains(gogLanguage) {
            gogLanguage = offered.contains(L10n.language) ? L10n.language : offered.contains("en") ? "en" : offered.first ?? "en"
        }
        gogError = nil
        settingsShown = false
        gogShown = true
        refreshGogLibrary()
        refreshGogSizes()
    }

    /// GOG's sign-in page came back with `code`.
    func gogSignedIn(code: String) {
        gogLoginShown = false
        gogBusy = true
        gogError = nil
        Task { [weak self] in
            do {
                let name = try await GogSession.shared.signIn(code: code)
                self?.gogAccount = name
                self?.gogBusy = false
                self?.refreshGogLibrary()
            } catch {
                self?.gogBusy = false
                self?.gogError = L10n.gogSignInFailed(error.localizedDescription)
            }
        }
    }

    func gogSignOut() {
        GogSession.signOut()
        gogAccount = nil
        gogOwned = nil
    }

    /// Which of the installers' products the signed-in account owns.
    func refreshGogLibrary() {
        guard gogAccount != nil, !gogBusy else { return }
        gogBusy = true
        Task { [weak self] in
            do {
                let owned = try await GogSession.shared.owned()
                self?.gogOwned = owned
            } catch let e as GogFailure where e.signedOut {
                self?.gogAccount = nil
                self?.gogOwned = nil
                self?.gogError = e.errorDescription
            } catch {
                self?.gogError = error.localizedDescription
            }
            self?.gogBusy = false
        }
    }

    /// The sizes of the downloads in the chosen language (GOG's public build data).
    private func refreshGogSizes() {
        let language = gogLanguage
        for installer in config.installers ?? [] {
            Task { [weak self] in
                guard let built = try? await GogDepots.build(for: installer), self?.gogLanguage == language else { return }
                self?.gogSizes[installer.id] = GogDepots.estimate(built.1, language: language)
            }
        }
    }

    /// Downloads `installer`'s games from GOG.com into the Games folder;
    /// `source` resumes an unfinished download (its build and language).
    func downloadFromGog(_ installer: InstallerSpec, source: GogSource? = nil) {
        guard importing == nil, let base = Self.importBase() else { return }
        let config = self.config
        let manifestDir = self.manifestDir
        let language = source?.language ?? gogLanguage
        gogShown = false
        importing = ImportProgress(name: installer.title?.text ?? installer.folder, phase: .checking, current: "GOG.com")
        let update = { @MainActor [weak self] (p: ImportProgress) in
            if self?.importing != nil { self?.importing = p }
        }
        let done = { @MainActor [weak self] (result: Result<Void, Error>) in
            guard let self else { return }
            self.importing = nil
            switch result {
            case .success:
                if let id = installer.games.first(where: { id in self.entries.contains { $0.id == id && $0.playableHere } })
                    ?? installer.games.first {
                    self.selectedID = id
                }
            case .failure(let error as GogFailure) where error.signedOut:
                self.gogAccount = nil
                self.gogOwned = nil
                self.gogError = error.errorDescription
                self.gogShown = true
            case .failure(let error) where !(error is CancellationError):
                self.notice = L10n.installFailed(error.localizedDescription)
            case .failure:
                break
            }
            self.refresh()
        }
        importTask = Task.detached(priority: .userInitiated) {
            let result = await Self.gogInstall(installer, language: language, buildID: source?.build, config: config,
                                               manifestDir: manifestDir, base: base) { p in
                Task { @MainActor in update(p) }
                return !Task.isCancelled
            }
            await done(result)
        }
    }

    nonisolated private static func gogInstall(_ installer: InstallerSpec, language: String, buildID: String?,
                                               config: LauncherConfig, manifestDir: URL?, base: URL,
                                               progress: (ImportProgress) -> Bool) async -> Result<Void, Error> {
        do {
            guard try await GogSession.shared.owned().contains(installer.gogId) else {
                throw GogFailure(errorDescription: L10n.gogNotInLibrary(installer.title?.text ?? installer.folder))
            }
            let plan = try await GogDepots.plan(installer: installer, language: language, buildID: buildID, config: config,
                                                manifestDir: manifestDir, base: base) { _ in !Task.isCancelled }
            return await GogDownloader.run(plan, progress: progress)
        } catch {
            return .failure(error)
        }
    }

    /// What a download of `spec` ("<installer id>[:<language>]") would bring,
    /// from GOG's public build data, in the log: no sign-in, nothing
    /// downloaded (RECOMP_LAUNCHER_GOG_PLAN).
    func logGogPlan(_ spec: String) {
        let parts = spec.split(separator: ":").map(String.init)
        let log = { (s: String) in FileHandle.standardError.write(Data("[launcher] gog plan: \(s)\n".utf8)) }
        guard let id = parts.first, let installer = (config.installers ?? []).first(where: { $0.id == id }),
              let base = Self.importBase() else { return log("no installer \(spec)") }
        let language = parts.count > 1 ? parts[1] : gogLanguage
        let config = self.config
        let manifestDir = self.manifestDir
        Task.detached {
            do {
                let plan = try await GogDepots.plan(installer: installer, language: language, config: config,
                                                    manifestDir: manifestDir, base: base) { _ in true }
                var lines = plan.files.map { f in
                    "  \(f.path) \(f.size) \(f.chunks.count) \(f.sha256 == nil ? "md5" : "sha256") -> "
                        + GogDownloader.place(f.path, in: plan.dest).path.dropFirst(plan.dest.path.count + 1)
                }
                lines.insert("\(installer.id) \(language): build \(plan.source.build), manifest \(plan.manifestName ?? "none"), "
                             + "\(plan.files.count) files, \(plan.bytes) bytes, "
                             + "\(plan.files.filter { $0.sha256 != nil }.count) with SHA-256, "
                             + "\(plan.files.reduce(0) { $0 + $1.chunks.count }) chunks -> \(plan.dest.path)", at: 0)
                lines.forEach(log)
            } catch {
                log("\(installer.id) \(language) failed: \(error.localizedDescription)")
            }
        }
    }

    /// Picks up an install the selected game's folder says is unfinished: a
    /// download from GOG.com downloads on, an installer unpacks on.
    func resumeInstall() {
        guard let entry = selected, case .interrupted(let setup) = entry.state else { return }
        if let root = entry.root, let marker = InstallMarker.read(root), let gog = marker.gog,
           let installer = (config.installers ?? []).first(where: { $0.id == marker.installer }) {
            downloadFromGog(installer, source: gog)
            return
        }
        #if os(macOS) || RECOMP_INNOEXTRACT_LIB
        resumeFromInstaller(setup, entry: entry)
        #else
        notice = L10n.installerOnPhone
        #endif
    }

    #if os(macOS) || RECOMP_INNOEXTRACT_LIB
    #if os(iOS)
    /// The GOG installers in what the player picked, installed one after
    /// another; what keeps one from installing is told in the player's words.
    private func installFound(in urls: [URL]) {
        let found = InstallerFinder.find(in: urls)
        // A file picked alone: its partner beside it is out of the app's reach.
        let files = Set(urls.map { $0.standardizedFileURL.path })
        var messages = found.problems.map { problem -> String in
            switch problem {
            case .missingSlice(let exe, let expected, nil) where files.contains(exe.standardizedFileURL.path):
                return L10n.pickBoth(exe.lastPathComponent, expected)
            case .missingSetup(let bin, let expected, nil) where files.contains(bin.standardizedFileURL.path):
                return L10n.pickBoth(bin.lastPathComponent, expected)
            default:
                return InstallerFinder.describe(problem)
            }
        }
        if found.setups.isEmpty, found.patches.isEmpty, messages.isEmpty {
            messages = found.emptyFolders.map { url in
                var isDir: ObjCBool = false
                FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
                return isDir.boolValue ? L10n.noInstallerIn(url.lastPathComponent) : L10n.notInstaller(url.lastPathComponent)
            }
        }
        if !messages.isEmpty { notice = messages.joined(separator: "\n\n") }
        installQueue = found.setups + found.patches
        quietPatches = found.setups.isEmpty ? [] : Set(found.patches)
        installNext()
    }
    #endif

    private func installNext() {
        guard importing == nil, !installQueue.isEmpty else { return }
        let next = installQueue.removeFirst()
        if InstallerFinder.isPatchName(next.lastPathComponent) {
            applyPatch(next, quiet: quietPatches.contains(next))
        } else {
            install(setup: next)
        }
    }

    /// Installers, their folders, GOG updates: what the player picked or
    /// named (RECOMP_LAUNCHER_INSTALL), installed one after another.
    func installFrom(_ urls: [URL]) {
        #if os(macOS)
        accept(urls)
        #else
        installFound(in: urls)
        #endif
    }

    /// Applies a GOG update (patch_*.exe) to the installs it is for; `quiet`
    /// leaves out the note when it found nothing to replace.
    func applyPatch(_ patch: URL, quiet: Bool = false) {
        guard importing == nil, let base = Self.importBase() else { return }
        let config = self.config
        let manifestDir = self.manifestDir
        importing = ImportProgress(name: patch.lastPathComponent, phase: .patching, current: patch.lastPathComponent)
        let update = { @MainActor [weak self] (p: ImportProgress) in
            if self?.importing != nil { self?.importing = p }
        }
        let done = { @MainActor [weak self] (result: Result<[GogInstaller.PatchResult], Error>) in
            guard let self else { return }
            self.importing = nil
            switch result {
            case .success(let results):
                let lines = results.compactMap { r -> String? in
                    r.replaced > 0 ? L10n.patchApplied(r.title) : quiet ? nil : L10n.patchUpToDate(r.title)
                }
                if !lines.isEmpty { self.notice = lines.joined(separator: "\n\n") }
            case .failure(let error) where !(error is CancellationError):
                self.installQueue.removeAll()
                self.notice = L10n.installFailed(error.localizedDescription)
            case .failure:
                self.installQueue.removeAll()
            }
            self.refresh()
            self.installNext()
            #if os(iOS)
            if self.importing == nil { self.releasePicked() }
            #endif
        }
        importTask = Task.detached(priority: .userInitiated) {
            #if os(iOS)
            var error: NSError?
            NSFileCoordinator().coordinate(readingItemAt: patch, options: [], error: &error) { _ in }
            #endif
            let result = GogInstaller.applyPatch(patch, config: config, manifestDir: manifestDir, base: base) { p in
                Task { @MainActor in update(p) }
                return !Task.isCancelled
            }
            await done(result)
        }
    }

    /// Unpacks on from the installer an unfinished install came from.
    private func resumeFromInstaller(_ setup: String, entry: GameEntry) {
        #if os(iOS)
        if let root = entry.root, let marker = InstallMarker.read(root) { reopenAccess(marker) }
        #endif
        if FileManager.default.fileExists(atPath: setup) {
            install(setup: URL(fileURLWithPath: setup))
        } else {
            #if os(macOS)
            pickInstaller(message: L10n.setupGone(setup))
            #else
            releasePicked()
            notice = L10n.setupGone(setup)
            #endif
        }
    }

    func install(setup: URL) {
        guard importing == nil, let base = Self.importBase() else { return }
        let config = self.config
        let manifestDir = self.manifestDir
        #if os(iOS)
        let access = bookmarks(for: setup)
        #endif
        let patches = installQueue.filter { InstallerFinder.isPatchName($0.lastPathComponent) }
        importing = ImportProgress(name: setup.lastPathComponent, phase: .checking, current: setup.lastPathComponent)
        let update = { @MainActor [weak self] (p: ImportProgress) in
            if self?.importing != nil { self?.importing = p }
        }
        let done = { @MainActor [weak self] (result: Result<GogInstaller.Plan, Error>) in
            guard let self else { return }
            self.importing = nil
            switch result {
            case .success(let plan):
                // The installed game this app plays; another one only when
                // nothing playable is selected (a queue installs several).
                if let id = plan.installer.games.first(where: { id in self.entries.contains { $0.id == id && $0.playableHere } }) {
                    self.selectedID = id
                } else if self.selected?.canPlay != true, let id = plan.installer.games.first {
                    self.selectedID = id
                }
            case .failure(let error) where !(error is CancellationError):
                self.installQueue.removeAll()
                self.notice = L10n.installFailed(error.localizedDescription)
            case .failure:
                self.installQueue.removeAll()
            }
            self.refresh()
            self.installNext()
            #if os(iOS)
            if self.importing == nil { self.releasePicked() }
            #endif
        }
        importTask = Task.detached(priority: .userInitiated) {
            #if os(iOS)
            GogInstaller.download(setup)
            #endif
            let result = GogInstaller.plan(setup: setup, config: config, manifestDir: manifestDir, base: base)
                .flatMap { plan in
                    var plan = plan
                    plan.patches = patches
                    #if os(iOS)
                    plan.access = access
                    #endif
                    return GogInstaller.run(plan) { p in
                        Task { @MainActor in update(p) }
                        return !Task.isCancelled
                    }.map { plan }
                }
            await done(result)
        }
    }
    #endif

    #if os(macOS)
    func revealSelected() {
        guard let url = selected?.exe ?? selected?.root else { return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    func revealSaves() {
        guard let dir = profileDir else { return }
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(dir)
    }
    #endif
}

// MARK: - Detection (off the main actor)

enum Detector {
    struct Found {
        var state: GameState
        var root: URL
        var exe: URL?
        var art: PlatformImage?
    }

    nonisolated(unsafe) private static var digestCache: [String: (Int64, Date, String)] = [:]
    private static let cacheLock = NSLock()

    static func children(of dir: URL) -> [URL] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return [] }
        return names.filter { !$0.hasPrefix(".") }.compactMap { name in
            let u = dir.appendingPathComponent(name, isDirectory: true)
            var isDir: ObjCBool = false
            return fm.fileExists(atPath: u.path, isDirectory: &isDir) && isDir.boolValue ? u : nil
        }
    }

    static func hasGogInfo(_ dir: URL) -> Bool {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return names.contains { $0.hasPrefix("goggame-") && $0.hasSuffix(".info") }
    }

    /// The entry of `dir` called `name`, compared without case (a copy made on
    /// Windows or by an installer may spell BIN or Data).
    static func entry(_ dir: URL, _ relative: String) -> URL? {
        var url = dir
        let fm = FileManager.default
        for part in relative.split(separator: "/").map(String.init) {
            let exact = url.appendingPathComponent(part)
            if fm.fileExists(atPath: exact.path) {
                url = exact
                continue
            }
            guard let names = try? fm.contentsOfDirectory(atPath: url.path),
                  let match = names.first(where: { $0.caseInsensitiveCompare(part) == .orderedSame })
            else { return nil }
            url = url.appendingPathComponent(match)
        }
        return url
    }

    static func sha256(_ url: URL) -> String {
        let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attrs?[.size] as? NSNumber)?.int64Value ?? -1
        let mtime = attrs?[.modificationDate] as? Date ?? .distantPast
        cacheLock.lock()
        if let c = digestCache[url.path], c.0 == size, c.1 == mtime {
            cacheLock.unlock()
            return c.2
        }
        cacheLock.unlock()
        guard let h = FileHandle(forReadingAtPath: url.path) else { return "" }
        defer { try? h.close() }
        var hasher = SHA256()
        while let chunk = try? h.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
        }
        let digest = hasher.finalize().map { String(format: "%02x", $0) }.joined()
        cacheLock.lock()
        digestCache[url.path] = (size, mtime, digest)
        cacheLock.unlock()
        return digest
    }

    /// How well `root` holds `spec`: nil when its executable is not there.
    static func probe(_ spec: GameSpec, root: URL) -> Found? {
        // Another GOG product's folder (Tribes of the East keeps its executable
        // at bin/H5_Game.exe too): only that product's games count there.
        if let gog = spec.gogId, hasGogInfo(root), entry(root, "goggame-\(gog).info") == nil {
            return nil
        }
        for rel in spec.exe {
            guard let exe = entry(root, rel) else { continue }
            let digest = sha256(exe)
            let art = (spec.art ?? []).lazy.compactMap { entry(root, $0) }.first.flatMap { loadArt($0, crop: spec.artCrop) }
            guard spec.sha256.contains(digest) else {
                return Found(state: .wrongVersion(digest: digest), root: root, exe: exe, art: art)
            }
            let missing = (spec.requiredDirs ?? []).filter { entry(root, $0) == nil }
            return Found(state: missing.isEmpty ? .ready : .incomplete(missing: missing), root: root, exe: exe, art: art)
        }
        return nil
    }

    static func loadArt(_ url: URL, crop: [Double]?) -> PlatformImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let full = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        var image = full
        if let c = crop, c.count == 4 {
            let w = Double(full.width), h = Double(full.height)
            let rect = CGRect(x: c[0] * w, y: c[1] * h, width: c[2] * w, height: c[3] * h).integral
            image = full.cropping(to: rect) ?? full
        }
        #if canImport(AppKit)
        return NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
        #else
        return UIImage(cgImage: image)
        #endif
    }

    static func rank(_ s: GameState) -> Int {
        switch s {
        case .ready: return 3
        case .incomplete, .interrupted: return 2
        case .wrongVersion: return 1
        case .notFound: return 0
        }
    }

    static func scan(specs: [GameSpec], roots: [URL]) -> [String: Found] {
        var best: [String: Found] = [:]
        for root in roots {
            // An unfinished install: its games wait for it to resume.
            if let marker = InstallMarker.read(root) {
                for spec in specs where marker.games.contains(spec.id) {
                    if let old = best[spec.id], rank(old.state) >= 2 { continue }
                    let art = (spec.art ?? []).lazy.compactMap { entry(root, $0) }.first.flatMap { loadArt($0, crop: spec.artCrop) }
                    best[spec.id] = Found(state: .interrupted(setup: marker.setup), root: root, exe: nil, art: art)
                }
                continue
            }
            for spec in specs {
                guard let f = probe(spec, root: root) else { continue }
                if let old = best[spec.id], rank(old.state) >= rank(f.state) { continue }
                best[spec.id] = f
            }
        }
        return best
    }

    /// Install roots at `dir` or up to two levels below it that hold any game.
    static func rootsBelow(_ dir: URL, specs: [GameSpec]) -> [URL] {
        var out: [URL] = []
        var level = [dir]
        for _ in 0..<3 {
            var next: [URL] = []
            for d in level {
                if specs.contains(where: { probe($0, root: d) != nil }) {
                    out.append(d)
                } else {
                    next += children(of: d)
                }
            }
            level = next
        }
        return out
    }
}

// MARK: - Import

enum Importer {
    struct Failure: LocalizedError {
        var errorDescription: String?
    }

    static func excluded(_ relative: String, _ patterns: [String]) -> Bool {
        let first = relative.split(separator: "/").first.map(String.init) ?? relative
        let name = (relative as NSString).lastPathComponent
        return patterns.contains { p in
            fnmatch(p.lowercased(), first.lowercased(), 0) == 0 || fnmatch(p.lowercased(), name.lowercased(), 0) == 0
        }
    }

    /// Copies `src` into `dest`. Files already there with the same size are
    /// kept, so an interrupted import resumes; each file is written as
    /// <name>.part and renamed.
    static func copy(from src: URL, to dest: URL, exclude: [String],
                     progress: (ImportProgress) -> Bool) -> Result<Void, Error> {
        let fm = FileManager.default
        guard let walker = fm.enumerator(at: src, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey]) else {
            return .failure(Failure(errorDescription: src.path))
        }
        var files: [(URL, String, Int64)] = []
        var total: Int64 = 0
        let base = src.standardizedFileURL.path
        for case let url as URL in walker {
            let path = url.standardizedFileURL.path
            guard path.hasPrefix(base) else { continue }
            let rel = String(path.dropFirst(base.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            if rel.isEmpty || excluded(rel, exclude) {
                if !rel.isEmpty, (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                    walker.skipDescendants()
                }
                continue
            }
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey])
            if values?.isDirectory == true { continue }
            let size = Int64(values?.fileSize ?? 0)
            files.append((url, rel, size))
            total += size
        }
        var p = ImportProgress(name: src.lastPathComponent, filesTotal: files.count, bytesTotal: total)
        if let free = (try? dest.deletingLastPathComponent().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage,
           free > 0, free < total {
            return .failure(Failure(errorDescription: L10n.noSpace(total, free)))
        }
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for (url, rel, size) in files {
                p.current = rel
                guard progress(p) else { return .failure(CancellationError()) }
                let target = dest.appendingPathComponent(rel)
                try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                let have = (try? fm.attributesOfItem(atPath: target.path)[.size] as? NSNumber)?.int64Value
                if have != size {
                    let part = target.appendingPathExtension("part")
                    try? fm.removeItem(at: part)
                    try? fm.removeItem(at: target)
                    try fm.copyItem(at: url, to: part)
                    try fm.moveItem(at: part, to: target)
                }
                p.filesDone += 1
                p.bytesDone += size
            }
            _ = progress(p)
            #if os(iOS)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var d = dest
            try? d.setResourceValues(values)
            #endif
            return .success(())
        } catch {
            return .failure(error)
        }
    }
}
