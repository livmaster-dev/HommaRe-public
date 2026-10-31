// CustomGamePlayers.swift - "custom_players": the players before a custom
// game starts (the game's CustomGameWait screen).
//
// Left, a card per player: its colour and name, then its five settings as
// plates (who plays, colour, faction, hero, starting bonus). A tap on a plate
// lists the values when the game gave them and sets the chosen one; without a
// list it steps to the next value, as the game's right arrow does. A held
// plate offers the previous and the next value. Right, the map and the Start
// button.
//
// The game only steps through its values (next and previous); "set" is the
// game side's to reach, through those same steps (native/native_screens.c).

import SwiftUI

struct PlayersScreenView: View {
    let screen: PlayersScreen
    @EnvironmentObject var store: ScreenStore
    @State private var picking: Pick?
    /// Hot seat: the player whose name is being typed.
    @State private var naming: PlayerRow?
    /// A network game's right column: 0 the map, 1 the chat.
    @State private var side = 0

    struct Pick: Identifiable {
        let slot: Int
        let cell: PlayerCell
        var id: Int { slot * 16 + cell.field }
    }

    var body: some View {
        VStack(spacing: 8) {
            ScreenHeader(title: screen.title) { store.send("press", ["name": "back"]) }
            GeometryReader { geo in
                HStack(alignment: .top, spacing: 10) {
                    ScrollView {
                        LazyVStack(spacing: 8) {
                            ForEach(screen.players) { player in
                                PlayerCardView(player: player, rename: { naming = player }) { cell in
                                    tapped(player, cell)
                                } step: { cell, dir in
                                    store.send("rotate", ["slot": player.slot, "field": cell.field, "dir": dir])
                                }
                            }
                        }
                        .padding(8)
                    }
                    .panel()
                    .frame(width: geo.size.width * 0.62)
                    VStack(spacing: 8) {
                        mapPanel
                        if let start = screen.buttons[named: "start"] {
                            Button(start.label) { store.send("press", ["name": "start"]) }
                                .buttonStyle(PrimaryButtonStyle())
                                .disabled(!start.enabled)
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 8)
        .padding(.top, 6)
        .padding(.bottom, 4)
        .overlay { if let busy = screen.busy { BusyOverlay(text: busy) } }
        .gameSheet(item: $picking, title: { $0.cell.title }, width: 460) { pick in
            ValuePicker(cell: pick.cell) { index in
                store.send("set", ["slot": pick.slot, "field": pick.cell.field, "index": index])
                picking = nil
            }
        }
        .gameSheet(item: $naming, title: { _ in store.tr("Имя игрока", "Player name") }, width: 460) { player in
            NameSheet(name: player.name ?? "") { name in
                store.send("name", ["slot": player.slot, "text": name])
                naming = nil
            }
        }
    }

    private func tapped(_ player: PlayerRow, _ cell: PlayerCell) {
        if let values = cell.values, values.count > 1, cell.index != nil {
            picking = Pick(slot: player.slot, cell: cell)
        } else {
            store.send("rotate", ["slot": player.slot, "field": cell.field, "dir": 1])
        }
    }

    @ViewBuilder private var mapPanel: some View {
        if let chat = screen.chat {
            VStack(spacing: 6) {
                Picker("", selection: $side) {
                    Text(store.tr("Карта", "Map")).tag(0)
                    Text(store.tr("Чат", "Chat")).tag(1)
                }
                .pickerStyle(.segmented)
                if side == 1 {
                    ChatPanel(chat: chat, compact: true)
                } else {
                    mapInfo
                }
            }
            .padding(8)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .panel()
        } else {
            mapInfo
                .padding(10)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .panel()
        }
    }

    private var mapInfo: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let info = screen.info {
                MapPreview(path: info.preview)
                    .frame(maxWidth: .infinity)
                    .frame(height: 96)
                Text(info.name)
                    .font(Theme.heading(20))
                    .foregroundStyle(Theme.gold)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                ScrollView {
                    FactsGrid(facts: info.facts)
                }
            }
            Spacer(minLength: 0)
        }
    }
}

/// A player: its colour and name over its five settings.
struct PlayerCardView: View {
    let player: PlayerRow
    let rename: () -> Void
    let tap: (PlayerCell) -> Void
    let step: (PlayerCell, Int) -> Void
    @EnvironmentObject var store: ScreenStore

    private var colour: Color {
        Theme.players.indices.contains(player.colour) ? Theme.players[player.colour] : Theme.gold
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Circle().fill(colour).frame(width: 12, height: 12)
                Text(player.label)
                    .font(Theme.heading(17))
                    .foregroundStyle(Theme.text)
                    .lineLimit(1)
                if let name = player.name {
                    Spacer(minLength: 8)
                    Button(action: rename) {
                        Label(name.isEmpty ? store.tr("Имя", "Name") : name, systemImage: "pencil")
                            .font(Theme.body(15, weight: .medium))
                            .foregroundStyle(Theme.gold)
                            .lineLimit(1)
                            .frame(minHeight: 32)
                    }
                    .buttonStyle(.plain)
                }
            }
            HStack(spacing: 6) {
                ForEach(player.cells) { cell in
                    Button {
                        tap(cell)
                    } label: {
                        CellLabel(cell: cell, colour: cell.field == 2 ? colour : nil)
                    }
                    .buttonStyle(PlateButtonStyle())
                    .disabled(!cell.enabled)
                    .contextMenu {
                        if cell.enabled {
                            Button(store.tr("Предыдущий", "Previous")) { step(cell, 0) }
                            Button(store.tr("Следующий", "Next")) { step(cell, 1) }
                        }
                    }
                }
            }
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.well)
        )
        .overlay(alignment: .leading) {
            // The player's colour down the card's edge.
            RoundedRectangle(cornerRadius: 2).fill(colour).frame(width: 4).padding(.vertical, 8)
        }
    }
}

/// A setting's plate: the game's picture for its value over the value's
/// name (the title only where there is no picture). The faction shows its
/// crest alone, the hero its portrait and name.
struct CellLabel: View {
    let cell: PlayerCell
    let colour: Color?
    var body: some View {
        if cell.field == 3, let icon = cell.icon, !icon.isEmpty {
            GameImage(ref: icon, placeholder: "questionmark")
                .frame(width: 46, height: 46)
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .frame(maxWidth: .infinity, minHeight: 64)
                .padding(.vertical, 2)
                .accessibilityLabel(cell.value)
        } else if cell.field == 4 {
            // The hero: portrait and name, no specialisation; the name on one
            // line, made smaller to stay inside the plate's border.
            VStack(spacing: 2) {
                if let icon = cell.icon, !icon.isEmpty {
                    GameImage(ref: icon, placeholder: "questionmark")
                        .frame(width: 34, height: 34)
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                }
                Text(cell.value)
                    .font(Theme.body(cell.icon?.isEmpty == false ? 13 : 15, weight: .medium))
                    .foregroundStyle(Theme.text)
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
            }
            .padding(.horizontal, 6)
            .frame(maxWidth: .infinity, minHeight: 64)
            .padding(.vertical, 2)
        } else {
            plate
        }
    }

    private var plate: some View {
        VStack(spacing: 2) {
            if let icon = cell.icon, !icon.isEmpty {
                // Smaller with a line under the value (a hero's specialisation),
                // so every plate keeps one height.
                GameImage(ref: icon, placeholder: "questionmark")
                    .frame(width: cell.detail == nil ? 34 : 26, height: cell.detail == nil ? 34 : 26)
                    .clipShape(RoundedRectangle(cornerRadius: 5))
            } else if cell.field == 1 {
                Image(systemName: "person.fill")
                    .font(.system(size: 20))
                    .foregroundStyle(Theme.gold)
                    .frame(height: 34)
            } else {
                Text(cell.title)
                    .font(Theme.body(11))
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(1)
                if let colour {
                    RoundedRectangle(cornerRadius: 3).fill(colour).frame(width: 14, height: 14)
                }
            }
            Text(cell.value)
                .font(Theme.body(13, weight: .medium))
                .foregroundStyle(Theme.text)
                // One word ("Willkürlich") shrinks rather than breaking in two.
                .lineLimit(cell.detail == nil && cell.value.contains(" ") ? 2 : 1)
                .multilineTextAlignment(.center)
                .minimumScaleFactor(cell.value.contains(" ") ? 0.7 : 0.55)
            if let detail = cell.detail, !detail.isEmpty {
                Text(detail)
                    .font(Theme.body(10))
                    .foregroundStyle(Theme.textDim)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 64)
        .padding(.vertical, 2)
    }
}

/// Every value of a setting, the current one in the game's field.
struct ValuePicker: View {
    let cell: PlayerCell
    let choose: (Int) -> Void

    var body: some View {
        ScrollView {
            VStack(spacing: 4) {
                ForEach(Array((cell.values ?? []).enumerated()), id: \.offset) { i, name in
                    Button {
                        choose(i)
                    } label: {
                        HStack(spacing: 10) {
                            if let icon = cell.icons?[safe: i] {
                                GameImage(ref: icon, placeholder: "questionmark")
                                    .frame(width: 36, height: 36)
                                    .clipShape(RoundedRectangle(cornerRadius: 5))
                            }
                            Text(name)
                                .font(Theme.body(18))
                                .foregroundStyle(i == cell.index ? Theme.gold : Theme.text)
                            Spacer()
                        }
                        .padding(.horizontal, 10)
                        .frame(minHeight: Theme.touch + 4)
                        .contentShape(Rectangle())
                        .background { if i == cell.index { Color.clear.gameField() } }
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .frame(maxHeight: 280)
    }
}

/// A hot seat player's name: typed, checked by the game's rule (2 to 10
/// letters, digits or spaces) so its "wrong name" box never comes.
struct NameSheet: View {
    @State var name: String
    let done: (String) -> Void
    @EnvironmentObject var store: ScreenStore

    private var valid: Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return (2...10).contains(trimmed.count)
            && trimmed.unicodeScalars.allSatisfy {
                CharacterSet.letters.contains($0) || CharacterSet.decimalDigits.contains($0) || $0 == " "
            }
    }

    var body: some View {
        VStack(spacing: 10) {
            TextField(store.tr("Имя", "Name"), text: $name)
                .font(Theme.body(20))
                .foregroundStyle(Theme.text)
                .textInputAutocapitalization(.words)
                .autocorrectionDisabled()
                .padding(.horizontal, 12)
                .frame(minHeight: 48)
                .gameField()
            Text(store.tr("От 2 до 10 букв, цифр или пробелов", "2 to 10 letters, digits or spaces"))
                .font(Theme.body(14))
                .foregroundStyle(Theme.textDim)
            Button(store.tr("Готово", "Done")) { done(name.trimmingCharacters(in: .whitespacesAndNewlines)) }
                .buttonStyle(PrimaryButtonStyle())
                .disabled(!valid)
        }
    }
}
