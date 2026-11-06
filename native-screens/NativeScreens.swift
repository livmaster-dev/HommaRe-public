// NativeScreens.swift - the game's screens drawn natively over it on iOS: the
// C entry points kit/host/sdl/platform_ui_ios.mm calls (platform_ui_native_screen),
// the store the views read and the commands they send back.
//
// The game keeps each of these screens alive and works it as before; it only
// stops drawing it (native/native_screens.c) and describes it every time it
// changes, as one JSON document {"screen": "<id>", ...}. The views here draw
// that document and answer with commands, {"do": "<verb>", ...}, which the
// game applies on its own thread through the screen's own paths (its
// messages, its list selection, its arrows). Nothing here knows the game's
// rules: what is allowed and what each value is called comes in the document.
//
// Screens: "custom_create" (Single player > Custom game: the map and its
// settings, CustomGameCreate.swift) and "custom_players" (the players before
// the game starts, CustomGamePlayers.swift).

import SwiftUI
import UIKit

public typealias ScreenCommandFn = @convention(c) (UnsafePointer<CChar>?) -> Void

/// A screen's document: decoded from the game's JSON, drawn by its view.
protocol ScreenDocument: Decodable {
    associatedtype Body: View
    @ViewBuilder @MainActor func view() -> Body
}

extension ScreenDocument {
    @MainActor func anyView() -> AnyView { AnyView(view()) }
}

/// The screens there are views for, by the document's "screen".
private let documentTypes: [String: any ScreenDocument.Type] = [
    "custom_create": CreateScreen.self,
    "custom_players": PlayersScreen.self,
    "hotseat_create": CreateScreen.self,
    "hotseat_players": PlayersScreen.self,
    "main_menu": MainMenuScreen.self,
    "profile_name": ProfileNameScreen.self,
    "load_game": LoadScreen.self,
    "save_game": LoadScreen.self,
    "hall_of_fame": HallOfFameScreen.self,
    "options": OptionsScreen.self,
    "duel": DuelScreen.self,
    "duel3": DuelScreen.self,
    "question": QuestionScreen.self,
    "esc_menu": EscMenuScreen.self,
    // LAN games (Lan.swift and the shared screens they extend).
    "lan_lobby": LanLobbyScreen.self,
    "lan_name": ProfileNameScreen.self,
    "form": FormScreen.self,
    "mp_create": CreateScreen.self,
    "mp_players": PlayersScreen.self,
    "lan_duel": DuelScreen.self,
    "lan_duel3": DuelScreen.self,
    // ubi-gs (screens_ubi.c): the login is a "form", the CD key a "lan_name".
    "table": TableScreen.self,
]

/// The document the game sent last, decoded for the view of its screen.
final class ScreenStore: ObservableObject {
    @Published var screen: String = ""
    @Published var document: (any ScreenDocument)?
    /// The interface's language, as the game's own texts are ("ru", "en"...).
    @Published var lang: String = "en"
    var command: ScreenCommandFn?

    func load(_ json: Data) {
        let decoder = JSONDecoder()
        guard let head = try? decoder.decode(ScreenHead.self, from: json) else {
            print("[screens] unreadable document"); fflush(stdout)
            return
        }
        // A document there is no view for, or one that does not read, must
        // not leave the last screen's view up: its buttons would go to this one.
        guard let type = documentTypes[head.screen] else {
            print("[screens] no view for \(head.screen)"); fflush(stdout)
            document = nil
            screen = ""
            return
        }
        do {
            document = try decoder.decode(type, from: json)
        } catch {
            print("[screens] \(head.screen): \(error)"); fflush(stdout)
            document = nil
            screen = ""
            return
        }
        if let l = head.lang { lang = l }
        screen = head.screen
    }

    /// A command to the game: {"do": verb, ...args}; a tick felt unless it
    /// only follows typing (`feedback` false).
    func send(_ verb: String, _ args: [String: Any] = [:], feedback: Bool = true) {
        var all = args
        all["do"] = verb
        guard let command, let data = try? JSONSerialization.data(withJSONObject: all),
              let text = String(data: data, encoding: .utf8) else { return }
        text.withCString { command($0) }
        if feedback { UISelectionFeedbackGenerator().selectionChanged() }
    }

    /// A word of the views' own, in the game's language: Russian, English,
    /// or German, French and Polish from ScreenWords (else English).
    func tr(_ ru: String, _ en: String) -> String {
        if lang.hasPrefix("ru") { return ru }
        return ScreenWords.table[en]?[lang] ?? en
    }
}

private struct ScreenHead: Decodable {
    let screen: String
    let lang: String?
}

/// The view for the screen showing now.
struct ScreenRoot: View {
    @EnvironmentObject var store: ScreenStore
    /// The game's frames and fonts read: the screen is drawn again with them.
    @ObservedObject private var kit = KitStore.shared

    var body: some View {
        ZStack {
            // Takes every touch that misses the panels, so none reaches the
            // screen the game keeps undrawn; clear, so the compositor has no
            // full-screen layer to blend over the game every frame (the
            // earlier look dimmed the scene: Theme.heavy).
            (Theme.heavy ? Color.black.opacity(0.3) : Color.clear)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture {}
            if let document = store.document {
                document.anyView()
                    // A new screen is a new view: no state carried over.
                    .id(store.screen + (kit.ready ? "" : "-"))
            }
        }
        .environment(\.colorScheme, .dark)
    }
}

// MARK: - The host's side

private final class ScreenHost {
    static let shared = ScreenHost()
    let store = ScreenStore()
    var controller: UIHostingController<AnyView>?

    func show(in root: UIView, state: Data, command: ScreenCommandFn?) {
        if controller == nil { Kit.warm() } // the game's frames and buttons, read once
        store.command = command
        store.load(state)
        if let c = controller {
            if c.view.superview !== root {
                c.view.removeFromSuperview()
                attach(c, to: root)
            }
            if c.view.alpha < 1 || !c.view.isUserInteractionEnabled {
                c.view.isUserInteractionEnabled = true
                UIView.animate(withDuration: 0.18) { c.view.alpha = 1 }
            }
            return
        }
        let c = UIHostingController(rootView: AnyView(ScreenRoot().environmentObject(store)))
        // The game's 3D scene shows behind the panels.
        c.view.backgroundColor = .clear
        c.view.alpha = 0
        controller = c
        attach(c, to: root)
        UIView.animate(withDuration: 0.18) { c.view.alpha = 1 }
    }

    /// Hidden, not dropped: the game shows the screen again after a message
    /// box above it, and the lists keep their place. Once faded the view
    /// leaves the window, so nothing lies over the game's Metal layer while
    /// it plays (RECOMP_NATIVE_SCREENS_KEEP_HIDDEN=1: it stays, transparent,
    /// as before - for comparing).
    func hide() {
        guard let c = controller else { return }
        c.view.isUserInteractionEnabled = false
        let keep = ProcessInfo.processInfo.environment["RECOMP_NATIVE_SCREENS_KEEP_HIDDEN"] == "1"
        UIView.animate(withDuration: 0.12, animations: { c.view.alpha = 0 }) { _ in
            // Shown again meanwhile (show() set it back): it stays.
            if !keep && c.view.alpha == 0 && !c.view.isUserInteractionEnabled {
                c.view.removeFromSuperview()
            }
        }
    }

    private func attach(_ c: UIHostingController<AnyView>, to root: UIView) {
        let v = c.view!
        v.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(v)
        NSLayoutConstraint.activate([
            v.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            v.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            v.topAnchor.constraint(equalTo: root.topAnchor),
            v.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
    }
}

@_cdecl("recomp_native_screens_show")
public func recomp_native_screens_show(_ root: UnsafeMutableRawPointer?, _ state: UnsafePointer<CChar>?,
                                       _ command: ScreenCommandFn?) {
    guard let root, let state else { return }
    let view = Unmanaged<UIView>.fromOpaque(root).takeUnretainedValue()
    let data = Data(bytes: state, count: strlen(state))
    ScreenHost.shared.show(in: view, state: data, command: command)
}

@_cdecl("recomp_native_screens_hide")
public func recomp_native_screens_hide() {
    ScreenHost.shared.hide()
}
