// MainMenu.swift - "main_menu": the game's main menu and its panels (the main
// buttons, Single player, Multiplayer, Hot seat's modes, the campaigns and a
// campaign's missions), and "profile_name": the first profile's name.
//
// The 3D scene stays the game's. The panel's buttons stand in a column on the
// right, as the game has them; Single player and the campaigns show the
// profile and the difficulty at the bottom left; the profile list is a sheet;
// the campaigns are cards, a campaign's missions a list with the chosen one's
// story and a Start button (the game starts a mission on the first tap; here
// the tap chooses and Start plays).

import SwiftUI

struct MainMenuScreen: Decodable {
    let panel: String
    let title: String
    let version: String
    let buttons: [ScreenButton]
    let profile: ProfileChip?
    let difficulty: DifficultyChoice?
    let profiles: ProfileList?
    let campaigns: [CampaignCard]?
    let campaign: String?
    let missions: [MissionRow]?

    struct ProfileChip: Decodable { let name: String; let change: String }
    struct DifficultyChoice: Decodable { let label: String; let index: Int; let values: [String] }
    struct ProfileList: Decodable {
        let names: [String]
        let current: Int
        let title: String
        let new: String
    }
    struct CampaignCard: Decodable, Identifiable {
        var id: String { name }
        let name: String
        let description: String
        let enabled: Bool
        let done: Int
        let missions: Int
        /// Its first mission's loading picture (the paks' path), when it has one.
        let picture: String?
    }
    struct MissionRow: Decodable, Identifiable {
        var id: String { name }
        let name: String
        let description: String
        let enabled: Bool
        let done: Bool
        /// The mission's loading picture (the paks' path); many have none.
        let picture: String?
    }
}

extension MainMenuScreen: ScreenDocument {
    func view() -> some View { MainMenuView(screen: self) }
}

struct MainMenuView: View {
    let screen: MainMenuScreen
    @EnvironmentObject var store: ScreenStore

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            switch screen.panel {
            case "campaigns", "user_campaigns":
                CampaignsPanel(screen: screen)
            case "missions":
                MissionsPanel(screen: screen)
            default:
                HStack(alignment: .bottom) {
                    Spacer()
                    menuColumn
                }
            }
            // The version line stays the game's (it is drawn from another root).
            if screen.profile != nil, screen.panel == "single" {
                SetupChip(screen: screen)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .gameSheet(isPresented: Binding(get: { screen.profiles != nil },
                                        set: { if !$0 && screen.profiles != nil { store.send("profiles", ["open": 0]) } }),
                   title: screen.profiles?.title ?? "", width: 500) {
            if let list = screen.profiles {
                ProfileSheet(list: list)
            }
        }
    }

    private var menuColumn: some View {
        VStack(spacing: 8) {
            ForEach(screen.buttons, id: \.name) { b in
                Button(b.label) { store.send("press", ["name": b.name]) }
                    .buttonStyle(MenuButtonStyle(back: b.name == "back"))
                    .disabled(!b.enabled)
            }
        }
        .frame(width: 285)
        .ornate(title: screen.title)
    }
}

/// A main-menu button: a wide plate with a heading's type; Back plainer.
struct MenuButtonStyle: ButtonStyle {
    var back = false
    /// The chosen one of a row of tabs: the game's gold button.
    var selected = false
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(back ? Theme.body(17, weight: .medium) : Theme.heading(20))
            .foregroundStyle(selected ? Theme.goldText : back ? Theme.khakiText.opacity(0.8) : Theme.khakiText)
            .frame(maxWidth: .infinity, minHeight: back ? Theme.touch : 47.5)
            .opacity(enabled ? 1 : 0.6)
            .background {
                // The game's khaki button (gold when chosen).
                GameNine(spec: selected ? Kit.gold(pressed: configuration.isPressed)
                                        : Kit.khaki(pressed: configuration.isPressed)) {
                    RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                        .fill(Theme.row.opacity(configuration.isPressed ? 0.6 : 1))
                        .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous)
                            .strokeBorder(back ? Theme.goldDim.opacity(0.5) : Theme.goldDim, lineWidth: 1))
                }
                .gameDisabled(!enabled)
            }
    }
}

/// The profile's name with Change, and the difficulty.
struct SetupChip: View {
    let screen: MainMenuScreen
    /// Narrower beside a campaign's Start.
    var width: CGFloat = 330
    @EnvironmentObject var store: ScreenStore

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let profile = screen.profile {
                HStack(spacing: 10) {
                    Image(systemName: "person.crop.circle").foregroundStyle(Theme.gold)
                    Text(profile.name.isEmpty ? "—" : profile.name)
                        .font(Theme.heading(19))
                        .foregroundStyle(Theme.text)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    if screen.panel == "single" {
                        Button(profile.change) { store.send("profiles", ["open": 1]) }
                            .buttonStyle(PlateButtonStyle())
                    }
                }
            }
            if let d = screen.difficulty {
                HStack(spacing: 10) {
                    Text(d.label).font(Theme.body(15)).foregroundStyle(Theme.textDim)
                    Spacer(minLength: 8)
                    Menu {
                        ForEach(Array(d.values.enumerated()), id: \.offset) { i, name in
                            Button {
                                store.send("difficulty", ["index": i])
                            } label: {
                                if i == d.index { Label(name, systemImage: "checkmark") } else { Text(name) }
                            }
                        }
                    } label: {
                        HStack(spacing: 6) {
                            Text(d.values.indices.contains(d.index) ? d.values[d.index] : "")
                            DropIcon()
                        }
                        .font(Theme.body(16, weight: .medium))
                        .foregroundStyle(Theme.text)
                        .padding(.horizontal, 12)
                        .frame(minHeight: Theme.touch)
                        .gameField()
                    }
                }
            }
        }
        .padding(12)
        .frame(width: width)
        .panel()
    }
}

/// The profiles: choose one, delete one (asked first), or make a new one.
struct ProfileSheet: View {
    let list: MainMenuScreen.ProfileList
    @EnvironmentObject var store: ScreenStore
    @State private var name = ""
    @State private var deleting: Int?

    private var valid: Bool {
        let t = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return (2...10).contains(t.count)
            && t.unicodeScalars.allSatisfy {
                CharacterSet.letters.contains($0) || CharacterSet.decimalDigits.contains($0) || $0 == " "
            }
    }

    var body: some View {
        VStack(spacing: 8) {
            ScrollView {
                VStack(spacing: 6) {
                    ForEach(Array(list.names.enumerated()), id: \.element) { i, n in
                        HStack(spacing: 8) {
                            Button {
                                store.send("profile_select", ["index": i])
                            } label: {
                                Text(n)
                                    .font(Theme.body(18))
                                    .foregroundStyle(i == list.current ? Theme.gold : Theme.text)
                                    .padding(.horizontal, 12)
                                    .frame(maxWidth: .infinity, minHeight: Theme.touch, alignment: .leading)
                                    .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .background { if i == list.current { Color.clear.gameField() } }
                            if list.names.count > 1 {
                                Button { deleting = i } label: { Image(systemName: "trash") }
                                    .buttonStyle(PlateButtonStyle())
                                    .frame(width: Theme.touch + 8)
                            }
                        }
                    }
                }
            }
            .frame(maxHeight: 190)
            Text(list.new).font(Theme.body(14)).foregroundStyle(Theme.textDim)
                .frame(maxWidth: .infinity, alignment: .leading)
            HStack(spacing: 8) {
                TextField(store.tr("Имя", "Name"), text: $name)
                    .font(Theme.body(18))
                    .foregroundStyle(Theme.text)
                    .textInputAutocapitalization(.words)
                    .autocorrectionDisabled()
                    .padding(.horizontal, 12)
                    .frame(minHeight: Theme.touch)
                    .gameField()
                Button(store.tr("Создать", "Create")) {
                    store.send("profile_create", ["text": name.trimmingCharacters(in: .whitespacesAndNewlines)])
                    name = ""
                }
                .buttonStyle(PlateButtonStyle())
                .disabled(!valid)
            }
        }
        .gameConfirm(isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                     title: deleting.flatMap { list.names.indices.contains($0) ? list.names[$0] : nil }
                         .map { String(format: store.tr("Удалить профиль «%@»?", "Delete the profile %@?"), $0) } ?? "",
                     message: store.tr("Сохранённые игры и успехи в кампаниях этого профиля пропадут.",
                                       "Its saved games and campaign progress go with it."),
                     confirm: store.tr("Удалить", "Delete"), cancel: store.tr("Отмена", "Cancel")) {
            if let i = deleting { store.send("profile_delete", ["index": i]) }
        }
    }
}

/// The campaigns as cards; a locked one is dimmed.
struct CampaignsPanel: View {
    let screen: MainMenuScreen
    @EnvironmentObject var store: ScreenStore

    var body: some View {
        VStack(spacing: 8) {
            ScreenHeader(title: screen.title) { store.send("press", ["name": "back"]) }
            HStack(alignment: .top, spacing: 10) {
                ScrollView {
                    LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10),
                                        GridItem(.flexible())], spacing: 10) {
                        ForEach(Array((screen.campaigns ?? []).enumerated()), id: \.offset) { i, c in
                            Button {
                                store.send("campaign", ["index": i])
                            } label: {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(c.name)
                                        .font(Theme.heading(19))
                                        .foregroundStyle(c.enabled ? Theme.gold : Theme.textDim)
                                        .lineLimit(1)
                                        .minimumScaleFactor(0.6)
                                    Text(c.description)
                                        .font(Theme.body(12))
                                        .foregroundStyle(Theme.textDim)
                                        .lineLimit(3)
                                    Spacer(minLength: 0)
                                    HStack {
                                        if !c.enabled { Image(systemName: "lock.fill") }
                                        Spacer()
                                        Text("\(c.done)/\(c.missions)")
                                    }
                                    .font(Theme.body(12, weight: .medium))
                                    .foregroundStyle(Theme.textDim)
                                }
                                .padding(10)
                                .frame(maxWidth: .infinity, minHeight: 118, alignment: .topLeading)
                                .background {
                                    // Its first mission's loading picture, dimmed under the words.
                                    ZStack {
                                        Theme.row
                                        GameBackdrop(ref: c.picture).opacity(0.6)
                                        LinearGradient(colors: [Theme.row.opacity(0.95), Theme.row.opacity(0.35)],
                                                       startPoint: .leading, endPoint: .trailing)
                                    }
                                    .clipShape(RoundedRectangle(cornerRadius: 3))
                                    .padding(3)
                                }
                                .overlay {
                                    // The game's picture frame (QuestName), as plan C's cards.
                                    GameNine(spec: Kit.pictureFrame) {
                                        RoundedRectangle(cornerRadius: Theme.radius)
                                            .strokeBorder(c.enabled ? Theme.goldDim : Theme.goldDim.opacity(0.3))
                                    }
                                }
                                // A locked campaign: greyed, as the game shows it.
                                .saturation(c.enabled ? 1 : 0.2)
                                .opacity(c.enabled ? 1 : 0.8)
                            }
                            .buttonStyle(.plain)
                            .disabled(!c.enabled)
                        }
                    }
                    .padding(8)
                }
                .panel()
                SetupChip(screen: screen)
            }
        }
    }
}

/// A campaign's missions: choose one, read its story, play it.
struct MissionsPanel: View {
    let screen: MainMenuScreen
    @EnvironmentObject var store: ScreenStore
    @State private var chosen: Int?

    private var missions: [MainMenuScreen.MissionRow] { screen.missions ?? [] }
    /// The chosen mission, else the first one still to play.
    private var current: Int? {
        chosen ?? missions.firstIndex { $0.enabled && !$0.done } ?? missions.lastIndex { $0.enabled }
    }

    var body: some View {
        VStack(spacing: 8) {
            ScreenHeader(title: screen.campaign ?? screen.title) { store.send("press", ["name": "back"]) }
            HStack(alignment: .top, spacing: 10) {
                ScrollView {
                    VStack(spacing: 6) {
                        ForEach(Array(missions.enumerated()), id: \.offset) { i, m in
                            Button {
                                chosen = i
                            } label: {
                                HStack {
                                    Text(m.name)
                                        .font(Theme.heading(18))
                                        .foregroundStyle(i == current ? Theme.gold : Theme.text)
                                        .lineLimit(1)
                                    Spacer()
                                    if m.done {
                                        Image(systemName: "checkmark.seal.fill").foregroundStyle(Theme.gold)
                                    } else if !m.enabled {
                                        Image(systemName: "lock.fill").foregroundStyle(Theme.textDim)
                                    }
                                }
                                .padding(.horizontal, 12)
                                .frame(minHeight: 48)
                                .background(RoundedRectangle(cornerRadius: 8)
                                    .fill(i == current ? Theme.rowSelected : Theme.row))
                                .overlay(RoundedRectangle(cornerRadius: 8)
                                    .strokeBorder(i == current ? Theme.gold : .clear, lineWidth: 1.5))
                            }
                            .buttonStyle(.plain)
                            .disabled(!m.enabled)
                            .opacity(m.enabled ? 1 : 0.5)
                        }
                    }
                    .padding(8)
                }
                .frame(width: 300)
                .panel()
                VStack(spacing: 8) {
                    ScrollView {
                        if let i = current, missions.indices.contains(i) {
                            VStack(alignment: .leading, spacing: 6) {
                                // Its own loading picture, else the campaign's first.
                                GameBanner(ref: missions[i].picture, fallback: missions.first?.picture, height: 130)
                                Text(missions[i].name)
                                    .font(Theme.heading(20))
                                    .foregroundStyle(Theme.gold)
                                Text(missions[i].description)
                                    .font(Theme.body(14))
                                    .foregroundStyle(Theme.text)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(10)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .panel()
                    HStack(spacing: 8) {
                        SetupChip(screen: screen, width: 250)
                        Button(store.tr("Начать", "Play")) {
                            if let i = current { store.send("mission", ["index": i]) }
                        }
                        .buttonStyle(PrimaryButtonStyle())
                        .lineLimit(1)
                        .disabled(current == nil)
                    }
                }
            }
        }
    }
}

// MARK: - profile_name

struct ProfileNameScreen: Decodable {
    let title: String
    let text: String
    let hint: String
    let ok: String
    /// A box that may be closed unanswered (the LAN name); none for the first profile.
    let cancel: String?
    /// The longest name the game takes, when it is not a profile's 2..10.
    let max: Int?
    /// A password: typed hidden.
    let secret: Bool?
}

extension ProfileNameScreen: ScreenDocument {
    func view() -> some View { ProfileNameView(screen: self) }
}

/// The first profile's name: asked once, at the first start.
struct ProfileNameView: View {
    let screen: ProfileNameScreen
    @EnvironmentObject var store: ScreenStore
    @State private var name: String?
    @FocusState private var focused: Bool

    private var text: String { name ?? screen.text }
    private var valid: Bool {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let max = screen.max { return (1...max).contains(t.count) }
        return (2...10).contains(t.count)
            && t.unicodeScalars.allSatisfy {
                CharacterSet.letters.contains($0) || CharacterSet.decimalDigits.contains($0) || $0 == " "
            }
    }

    var body: some View {
        VStack(spacing: 12) {
            Text(screen.title).font(Theme.heading(24)).foregroundStyle(Theme.gold)
            Group {
                if screen.secret == true {
                    SecureField("", text: Binding(get: { text }, set: { name = $0 }))
                } else {
                    TextField("", text: Binding(get: { text }, set: { name = $0 }))
                }
            }
                .font(Theme.body(20, weight: .medium))
                .foregroundStyle(Theme.text)
                .padding(.horizontal, 14)
                .frame(minHeight: 50)
                .gameField()
                .textInputAutocapitalization(.words)
                .autocorrectionDisabled()
                .focused($focused)
                .submitLabel(.done)
                .onSubmit(accept)
            if !screen.hint.isEmpty {
                Text(screen.hint).font(Theme.body(13)).foregroundStyle(Theme.textDim)
            }
            HStack(spacing: 10) {
                if let cancel = screen.cancel {
                    Button { store.send("cancel") } label: { Text(cancel).frame(maxWidth: .infinity) }
                        .buttonStyle(PlateButtonStyle())
                }
                Button(screen.ok, action: accept)
                    .buttonStyle(PrimaryButtonStyle())
                    .disabled(!valid)
            }
        }
        .padding(16)
        .frame(width: 420)
        .panel()
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .padding(.top, 20)
    }

    private func accept() {
        guard valid else { return }
        store.send("accept", ["text": text.trimmingCharacters(in: .whitespacesAndNewlines)])
    }
}
