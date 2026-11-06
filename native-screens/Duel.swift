// Duel.swift - "duel" and "duel3": hot seat's duels. Two players choose,
// one after the other, from the game's ready heroes: one hero each in a
// duel, three each in a 3x3. The presets (names, portraits) come from the
// game's own table in the paks; the document says whose turn it is, what each
// player holds and the settings (arena, battle speed; Hammers of Fate:
// dynamic battles instead of a speed), which are the first player's to change.

import SwiftUI

struct DuelScreen: Decodable {
    let screen: String
    let title: String
    let turn: Int
    let count: Int
    let players: [Player]
    let arenas: [String]
    let arena: Int
    let speeds: [String]?
    let speed: Int?
    /// Hammers of Fate: a dynamic battles flag instead of a speed.
    let dynamic: Flag?
    let settings_enabled: Bool
    let arena_label: String
    let speed_label: String
    let random: String
    let player_names: [String]
    let buttons: [ScreenButton]
    /// A network duel's chat (Lan.swift).
    let chat: ChatState?

    struct Flag: Decodable {
        let label: String
        let on: Bool
    }

    struct Player: Decodable {
        /// A preset index, -1 random, -2 nothing yet; one per slot.
        let picks: [Int]
        let shown: Bool
    }

    var triple: Bool { screen == "duel3" || screen == "lan_duel3" }
}

extension DuelScreen: ScreenDocument {
    func view() -> some View { DuelView(screen: self) }
}

/// One of the game's ready heroes (UI/MPDMLobby/presets.(DuelPresets).xdb).
struct DuelPreset: Identifiable {
    let id: Int
    let name: String
    let left: String
    let right: String
    let round: String

    static let all: [DuelPreset] = load()

    private static func load() -> [DuelPreset] {
        guard let data = GameArt.shared.file("UI/MPDMLobby/presets.(DuelPresets).xdb"),
              let xml = String(data: data, encoding: .utf8) else { return [] }
        func attr(_ item: Substring, _ tag: String) -> String {
            guard let r = item.range(of: "<\(tag) href=\"") else { return "" }
            let rest = item[r.upperBound...]
            return rest.firstIndex(of: "\"").map { String(rest[..<$0]) } ?? ""
        }
        return xml.components(separatedBy: "<Item>").dropFirst().enumerated().map { i, raw in
            let item = Substring(raw)
            let nameRef = attr(item, "PresetNameFileRef")
            return DuelPreset(id: i, name: GameArt.shared.text(nameRef) ?? "#\(i + 1)",
                              left: attr(item, "LeftFace"), right: attr(item, "RightFace"),
                              round: attr(item, "RoundedFace"))
        }
    }
}

struct DuelView: View {
    let screen: DuelScreen
    @EnvironmentObject var store: ScreenStore
    /// 3x3: the slot of the current player a pick goes to.
    @State private var slot = 0
    @State private var chatting = false

    private var presets: [DuelPreset] { Array(DuelPreset.all.prefix(screen.count)) }
    private var current: DuelScreen.Player? { screen.players.indices.contains(screen.turn) ? screen.players[screen.turn] : nil }

    var body: some View {
        VStack(spacing: 8) {
            HStack {
                ScreenHeader(title: screen.title) { store.send("back") }
                if screen.chat != nil {
                    Button { chatting = true } label: {
                        Label(store.tr("Чат", "Chat"), systemImage: "bubble.left.and.bubble.right.fill")
                    }
                    .buttonStyle(PlateButtonStyle())
                }
            }
            HStack(alignment: .top, spacing: 10) {
                // A phone's height: the column scrolls rather than push the
                // buttons off the screen.
                ScrollView(showsIndicators: false) {
                    VStack(spacing: 8) {
                        players
                        settings
                    }
                }
                .frame(width: 290)
                VStack(spacing: 8) {
                    grid
                    HStack(spacing: 8) {
                        if let back = screen.buttons[named: "back"] {
                            Button(back.label) { store.send("back") }
                                .buttonStyle(PlateButtonStyle())
                                .frame(maxWidth: 170)
                        }
                        if let next = screen.buttons[named: "next"] {
                            Button(next.label) { store.send("next") }
                                .buttonStyle(PrimaryButtonStyle())
                                .disabled(!next.enabled)
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 8)
        .padding(.top, 6)
        .padding(.bottom, 4)
        .onChange(of: screen.turn) { slot = 0 }
        .gameSheet(isPresented: $chatting, title: store.tr("Чат", "Chat"), width: 600) {
            if let chat = screen.chat {
                ChatPanel(chat: chat)
            }
        }
    }

    private func pickName(_ i: Int) -> String {
        i == -1 ? screen.random : presets.indices.contains(i) ? presets[i].name : "—"
    }

    /// Both players in one panel, the one choosing lit.
    private var players: some View {
        VStack(spacing: 2) {
            ForEach(0..<min(2, screen.players.count), id: \.self) { k in
                playerRow(k)
            }
        }
        .padding(6)
        .panel()
    }

    private func playerRow(_ k: Int) -> some View {
        let p = screen.players[k]
        let active = k == screen.turn
        return VStack(alignment: .leading, spacing: 2) {
            Text(screen.player_names[safe: k] ?? "")
                .font(Theme.heading(16))
                .foregroundStyle(active ? Theme.gold : Theme.textDim)
            HStack(spacing: 6) {
                ForEach(Array(p.picks.enumerated()), id: \.offset) { s, pick in
                    Button {
                        if active { slot = s }
                    } label: {
                        HStack(spacing: 6) {
                            portrait(pick, side: k, small: true)
                                .frame(width: 40, height: 40)
                            if !screen.triple {
                                Text(pickName(pick))
                                    .font(Theme.body(15, weight: .medium))
                                    .foregroundStyle(Theme.text)
                                    .lineLimit(2)
                                    .minimumScaleFactor(0.7)
                                Spacer(minLength: 0)
                            }
                        }
                        .padding(3)
                        .background(RoundedRectangle(cornerRadius: 8)
                            .strokeBorder(active && screen.triple && s == slot ? Theme.gold : .clear, lineWidth: 1.5))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(active ? Theme.rowSelected : .clear))
        .opacity(p.shown || active ? 1 : 0.6)
    }

    @ViewBuilder private func portrait(_ pick: Int, side: Int, small: Bool) -> some View {
        if presets.indices.contains(pick) {
            let preset = presets[pick]
            GameImage(ref: small ? preset.round : side == 0 ? preset.left : preset.right, placeholder: "person.fill")
        } else {
            Image(systemName: pick == -1 ? "questionmark.circle" : "person.crop.circle.dashed")
                .resizable().scaledToFit().foregroundStyle(Theme.goldDim).padding(8)
        }
    }

    private var settings: some View {
        VStack(spacing: 4) {
            choice(screen.arena_label, screen.arenas, screen.arena, "arena")
            if let speeds = screen.speeds, let speed = screen.speed {
                choice(screen.speed_label, speeds, speed, "speed")
            }
            if let dynamic = screen.dynamic {
                SwitchRow(label: dynamic.label, on: dynamic.on) {
                    store.send("dynamic", ["value": $0 ? 1 : 0])
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .panel()
        .disabled(!screen.settings_enabled)
        .opacity(screen.settings_enabled ? 1 : 0.5)
    }

    private func choice(_ label: String, _ values: [String], _ index: Int, _ verb: String) -> some View {
        HStack {
            Text(label).font(Theme.body(14)).foregroundStyle(Theme.textDim).lineLimit(1)
            Spacer(minLength: 6)
            Menu {
                ForEach(Array(values.enumerated()), id: \.offset) { i, v in
                    Button {
                        store.send(verb, ["index": i])
                    } label: {
                        if i == index { Label(v, systemImage: "checkmark") } else { Text(v) }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(values[safe: index] ?? "—").lineLimit(1).minimumScaleFactor(0.7)
                    DropIcon()
                }
                .font(Theme.body(15, weight: .medium))
                .foregroundStyle(Theme.text)
                .frame(minHeight: Theme.touch)
            }
            .menuOrder(.fixed)
        }
    }

    /// The presets, the current player's pick lit; the random one first.
    private var grid: some View {
        let picked = current?.picks[safe: screen.triple ? slot : 0] ?? -2
        let taken = Set((current?.picks ?? []).enumerated().filter { $0.offset != slot && $0.element >= 0 }.map(\.element))
        return ScrollView {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 5), spacing: 6) {
                cell(-1, title: screen.random, picked: picked == -1, taken: false) {
                    Image(systemName: "questionmark.circle").resizable().scaledToFit()
                        .foregroundStyle(Theme.gold).padding(10)
                }
                ForEach(presets) { p in
                    cell(p.id, title: p.name, picked: picked == p.id, taken: screen.triple && taken.contains(p.id)) {
                        GameImage(ref: p.round, placeholder: "person.fill")
                    }
                }
            }
            .padding(8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .panel()
    }

    private func cell<Icon: View>(_ i: Int, title: String, picked: Bool, taken: Bool,
                                  @ViewBuilder icon: () -> Icon) -> some View {
        Button {
            store.send("pick", screen.triple ? ["index": i, "slot": slot] : ["index": i])
        } label: {
            VStack(spacing: 3) {
                icon().frame(height: 44)
                Text(title)
                    .font(Theme.body(12, weight: .medium))
                    .foregroundStyle(picked ? Theme.gold : Theme.text)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                    .minimumScaleFactor(0.7)
            }
            .frame(maxWidth: .infinity, minHeight: 80)
            .padding(4)
            .background(RoundedRectangle(cornerRadius: 8).fill(picked ? Theme.rowSelected : Theme.row))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(picked ? Theme.gold : .clear, lineWidth: 1.5))
            .opacity(taken ? 0.35 : 1)
        }
        .buttonStyle(.plain)
        .disabled(taken)
    }
}
