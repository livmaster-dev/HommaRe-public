// CustomGameCreate.swift - "custom_create": Single player > Custom game, the
// map and the game's settings (the game's CustomGameCreate screen).
//
// Left, the maps: the game's four sort keys as chips over a list of two-line
// rows (the name, then goal, players and size). Right, the chosen map's
// picture, facts and description, the settings (the turn's time limit and the
// difficulty) and the buttons. A tap on a row selects it at once here and
// asks the game to select it; the game's next document confirms it.

import SwiftUI

struct CreateScreenView: View {
    let screen: CreateScreen
    @EnvironmentObject var store: ScreenStore
    /// The row tapped last, until the game's document shows it chosen.
    @State private var pending: Int?
    /// A network game's settings sheet.
    @State private var settings = false

    private var selected: Int { pending ?? screen.selected }

    var body: some View {
        VStack(spacing: 8) {
            ScreenHeader(title: screen.title) { store.send("press", ["name": "back"]) }
            GeometryReader { geo in
                // A short screen (an iPhone smaller than the Max ones): a
                // smaller picture and tighter settings, so the description shows.
                let compact = geo.size.height < 350
                HStack(alignment: .top, spacing: 10) {
                    mapList
                        .frame(width: geo.size.width * 0.55)
                    VStack(spacing: 8) {
                        infoPanel(compact: compact)
                        if let fields = screen.fields, !fields.isEmpty {
                            // A network game: its settings in a sheet, the panel a button.
                            Button {
                                settings = true
                            } label: {
                                Label(store.tr("Параметры игры", "Game settings"), systemImage: "slider.horizontal.3")
                                    .frame(maxWidth: .infinity)
                            }
                            .buttonStyle(PlateButtonStyle())
                        } else {
                            optionsPanel(compact: compact)
                        }
                        buttonRow
                    }
                }
            }
        }
        .padding(.horizontal, 8)
        .padding(.top, 6)
        .padding(.bottom, 4)
        .onChange(of: screen.selected) { pending = nil }
        .overlay { if let busy = screen.busy { BusyOverlay(text: busy) } }
        .gameSheet(isPresented: $settings, title: store.tr("Параметры игры", "Game settings"), width: 600) {
            ScrollView {
                VStack(spacing: 6) {
                    ForEach(screen.fields ?? []) { f in FormFieldRow(field: f) }
                    optionsPanel(compact: false)
                }
            }
        }
    }

    // MARK: The maps

    private var mapList: some View {
        VStack(spacing: 8) {
            HStack(spacing: 6) {
                ForEach(Array(screen.columns.enumerated()), id: \.offset) { i, name in
                    Button {
                        store.send("sort", ["key": i])
                    } label: {
                        HStack(spacing: 3) {
                            Text(name).lineLimit(1).minimumScaleFactor(0.75)
                            if screen.sort == i {
                                Image(systemName: screen.descending ? "chevron.down" : "chevron.up")
                                    .font(.system(size: 11, weight: .bold))
                            }
                        }
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(PlateButtonStyle(selected: screen.sort == i))
                }
            }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 6) {
                        ForEach(Array(screen.maps.enumerated()), id: \.offset) { i, map in
                            MapRowView(map: map, selected: i == selected)
                                .id(i)
                                .contentShape(Rectangle())
                                .onTapGesture {
                                    guard i != selected else { return }
                                    pending = i
                                    store.send("select", ["index": i])
                                }
                        }
                    }
                    .padding(.vertical, 2)
                }
                .onAppear {
                    if screen.selected >= 0 { proxy.scrollTo(screen.selected, anchor: .center) }
                }
                // A new sort, or the same one turned round, moves the chosen
                // map: keep it in view.
                .onChange(of: screen.sort) {
                    if screen.selected >= 0 { proxy.scrollTo(screen.selected, anchor: .center) }
                }
                .onChange(of: screen.descending) {
                    if screen.selected >= 0 { proxy.scrollTo(screen.selected, anchor: .center) }
                }
            }
            // The list's filters (multiplayer maps) sit with the list.
            ForEach(screen.toggles ?? []) { toggle in
                SwitchRow(label: toggle.label, on: toggle.on, size: 14) { _ in
                    store.send("toggle", ["id": toggle.id])
                }
                .padding(.horizontal, 6)
            }
        }
        .padding(8)
        .panel()
    }

    // MARK: The chosen map

    private func infoPanel(compact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if let info = screen.info {
                if info.preview != nil {
                    HStack(alignment: .top, spacing: 10) {
                        MapPreview(path: info.preview)
                            .frame(width: compact ? 76 : 104, height: compact ? 76 : 104)
                        VStack(alignment: .leading, spacing: compact ? 2 : 6) {
                            mapName(info)
                            FactsGrid(facts: info.facts, size: compact ? 12.5 : 14, spacing: compact ? 0 : 3)
                        }
                    }
                } else {
                    // No picture: the facts two to a line, leaving the
                    // description room on a phone's short screen.
                    mapName(info)
                    LazyVGrid(columns: [GridItem(.flexible(), spacing: 12, alignment: .leading),
                                        GridItem(.flexible(), alignment: .leading)],
                              alignment: .leading, spacing: 3) {
                        ForEach(Array(info.facts.enumerated()), id: \.offset) { _, f in
                            (Text(f.label + " ").foregroundStyle(Theme.textDim)
                                + Text(f.value).foregroundStyle(Theme.text))
                                .font(Theme.body(14))
                                .lineLimit(1)
                                .minimumScaleFactor(0.75)
                        }
                    }
                }
                if let text = info.description, !text.isEmpty {
                    ScrollView {
                        Text(text)
                            .font(Theme.body(14))
                            .foregroundStyle(Theme.text)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            } else {
                Spacer()
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .panel()
    }

    private func mapName(_ info: MapInfo) -> some View {
        Text(info.name)
            .font(Theme.heading(20))
            .foregroundStyle(Theme.gold)
            .lineLimit(2)
            .minimumScaleFactor(0.8)
    }

    // MARK: Settings and buttons

    private func optionsPanel(compact: Bool) -> some View {
        VStack(spacing: compact ? 0 : 4) {
            ForEach(screen.options) { option in
                OptionRowView(option: option, height: compact ? 38 : Theme.touch) { index in
                    store.send("option", ["id": option.id, "index": index])
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .panel()
    }

    private var buttonRow: some View {
        HStack(spacing: 8) {
            if let load = screen.buttons[named: "load"] {
                Button(load.label) { store.send("press", ["name": "load"]) }
                    .buttonStyle(PlateButtonStyle())
                    .disabled(!load.enabled)
                    .frame(maxWidth: 150)
            }
            // The expansions' random map generator (the game's own screen).
            if let rmg = screen.buttons[named: "rmg"] {
                Button(rmg.label) { store.send("press", ["name": "rmg"]) }
                    .buttonStyle(PlateButtonStyle())
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    .disabled(!rmg.enabled)
                    .frame(maxWidth: 150)
            }
            if let create = screen.buttons[named: "create"] {
                Button(create.label) { store.send("press", ["name": "create"]) }
                    .buttonStyle(PrimaryButtonStyle())
                    .disabled(!create.enabled || selected < 0)
            }
        }
    }
}

/// A map in the list: its name, then its goal, players and size.
struct MapRowView: View {
    let map: MapRow
    let selected: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(map.name)
                .font(Theme.heading(18))
                .foregroundStyle(selected ? Theme.gold : Theme.text)
                .lineLimit(1)
            HStack(spacing: 6) {
                Text(map.goal).lineLimit(1)
                Spacer(minLength: 6)
                Text(map.players).lineLimit(1)
                // A dot of its own: the game font's middle dot sits high.
                Circle().frame(width: 3, height: 3)
                Text(map.size).lineLimit(1)
            }
            .font(Theme.body(13))
            .foregroundStyle(Theme.textDim)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(selected ? Theme.rowSelected : Theme.row)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(selected ? Theme.gold : .clear, lineWidth: 1.5)
        )
    }
}

/// A setting: its name, then its value between the arrows; with the values
/// known, a tap on the value lists them all.
struct OptionRowView: View {
    let option: ScreenOption
    var height: CGFloat = Theme.touch
    let choose: (Int) -> Void

    var body: some View {
        HStack(spacing: 6) {
            Text(option.label)
                .font(Theme.body(15))
                .foregroundStyle(Theme.textDim)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
            Spacer(minLength: 4)
            arrow("chevron.left", to: option.index - 1)
            value
                .frame(minWidth: 120)
            arrow("chevron.right", to: option.index + 1)
        }
        .frame(minHeight: height)
        .disabled(!option.enabled)
        .opacity(option.enabled ? 1 : 0.5)
    }

    @ViewBuilder private var value: some View {
        let label = Text(option.value)
            .font(Theme.body(16, weight: .medium))
            .foregroundStyle(Theme.text)
            .lineLimit(1)
            .minimumScaleFactor(0.75)
        if let values = option.values, values.count == option.count {
            Menu {
                ForEach(Array(values.enumerated()), id: \.offset) { i, name in
                    Button {
                        choose(i)
                    } label: {
                        if i == option.index { Label(name, systemImage: "checkmark") } else { Text(name) }
                    }
                }
            } label: {
                label.frame(minHeight: height)
            }
        } else {
            label
        }
    }

    private func arrow(_ symbol: String, to index: Int) -> some View {
        Button {
            choose(index)
        } label: {
            // The game's round arrow (a jog dial's), 40 pt in a finger's square.
            GameImage(ref: Kit.jog(symbol == "chevron.left" ? "Left" : "Right"), placeholder: symbol)
                .frame(width: 40, height: 40)
                .frame(width: Theme.touch, height: Theme.touch)
                .contentShape(Rectangle())
        }
        .buttonStyle(PressDim())
        .disabled(index < 0 || index >= option.count)
        .opacity(index < 0 || index >= option.count ? 0.35 : 1)
    }
}
