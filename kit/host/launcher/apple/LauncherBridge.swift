// LauncherBridge.swift - the C entry points (native_launcher.h) and the two
// presenters: an NSHostingView laid over the SDL window's content on the Mac,
// a full-screen UIHostingController over SDL's view controller on iOS.
import Foundation
import ImageIO
import SwiftUI

#if canImport(AppKit)
import AppKit
#else
import UIKit
#endif

#if !canImport(AppKit)
/// The launcher over the game's window: upright too on an iPhone (Info.plist
/// allows it; the game's own controller keeps to its orientations).
private final class LauncherHostingController<Content: View>: UIHostingController<Content> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .allButUpsideDown }
}
#endif

@MainActor
private final class LauncherSession {
    static var current: LauncherSession?

    let model: LauncherModel
    var result: Int32 = 0
    var exe: UnsafeMutablePointer<CChar>?
    var root: UnsafeMutablePointer<CChar>?
    var game: UnsafeMutablePointer<CChar>?
    var module: UnsafeMutablePointer<CChar>?
    #if canImport(AppKit)
    var host: NSView?
    weak var window: NSWindow?
    var savedTitle: String?
    #else
    var controller: UIViewController?
    #endif

    init(model: LauncherModel) {
        self.model = model
    }

    func finish(_ entry: GameEntry) {
        exe = strdup(entry.exe?.path ?? "")
        root = strdup(entry.root?.path ?? "")
        game = strdup(entry.id)
        module = strdup(entry.spec.module)
        result = 1
    }

    /// The launcher as it looks now, drawn offscreen at its window's size.
    static func snapshot(model: LauncherModel, to path: String) {
        guard let session = current else { return }
        #if canImport(AppKit)
        let size = session.host?.bounds.size ?? CGSize(width: 1280, height: 800)
        #else
        let size = session.controller?.view.bounds.size ?? CGSize(width: 1180, height: 820)
        #endif
        let renderer = ImageRenderer(content: LauncherRootView(model: model).frame(width: size.width, height: size.height))
        renderer.scale = 2
        guard let image = renderer.cgImage,
              let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(dest, image, nil)
        CGImageDestinationFinalize(dest)
    }

    #if !canImport(AppKit)
    /// The launcher may stand upright on an iPhone while the game's window
    /// (SDL's controller, SDL_HINT_ORIENTATIONS) plays sideways only: once the
    /// launcher is gone, the screen turns to what the game takes, and the game
    /// starts after that, at its own shape.
    private func turnBack(to presenter: UIViewController) {
        guard let scene = presenter.view.window?.windowScene else { return }
        let allowed = presenter.supportedInterfaceOrientations
        func fits() -> Bool {
            allowed.contains(UIInterfaceOrientationMask(rawValue: 1 << scene.effectiveGeometry.interfaceOrientation.rawValue))
        }
        presenter.setNeedsUpdateOfSupportedInterfaceOrientations()
        guard !fits() else { return }
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: allowed)) { error in
            fputs("[launcher] could not turn the screen for the game: \(error.localizedDescription)\n", stderr)
        }
        let until = Date().addingTimeInterval(2)
        while !fits(), Date() < until {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
        }
    }
    #endif

    func tearDown() {
        #if canImport(AppKit)
        host?.removeFromSuperview()
        host = nil
        if let window, let savedTitle { window.title = savedTitle }
        #else
        let presenter = controller?.presentingViewController
        controller?.dismiss(animated: false)
        controller = nil
        if let presenter { turnBack(to: presenter) }
        #endif
        for p in [exe, root, game, module] { free(p) }
        exe = nil; root = nil; game = nil; module = nil
    }
}

private func string(_ p: UnsafePointer<CChar>?) -> String {
    p.map { String(cString: $0) } ?? ""
}

@_cdecl("recomp_native_launcher_begin")
public func recompNativeLauncherBegin(_ window: UnsafeMutableRawPointer?, _ configPath: UnsafePointer<CChar>?,
                                      _ thisModule: UnsafePointer<CChar>?, _ knownExe: UnsafePointer<CChar>?,
                                      _ profileDir: UnsafePointer<CChar>?) -> Int32 {
    MainActor.assumeIsolated {
        LauncherModel.adoptFormerDefaults() // before anything reads the launcher's defaults
        guard let window,
              let data = FileManager.default.contents(atPath: string(configPath)),
              let config = try? JSONDecoder().decode(LauncherConfig.self, from: data),
              !config.games.isEmpty else { return 0 }
        let model = LauncherModel(config: config, thisModule: string(thisModule), knownExe: string(knownExe),
                                  profileDir: string(profileDir))
        model.manifestDir = URL(fileURLWithPath: string(configPath)).deletingLastPathComponent()
            .appendingPathComponent("launcher-manifests", isDirectory: true)
        let session = LauncherSession(model: model)
        model.onPlay = { [weak session] entry in session?.finish(entry) }
        model.onQuit = { [weak session] in session?.result = -1 }
        let root = LauncherRootView(model: model)
        // Unattended runs: RECOMP_LAUNCHER_DUMP writes a picture of the
        // launcher once the search is done, RECOMP_LAUNCHER_AUTOPLAY=1 then
        // presses Play when the selected game is ready.
        // RECOMP_LAUNCHER_INSTALL=<path>[:<path>...] installs what those name
        // (GOG installers, their folders, GOG updates) after the first search (and
        // RECOMP_LAUNCHER_INSTALL_CANCEL=<seconds> cancels it that long after
        // it began); autoplay then waits for the installed game.
        // RECOMP_LAUNCHER_DUMP_DIR=<dir> writes a picture every
        // RECOMP_LAUNCHER_DUMP_EVERY seconds (default 5) while the launcher is up.
        // RECOMP_LAUNCHER_GAMES_DIR=<dir> stands in for the app's Games folder
        // and is then the only place the launcher looks (LauncherModel).
        // RECOMP_LAUNCHER_GOG_PLAN=<installer id>[:<language>] logs what a
        // download from GOG.com would bring (public data, no sign-in), and
        // RECOMP_LAUNCHER_GOG=1 opens the GOG.com panel.
        let env = ProcessInfo.processInfo.environment
        if let spec = env["RECOMP_LAUNCHER_GOG_PLAN"], !spec.isEmpty {
            for one in spec.split(separator: ",") { model.logGogPlan(String(one)) }
        }
        if env["RECOMP_LAUNCHER_GOG"] == "1" { model.showGog() }
        let dump = env["RECOMP_LAUNCHER_DUMP"] ?? ""
        let autoplay = (env["RECOMP_LAUNCHER_AUTOPLAY"] ?? "0") != "0"
        let installSetup = env["RECOMP_LAUNCHER_INSTALL"] ?? ""
        let cancelAfter = Double(env["RECOMP_LAUNCHER_INSTALL_CANCEL"] ?? "") ?? 0
        if !dump.isEmpty || autoplay || !installSetup.isEmpty {
            var dumped = false, played = false, installed = false
            model.onScanned = { [weak model] in
                guard let model else { return }
                if !dump.isEmpty, !dumped {
                    dumped = true
                    LauncherSession.snapshot(model: model, to: dump)
                }
                #if os(macOS) || RECOMP_INNOEXTRACT_LIB
                if !installSetup.isEmpty, !installed {
                    installed = true
                    model.installFrom(installSetup.split(separator: ":").map { URL(fileURLWithPath: String($0)) })
                    if cancelAfter > 0 {
                        DispatchQueue.main.asyncAfter(deadline: .now() + cancelAfter) { model.cancelImport() }
                    }
                    return
                }
                #endif
                if autoplay, !played, model.importing == nil, model.selected?.canPlay == true {
                    played = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { model.play() }
                }
            }
        }
        if let dir = env["RECOMP_LAUNCHER_DUMP_DIR"], !dir.isEmpty {
            try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            let every = max(1, Double(env["RECOMP_LAUNCHER_DUMP_EVERY"] ?? "") ?? 5)
            var n = 0
            Timer.scheduledTimer(withTimeInterval: every, repeats: true) { [weak model] timer in
                MainActor.assumeIsolated {
                    guard let model, LauncherSession.current != nil else { return timer.invalidate() }
                    n += 1
                    LauncherSession.snapshot(model: model, to: String(format: "%@/launcher-%03d.png", dir, n))
                }
            }
        }

        #if canImport(AppKit)
        let nsWindow = Unmanaged<NSWindow>.fromOpaque(window).takeUnretainedValue()
        guard let content = nsWindow.contentView else { return 0 }
        let host = NSHostingView(rootView: root)
        host.frame = content.bounds
        host.autoresizingMask = [.width, .height]
        content.addSubview(host, positioned: .above, relativeTo: nil)
        nsWindow.makeFirstResponder(host)
        session.host = host
        session.window = nsWindow
        session.savedTitle = nsWindow.title
        nsWindow.title = model.title
        #else
        let uiWindow = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue()
        guard let presenter = uiWindow.rootViewController else { return 0 }
        let controller = LauncherHostingController(rootView: root)
        controller.modalPresentationStyle = .fullScreen
        controller.view.backgroundColor = UIColor(red: 0.035, green: 0.04, blue: 0.055, alpha: 1)
        var top = presenter
        while let next = top.presentedViewController { top = next }
        top.present(controller, animated: false)
        session.controller = controller
        // RECOMP_LAUNCHER_UPRIGHT=1 stands the launcher upright as a phone held
        // so would, =0 lays it sideways (a simulator without its window cannot
        // be turned).
        if let upright = env["RECOMP_LAUNCHER_UPRIGHT"], let scene = uiWindow.windowScene {
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: upright == "1" ? .portrait : .landscape)) { _ in }
        }
        #endif
        LauncherSession.current = session
        return 1
    }
}

@_cdecl("recomp_native_launcher_poll")
public func recompNativeLauncherPoll() -> Int32 {
    MainActor.assumeIsolated { LauncherSession.current?.result ?? -1 }
}

@_cdecl("recomp_native_launcher_exe")
public func recompNativeLauncherExe() -> UnsafePointer<CChar>? {
    UnsafePointer(bitPattern: MainActor.assumeIsolated { Int(bitPattern: LauncherSession.current?.exe) })
}

@_cdecl("recomp_native_launcher_root")
public func recompNativeLauncherRoot() -> UnsafePointer<CChar>? {
    UnsafePointer(bitPattern: MainActor.assumeIsolated { Int(bitPattern: LauncherSession.current?.root) })
}

@_cdecl("recomp_native_launcher_game")
public func recompNativeLauncherGame() -> UnsafePointer<CChar>? {
    UnsafePointer(bitPattern: MainActor.assumeIsolated { Int(bitPattern: LauncherSession.current?.game) })
}

@_cdecl("recomp_native_launcher_module")
public func recompNativeLauncherModule() -> UnsafePointer<CChar>? {
    UnsafePointer(bitPattern: MainActor.assumeIsolated { Int(bitPattern: LauncherSession.current?.module) })
}

@_cdecl("recomp_native_launcher_end")
public func recompNativeLauncherEnd() {
    MainActor.assumeIsolated {
        LauncherSession.current?.tearDown()
        LauncherSession.current = nil
    }
}
