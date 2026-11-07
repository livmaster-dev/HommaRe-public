// LoadGame.swift - "load_game": the saves (the game's Load screen). Left, the
// saves newest first (name, map, date); right, the chosen one's picture,
// facts and description, Load, and Delete asked first. "save_game" (Save in
// a game) is the same with the save's name above the details: a tapped save
// fills it (to save over it), and Save writes under the name typed.

import SwiftUI

struct LoadScreen: Decodable {
    let title: String
    let saves: [Save]
    let selected: Int
    let reading: Bool
    let info: MapInfo?
    let buttons: [ScreenButton]
    /// Save only: the name in the game's field, and the field's hint.
    let name: String?
    let nameHint: String?

    enum CodingKeys: String, CodingKey {
        case title, saves, selected, reading, info, buttons, name
        case nameHint = "name_hint"
    }

    struct Save: Decodable, Hashable {
        let name: String
        let map: String
        let date: String
    }
}

extension LoadScreen: ScreenDocument {
    func view() -> some View { LoadScreenView(screen: self) }
}

struct LoadScreenView: View {
    let screen: LoadScreen
    @EnvironmentObject var store: ScreenStore
    @State private var pending: Int?
    @State private var deleting = false
    /// The name typed since the game last set it (a tapped save sets it).
    @State private var typed: String?
    @FocusState private var naming: Bool

    private var name: String { typed ?? screen.name ?? "" }

    private var selected: Int { pending ?? screen.selected }

    var body: some View {
        VStack(spacing: 8) {
            ScreenHeader(title: screen.title) { store.send("back") }
            GeometryReader { geo in
                HStack(alignment: .top, spacing: 10) {
                    ScrollViewReader { proxy in
                        ScrollView {
                            LazyVStack(spacing: 6) {
                                ForEach(Array(screen.saves.enumerated()), id: \.offset) { i, save in
                                    row(save, selected: i == selected)
                                        .id(i)
                                        .contentShape(Rectangle())
                                        .onTapGesture {
                                            // Save: the highlighted row too, to save
                                            // over it (the game copies its name).
                                            guard i != selected || screen.name != nil else { return }
                                            pending = i
                                            typed = nil
                                            store.send("select", ["index": i])
                                        }
                                }
                                if screen.reading {
                                    ProgressView().tint(Theme.gold).padding()
                                }
                            }
                            .padding(8)
                        }
                        .onAppear { if screen.selected >= 0 { proxy.scrollTo(screen.selected) } }
                    }
                    .panel()
                    .frame(width: geo.size.width * 0.5)
                    VStack(spacing: 8) {
                        if screen.name != nil {
                            nameField
                        }
                        details
                        HStack(spacing: 8) {
                            if let delete = screen.buttons[named: "delete"] {
                                Button(delete.label) { deleting = true }
                                    .buttonStyle(PlateButtonStyle())
                                    .disabled(!delete.enabled)
                                    .frame(maxWidth: 150)
                            }
                            if let load = screen.buttons[named: "load"] {
                                Button(load.label) { store.send("load") }
                                    .buttonStyle(PrimaryButtonStyle())
                                    .disabled(!load.enabled)
                            }
                            if let save = screen.buttons[named: "save"] {
                                Button(save.label) {
                                    naming = false
                                    store.send("save", ["text": name])
                                }
                                .buttonStyle(PrimaryButtonStyle())
                                .disabled(!save.enabled || name.trimmingCharacters(in: .whitespaces).isEmpty)
                            }
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 8)
        .padding(.top, 6)
        .padding(.bottom, 4)
        .onChange(of: screen.selected) {
            pending = nil
            typed = nil // the game copied the tapped save's name
        }
        .gameConfirm(isPresented: $deleting, title: store.tr("Удалить сохранение?", "Delete the save?"),
                     message: screen.saves.indices.contains(selected) ? screen.saves[selected].name : nil,
                     confirm: store.tr("Удалить", "Delete"), cancel: store.tr("Отмена", "Cancel")) {
            store.send("delete")
        }
    }

    private func row(_ save: LoadScreen.Save, selected: Bool) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(save.name)
                .font(Theme.heading(18))
                .foregroundStyle(selected ? Theme.gold : Theme.text)
                .lineLimit(1)
            HStack {
                Text(save.map).lineLimit(1)
                Spacer(minLength: 6)
                Text(save.date).lineLimit(1)
            }
            .font(Theme.body(13))
            .foregroundStyle(Theme.textDim)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8, style: .continuous)
            .fill(selected ? Theme.rowSelected : Theme.row))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
            .strokeBorder(selected ? Theme.gold : .clear, lineWidth: 1.5))
    }

    /// Save: the name to save under (the game's own field, kept in step).
    private var nameField: some View {
        TextField(screen.nameHint ?? "", text: Binding(get: { name }, set: { new in
            typed = new
            store.send("name", ["text": new], feedback: false)
        }))
        .font(Theme.body(18, weight: .medium))
        .foregroundStyle(Theme.text)
        .textInputAutocapitalization(.sentences)
        .autocorrectionDisabled()
        .focused($naming)
        .submitLabel(.done)
        .padding(.horizontal, 12)
        .frame(minHeight: Theme.touch)
        .gameField()
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let info = screen.info {
                HStack(alignment: .top, spacing: 10) {
                    MapPreview(path: info.preview)
                        .frame(width: 150, height: 112)
                    VStack(alignment: .leading, spacing: 6) {
                        Text(info.name)
                            .font(Theme.heading(19))
                            .foregroundStyle(Theme.gold)
                            .lineLimit(2)
                            .minimumScaleFactor(0.8)
                        FactsGrid(facts: info.facts)
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
}

/// A picture the game side wrote as raw pixels: a 32-bit width and height,
/// then BGRA (the game's A8R8G8B8, little-endian).
enum RawPicture {
    static func image(atPath path: String) -> PlatformImage? {
        guard let data = FileManager.default.contents(atPath: path), data.count > 8 else { return nil }
        let w = Int(data.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt32.self) })
        let h = Int(data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self) })
        guard w > 0, h > 0, data.count >= 8 + w * h * 4,
              let provider = CGDataProvider(data: data.subdata(in: 8..<(8 + w * h * 4)) as CFData),
              let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue
                                   | CGImageAlphaInfo.noneSkipFirst.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        return PlatformImage.of(cg)
    }
}
