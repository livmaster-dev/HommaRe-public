// SkillWheel.swift - "skill_wheel": the game's skills and perks for one hero
// class as a wheel, over whatever the game shows (native/native_screens.c
// opens it from the control panel's "skill-wheel" command; the game keeps
// drawing under it). Everything shown is the install's own: the skill table
// (GameMechanics/RefTables/Skills.xdb: each perk's skill, kind and, per class,
// the perks it needs), the classes' chances to learn each skill
// (HeroClass.xdb, Tribes of the East and Hammers of Fate; the base game has
// none), the names, descriptions and icons, read from the paks the game
// mounts (GameArt), in the game's language. Nothing here knows a rule of its
// own: a class's wheel is what the tables give it.
//
// The wheel: the class's racial skill on top, its skills around the middle,
// the perks outward from their skill in rows by how deep they lie (a skill's
// own perks first, then those that need them, ...), the ultimate in the
// centre. A tap picks a skill or a perk: the panel beside the wheel says what
// it is and does, what it needs and what it opens, and the wheel draws the
// lines. Pinch or double-tap to zoom (a phone's wheel is small).
//
// The hero: the game sends the one it has in view (native/skill_wheel.c),
// its class, level, name and its skills and perks by the game's numbers, and
// a level-up's offers. The wheel opens on its class and rings what it has
// learnt (gold, each skill's level above it), what it could take next (green)
// and what the level-up offers (blue); the rest of its perks are the game's
// greyed pictures of them. On the Mac (the J key) the same view.

import SwiftUI

struct SkillWheelScreen: Decodable {
    let lang: String?
    /// The class to open on (HERO_CLASS_...), else the last one shown.
    let heroClass: String?
    /// A skill or perk to pick at once (HERO_SKILL_...).
    let select: String?
    /// The hero the game had in view when the wheel opened (the map's
    /// chosen one, a hero's screen's, a level-up's), when there is one.
    let hero: WheelHero?
    /// A level-up's choices: the skills and perks the game offers now.
    let offer: [SkillRef]?

    enum CodingKeys: String, CodingKey {
        case lang, select, hero, offer
        case heroClass = "class"
    }
}

extension SkillWheelScreen: ScreenDocument {
    func view() -> some View {
        SkillWheelView(openClass: heroClass, openSelect: select, hero: hero, offer: offer ?? [])
    }
}

/// A skill or a perk as the game names it: HERO_SKILL_... or the game's
/// number for it (its place in the skill table, SkillBook.byNumber).
enum SkillRef: Decodable, Hashable {
    case name(String)
    case number(Int)

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let n = try? c.decode(Int.self) { self = .number(n) } else { self = .name(try c.decode(String.self)) }
    }
}

/// A hero's skills and perks, from the game.
struct WheelHero: Decodable {
    let name: String?
    let level: Int?
    /// HERO_CLASS_..., or the game's number for it (its place in HeroClass.xdb).
    let heroClass: SkillRef?
    /// The skills learnt, with their mastery (1 the first level); the game
    /// may list the perks here too (mastery 1), which the table tells apart.
    let skills: [Skill]
    let perks: [SkillRef]

    struct Skill: Decodable {
        let id: SkillRef
        let mastery: Int
    }

    enum CodingKeys: String, CodingKey {
        case name, level, skills, perks
        case heroClass = "class"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decodeIfPresent(String.self, forKey: .name)
        level = try c.decodeIfPresent(Int.self, forKey: .level)
        heroClass = try c.decodeIfPresent(SkillRef.self, forKey: .heroClass)
        skills = try c.decodeIfPresent([Skill].self, forKey: .skills) ?? []
        perks = try c.decodeIfPresent([SkillRef].self, forKey: .perks) ?? []
    }
}

// MARK: - The tables

/// A row of Skills.xdb: a skill or a perk.
struct SkillRow {
    enum Kind {
        case skill, standard, racial, special, ultimate, other
        init(_ s: String) {
            switch s {
            case "SKILLTYPE_SKILL": self = .skill
            case "SKILLTYPE_STANDART_PERK": self = .standard
            case "SKILLTYPE_CLASS_PERK": self = .racial
            case "SKILLTYPE_SPECIAL_PERK": self = .special
            case "SKILLTYPE_UINQUE_PERK", "SKILLTYPE_UNIQUE_PERK": self = .ultimate
            default: self = .other
            }
        }
    }
    var id: String
    var kind = Kind.other
    /// The skill a perk belongs to (BasicSkillID).
    var basic = ""
    /// A racial skill's class (HeroClass; HERO_CLASS_NONE for the others').
    var owner = ""
    var textures: [String] = []
    var names: [String] = []
    var descriptions: [String] = []
    /// The perks (or skills) a class needs first, by class.
    var needs: [String: [String]] = [:]
}

/// A row of HeroClass.xdb.
struct ClassRow {
    let id: String
    var nameRef = ""
    /// The chance (percent) to be offered each skill, in the table's order.
    var chances: [(String, Int)] = []
}

/// The two tables, read once from the install.
final class SkillBook {
    let rows: [String: SkillRow]
    /// Skills.xdb's order: the wheel's around the middle.
    let order: [String]
    let classes: [ClassRow]
    /// HeroClass.xdb's order, HERO_CLASS_NONE too: the game's class numbers.
    let classOrder: [String]

    /// A class by the game's name or number.
    func classID(_ ref: SkillRef?) -> String? {
        switch ref {
        case .name(let n): return n
        case .number(let i): return classOrder.indices.contains(i) ? classOrder[i] : nil
        case nil: return nil
        }
    }

    /// A skill or perk by the game's name or number (its place in the table).
    func id(_ ref: SkillRef) -> String? {
        switch ref {
        case .name(let n): return rows[n] != nil ? n : nil
        case .number(let i): return order.indices.contains(i) ? order[i] : nil
        }
    }

    private init(rows: [String: SkillRow], order: [String], classes: [ClassRow], classOrder: [String]) {
        self.rows = rows
        self.order = order
        self.classes = classes
        self.classOrder = classOrder
    }

    private static let lock = NSLock()
    private static var loaded: SkillBook??

    /// The install's tables (nil when it has none that read).
    static func shared() -> SkillBook? {
        lock.lock()
        defer { lock.unlock() }
        if let loaded { return loaded }
        let book = read()
        loaded = .some(book)
        return book
    }

    private static func read() -> SkillBook? {
        guard let skills = GameArt.shared.file("GameMechanics/RefTables/Skills.xdb"),
              let classes = GameArt.shared.file("GameMechanics/RefTables/HeroClass.xdb") else {
            print("[screens] skill wheel: no skill tables in the install"); fflush(stdout)
            return nil
        }
        let s = SkillsReader()
        let c = ClassesReader()
        guard s.read(skills), c.read(classes) else {
            print("[screens] skill wheel: the skill tables do not read"); fflush(stdout)
            return nil
        }
        var rows: [String: SkillRow] = [:]
        for r in s.rows { rows[r.id] = r }
        let classRows = c.rows.filter { $0.id != "HERO_CLASS_NONE" && !$0.id.isEmpty }
        print("[screens] skill wheel: \(rows.count) skills and perks, \(classRows.count) classes"); fflush(stdout)
        return SkillBook(rows: rows, order: s.rows.map(\.id), classes: classRows, classOrder: c.rows.map(\.id))
    }

    /// A class's name in the game's language (its text beside the table).
    func className(_ c: ClassRow) -> String {
        let ref = c.nameRef.hasPrefix("/") ? c.nameRef : "GameMechanics/RefTables/" + c.nameRef
        if let t = GameArt.shared.text(ref), !t.isEmpty { return t }
        return c.id.replacingOccurrences(of: "HERO_CLASS_", with: "").capitalized
    }
}

/// Reads the rows of a table (objects/Item), each element by its path in the row.
private class TableReader: NSObject, XMLParserDelegate {
    private var path: [String] = []
    private var text = ""

    func read(_ data: Data) -> Bool {
        let p = XMLParser(data: data)
        p.delegate = self
        return p.parse()
    }

    /// The path inside the row: ["ID"], ["obj", "Texture", "Item"]...; nil outside a row.
    private func inRow() -> ArraySlice<String>? {
        path.count >= 3 && path[1] == "objects" && path[2] == "Item" ? path[3...] : nil
    }

    func parser(_ parser: XMLParser, didStartElement name: String, namespaceURI: String?,
                qualifiedName: String?, attributes: [String: String] = [:]) {
        path.append(name)
        text = ""
        if path.count == 3 && path[1] == "objects" && name == "Item" {
            beginRow()
        } else if let rel = inRow() {
            start(Array(rel), attributes)
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        text += string
    }

    func parser(_ parser: XMLParser, didEndElement name: String, namespaceURI: String?,
                qualifiedName: String?) {
        if path.count == 3 && path[1] == "objects" && name == "Item" {
            endRow()
        } else if let rel = inRow() {
            end(Array(rel), text.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        path.removeLast()
        text = ""
    }

    func beginRow() {}
    func endRow() {}
    func start(_ rel: [String], _ attributes: [String: String]) {}
    func end(_ rel: [String], _ text: String) {}
}

private final class SkillsReader: TableReader {
    var rows: [SkillRow] = []
    private var row = SkillRow(id: "")
    private var needClass = ""
    private var needs: [String] = []

    override func beginRow() { row = SkillRow(id: "") }
    override func endRow() { if !row.id.isEmpty { rows.append(row) } }

    override func start(_ rel: [String], _ a: [String: String]) {
        switch rel {
        case ["obj", "Texture", "Item"]: row.textures.append(a["href"] ?? "")
        case ["obj", "NameFileRef", "Item"]: row.names.append(a["href"] ?? "")
        case ["obj", "DescriptionFileRef", "Item"]: row.descriptions.append(a["href"] ?? "")
        case ["obj", "SkillPrerequisites", "Item"]:
            needClass = ""
            needs = []
        default: break
        }
    }

    override func end(_ rel: [String], _ t: String) {
        switch rel {
        case ["ID"]: row.id = t
        case ["obj", "SkillType"]: row.kind = SkillRow.Kind(t)
        case ["obj", "BasicSkillID"]: row.basic = t
        case ["obj", "HeroClass"]: row.owner = t
        case ["obj", "SkillPrerequisites", "Item", "Class"]: needClass = t
        case ["obj", "SkillPrerequisites", "Item", "dependenciesIDs", "Item"]:
            if !t.isEmpty { needs.append(t) }
        case ["obj", "SkillPrerequisites", "Item"]:
            if !needClass.isEmpty { row.needs[needClass] = needs }
        default: break
        }
    }
}

private final class ClassesReader: TableReader {
    var rows: [ClassRow] = []
    private var row = ClassRow(id: "")
    private var skill = ""

    override func beginRow() { row = ClassRow(id: "") }
    override func endRow() { rows.append(row) }

    override func start(_ rel: [String], _ a: [String: String]) {
        if rel == ["obj", "NameFileRef"] { row.nameRef = a["href"] ?? "" }
    }

    override func end(_ rel: [String], _ t: String) {
        switch rel {
        case ["ID"]: row = ClassRow(id: t, nameRef: row.nameRef, chances: row.chances)
        case ["obj", "SkillsProbs", "Item", "SkillID"]: skill = t
        case ["obj", "SkillsProbs", "Item", "Prob"]: row.chances.append((skill, Int(t) ?? 0))
        default: break
        }
    }
}

// MARK: - One class's wheel

/// A perk or skill on the wheel and where it sits.
struct WheelNode: Identifiable {
    let id: String
    let row: SkillRow
    /// Its skill's place around the wheel.
    let sector: Int
    /// 0 a skill, 1... the perks' rows outward; the ultimate -1 (the centre).
    let ring: Int
    let angle: Double
    let radius: Double
    /// What the class needs first (perks or skills on this wheel).
    let needs: [String]
}

struct SkillWheel {
    let classID: String
    let skills: [String]
    /// The class's chance to be offered each skill, when the game has them.
    let chances: [String: Int]
    let nodes: [String: WheelNode]
    /// The order to draw in: skills, then perks row by row.
    let drawOrder: [String]
    /// What needs each node (the reverse of `needs`).
    let opens: [String: [String]]
    /// The wheel's size in its own units (WheelGeometry).
    let size: Double

    /// The class's skills: those it has a chance at, or (the base game, with
    /// no chances) the common ones and its racial one; the racial one first.
    static func make(_ book: SkillBook, _ cls: ClassRow) -> SkillWheel {
        let rows = book.rows
        let chances = Dictionary(cls.chances, uniquingKeysWith: { a, _ in a })
        var skills = book.order.filter { id in
            guard let r = rows[id], r.kind == .skill, id != "HERO_SKILL_NONE" else { return false }
            if !cls.chances.isEmpty { return (chances[id] ?? 0) > 0 }
            return r.owner == cls.id || r.owner == "HERO_CLASS_NONE" || r.owner.isEmpty
        }
        if let racial = skills.firstIndex(where: { rows[$0]?.owner == cls.id }) {
            let first = skills.remove(at: racial)
            skills.insert(first, at: 0)
        }
        let onWheel = Set(skills)
        // The class's perks: a skill's own (standard, and the racial skill's),
        // and those its table lists the class for.
        var perks: [String] = []
        for id in book.order {
            guard let r = rows[id], onWheel.contains(r.basic) else { continue }
            switch r.kind {
            case .standard, .racial: perks.append(id)
            case .special, .ultimate: if r.needs[cls.id] != nil { perks.append(id) }
            default: break
            }
        }
        let perkSet = Set(perks)
        func needs(_ id: String) -> [String] {
            (rows[id]?.needs[cls.id] ?? []).filter { perkSet.contains($0) || onWheel.contains($0) }
        }
        // How deep a perk lies: a skill's own perks 1, the others one past the
        // deepest perk they need.
        var depth: [String: Int] = [:]
        func depthOf(_ id: String, _ seen: Set<String>) -> Int {
            if let d = depth[id] { return d }
            guard let r = rows[id] else { return 1 }
            var d = 1
            if r.kind != .standard && r.kind != .racial {
                let deeper = needs(id).filter { perkSet.contains($0) && !seen.contains($0) }
                    .map { depthOf($0, seen.union([id])) }
                d = 1 + (deeper.max() ?? 0)
            }
            depth[id] = d
            return d
        }
        for p in perks { _ = depthOf(p, []) }

        let geo = WheelGeometry(sectors: skills.count)
        var nodes: [String: WheelNode] = [:]
        var drawOrder: [String] = []
        for (i, s) in skills.enumerated() {
            guard let r = rows[s] else { continue }
            nodes[s] = WheelNode(id: s, row: r, sector: i, ring: 0, angle: geo.center(i),
                                 radius: geo.skillRadius, needs: [])
            drawOrder.append(s)
        }
        var outer = geo.firstRow
        var ultimate: String?
        for (i, s) in skills.enumerated() {
            // The skill's perks by depth, then the table's order: each depth
            // starts a row, and a row holds what fits on its arc (the rest go
            // on the next one out).
            let mine = perks.filter { rows[$0]?.basic == s && rows[$0]?.kind != .ultimate }
                .sorted { (depth[$0] ?? 1, perks.firstIndex(of: $0) ?? 0) < (depth[$1] ?? 1, perks.firstIndex(of: $1) ?? 0) }
            var placed: [[String]] = []
            var rowDepth = 0
            for p in mine {
                let d = depth[p] ?? 1
                if placed.isEmpty || d > rowDepth
                    || placed[placed.count - 1].count >= geo.capacity(row: placed.count - 1) {
                    placed.append([])
                    rowDepth = d
                }
                placed[placed.count - 1].append(p)
            }
            for (k, line) in placed.enumerated() {
                let radius = geo.rowRadius(k)
                outer = max(outer, radius)
                for (j, p) in line.enumerated() {
                    guard let r = rows[p] else { continue }
                    nodes[p] = WheelNode(id: p, row: r, sector: i, ring: k + 1,
                                         angle: geo.angle(sector: i, index: j, of: line.count, radius: radius),
                                         radius: radius, needs: needs(p))
                    drawOrder.append(p)
                }
            }
            if ultimate == nil {
                ultimate = perks.first { rows[$0]?.basic == s && rows[$0]?.kind == .ultimate }
            }
        }
        if let u = ultimate ?? perks.first(where: { rows[$0]?.kind == .ultimate }), let r = rows[u] {
            nodes[u] = WheelNode(id: u, row: r, sector: -1, ring: -1, angle: -Double.pi / 2, radius: 0,
                                 needs: needs(u))
            drawOrder.append(u)
        }
        var opens: [String: [String]] = [:]
        for id in drawOrder {
            for n in nodes[id]?.needs ?? [] { opens[n, default: []].append(id) }
        }
        return SkillWheel(classID: cls.id, skills: skills, chances: chances, nodes: nodes,
                          drawOrder: drawOrder, opens: opens,
                          size: 2 * (outer + WheelGeometry.perkIcon / 2 + 8))
    }

    /// A node's place in the wheel's own units, from its centre.
    func point(_ n: WheelNode) -> CGPoint {
        CGPoint(x: size / 2 + n.radius * cos(n.angle), y: size / 2 + n.radius * sin(n.angle))
    }

    /// Everything `id` needs, all the way back.
    func chain(_ id: String) -> Set<String> {
        var out = Set<String>()
        var todo = nodes[id]?.needs ?? []
        while let n = todo.popLast() {
            if out.insert(n).inserted { todo += nodes[n]?.needs ?? [] }
        }
        return out
    }
}

/// Sizes in the wheel's own units (the view scales it to fit).
struct WheelGeometry {
    static let skillIcon = 56.0
    static let perkIcon = 36.0
    static let gap = 5.0
    let sectors: Int
    let skillRadius = 170.0
    let firstRow = 258.0
    let rowStep = 44.0

    var width: Double { 2 * Double.pi / Double(max(sectors, 1)) }
    /// The racial skill on top, the rest clockwise.
    func center(_ i: Int) -> Double { -Double.pi / 2 + Double(i) * width }
    func rowRadius(_ k: Int) -> Double { firstRow + Double(k) * rowStep }
    /// How many perks a row holds in one skill's slice.
    func capacity(row k: Int) -> Int {
        // A little under the gap is let go: three perks to a slice of 13.
        max(1, Int(rowRadius(k) * width / (Self.perkIcon + Self.gap) + 0.15))
    }
    func angle(sector i: Int, index j: Int, of count: Int, radius: Double) -> Double {
        let step = min(width / Double(max(count, 1)), (Self.perkIcon + Self.gap) / radius)
        return center(i) + (Double(j) - Double(count - 1) / 2) * step
    }
}

// MARK: - A hero on the wheel

/// What a hero has on its class's wheel: the skills learnt with their
/// mastery, the perks, what it could take next and what a level-up offers.
/// "Could take" reads the tables only: a skill below its last level; a perk
/// whose skill has a level to spare (each level of a skill opens one of its
/// perks) and whose needs are learnt; the ultimate once its needs are.
struct HeroMarks {
    var shown = false
    var mastery: [String: Int] = [:]
    var perks: Set<String> = []
    var open: Set<String> = []
    var offer: Set<String> = []

    func learnt(_ id: String) -> Bool { mastery[id] != nil || perks.contains(id) }

    static func make(_ wheel: SkillWheel, _ book: SkillBook, _ hero: WheelHero?, _ offer: [SkillRef]) -> HeroMarks {
        var m = HeroMarks()
        let cls = book.classID(hero?.heroClass)
        guard let hero, cls == nil || cls == wheel.classID else { return m }
        m.shown = true
        for s in hero.skills where s.mastery > 0 {
            guard let id = book.id(s.id), let row = book.rows[id] else { continue }
            if row.kind == .skill { m.mastery[id] = s.mastery } else { m.perks.insert(id) }
        }
        m.perks.formUnion(hero.perks.compactMap { book.id($0) })
        m.offer = Set(offer.compactMap { book.id($0) })
        var taken: [String: Int] = [:]
        for p in m.perks {
            if let b = book.rows[p]?.basic, !b.isEmpty { taken[b, default: 0] += 1 }
        }
        for (id, n) in wheel.nodes where !m.learnt(id) || n.ring == 0 {
            let needsMet = n.needs.allSatisfy { m.learnt($0) }
            switch n.row.kind {
            case .skill:
                if let have = m.mastery[id], have < n.row.masteries { m.open.insert(id) }
            case .ultimate:
                if needsMet && !n.needs.isEmpty { m.open.insert(id) }
            default:
                let level = m.mastery[n.row.basic] ?? 0
                if needsMet && level > taken[n.row.basic, default: 0] { m.open.insert(id) }
            }
        }
        return m
    }
}

extension HeroMarks {
    static let learntColor = Theme.gold
    static let openColor = Color(red: 0.55, green: 0.86, blue: 0.42)
    static let offerColor = Color(red: 0.48, green: 0.80, blue: 1.0)

    static func roman(_ n: Int) -> String { ["", "I", "II", "III", "IV", "V"][max(0, min(n, 5))] }
}

// MARK: - Texts and pictures

extension SkillRow {
    /// The perk's icon, or a skill's at its first level (its first texture
    /// is the "none" slot, empty).
    var icon: String? {
        if kind == .skill { return textures.dropFirst().first(where: { !$0.isEmpty }) ?? textures.first(where: { !$0.isEmpty }) }
        // A perk's first texture is its greyed look, the second the learnt one.
        return textures.count > 1 && !textures[1].isEmpty ? textures[1] : textures.first(where: { !$0.isEmpty })
    }

    /// The name: a skill's plain one ("Logistics", the game's CommonName text
    /// beside its folder) where the install has it, else its first level's.
    var name: String {
        if kind == .skill, let first = names.first, let folder = first.split(separator: "/").dropLast(2).last {
            for dir in ["CommonSkills", "UniqueSkills"] {
                if let t = GameArt.shared.text("GameMechanics/Skill/\(dir)/\(folder)/CommonName.txt"), !t.isEmpty {
                    return t
                }
            }
        }
        for ref in names where !ref.isEmpty {
            if let t = GameArt.shared.text(ref), !t.isEmpty { return t }
        }
        return id.replacingOccurrences(of: "HERO_SKILL_", with: "").replacingOccurrences(of: "_", with: " ").capitalized
    }

    /// A perk's description, or a skill's per level (its level, the
    /// mastery it stands for, name and text).
    var details: [LevelText] {
        if kind == .skill {
            return descriptions.indices.compactMap { i in
                let text = GameArt.shared.text(descriptions[i]) ?? ""
                let title = i < names.count ? GameArt.shared.text(names[i]) ?? "" : ""
                return text.isEmpty && title.isEmpty ? nil : LevelText(level: i, title: title, text: text)
            }
        }
        return descriptions.compactMap { ref in
            let t = GameArt.shared.text(ref) ?? ""
            return t.isEmpty ? nil : LevelText(level: 0, title: "", text: t)
        }
    }

    /// A skill's levels: those its table names (the first, "none", left out
    /// when it is empty).
    var masteries: Int { kind == .skill ? max(1, names.filter { !$0.isEmpty }.count) : 1 }

    /// The table's slot for `mastery` (1 the first level): the slots start
    /// with an empty "none" one where the table has it.
    func slot(mastery: Int) -> Int { names.first?.isEmpty == false ? mastery - 1 : mastery }
}

struct LevelText {
    let level: Int
    let title: String
    let text: String
}

/// The names and descriptions a wheel shows, read off the main thread once.
struct WheelTexts {
    var names: [String: String] = [:]
    var details: [String: [LevelText]] = [:]

    static func read(_ wheel: SkillWheel) -> WheelTexts {
        var t = WheelTexts()
        for (id, n) in wheel.nodes {
            t.names[id] = n.row.name
            t.details[id] = n.row.details
        }
        return t
    }
}

// MARK: - The view

struct SkillWheelView: View {
    var openClass: String? = nil
    var openSelect: String? = nil
    var hero: WheelHero? = nil
    var offer: [SkillRef] = []
    @EnvironmentObject var store: ScreenStore
    @State private var marks = HeroMarks()
    @State private var book: SkillBook?
    @State private var loading = true
    @State private var classIndex = 0
    @State private var classNames: [String] = []
    @State private var wheel: SkillWheel?
    @State private var texts = WheelTexts()
    @State private var selected: String?
    @State private var zoom: CGFloat = 1
    @State private var pinch: CGFloat = 1
    @State private var pan: CGSize = .zero
    @State private var drag: CGSize = .zero

    private static var lastClass: String?

    var body: some View {
        GeometryReader { geo in
            let phone = geo.size.height < 500
            let side = min(phone ? 270 : 340, geo.size.width * 0.38)
            ZStack {
                // The game goes on under it: dimmed, and no touch reaches it.
                Color.black.opacity(0.55)
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture { selected = nil }
                // The wheel is round: on a wide screen the panel keeps beside
                // it, the two in the middle.
                HStack(spacing: 10) {
                    wheelArea
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .clipped()
                    sidePanel(phone: phone)
                        .frame(width: side)
                }
                .padding(phone ? 6 : 14)
                .frame(maxWidth: geo.size.height + side + 40)
            }
        }
        .task { await load() }
    }

    // MARK: Loading

    private func load() async {
        let book = await Task.detached(priority: .userInitiated) { SkillBook.shared() }.value
        guard let book, !book.classes.isEmpty else {
            loading = false
            return
        }
        let names = await Task.detached(priority: .userInitiated) { book.classes.map { book.className($0) } }.value
        self.book = book
        classNames = names
        if let want = openClass ?? book.classID(hero?.heroClass) ?? Self.lastClass,
           let i = book.classes.firstIndex(where: { $0.id == want }) {
            classIndex = i
        }
        await show(classIndex)
        if let pick = openSelect, wheel?.nodes[pick] != nil { selected = pick }
        loading = false
    }

    private func show(_ i: Int) async {
        guard let book, book.classes.indices.contains(i) else { return }
        let cls = book.classes[i]
        let made = await Task.detached(priority: .userInitiated) { () -> (SkillWheel, WheelTexts) in
            let w = SkillWheel.make(book, cls)
            return (w, WheelTexts.read(w))
        }.value
        classIndex = i
        Self.lastClass = cls.id
        selected = nil
        wheel = made.0
        texts = made.1
        marks = HeroMarks.make(made.0, book, hero, offer)
        zoom = 1
        pan = .zero
    }

    // MARK: The wheel

    @ViewBuilder private var wheelArea: some View {
        if let wheel {
            GeometryReader { geo in
                let fit = min(geo.size.width, geo.size.height) / wheel.size
                let scale = fit * zoom * pinch
                WheelCanvas(wheel: wheel, texts: texts, marks: marks, selected: $selected)
                    .frame(width: wheel.size, height: wheel.size)
                    .scaleEffect(scale)
                    .frame(width: geo.size.width, height: geo.size.height)
                    .offset(x: pan.width + drag.width, y: pan.height + drag.height)
                    .contentShape(Rectangle())
                    .gesture(zoomGesture.simultaneously(with: panGesture))
                    .onTapGesture(count: 2) {
                        withAnimation(.easeOut(duration: 0.2)) {
                            if zoom > 1.05 {
                                zoom = 1
                                pan = .zero
                            } else {
                                zoom = 2.2
                            }
                        }
                    }
                    .onTapGesture { selected = nil }
            }
        } else if loading {
            ProgressView().tint(Theme.gold)
        } else {
            Text(store.tr("В игре не нашлось таблицы навыков.", "The game has no skill table to show."))
                .font(Theme.body(18))
                .foregroundStyle(Theme.text)
                .padding(20)
                .panel()
        }
    }

    private var zoomGesture: some Gesture {
        MagnifyGesture()
            .onChanged { pinch = $0.magnification }
            .onEnded { v in
                zoom = min(max(zoom * v.magnification, 1), 4)
                pinch = 1
                if zoom <= 1.01 { pan = .zero }
            }
    }

    private var panGesture: some Gesture {
        DragGesture(minimumDistance: 8)
            .onChanged { v in if zoom > 1.01 { drag = v.translation } }
            .onEnded { v in
                if zoom > 1.01 {
                    pan.width += v.translation.width
                    pan.height += v.translation.height
                }
                drag = .zero
            }
    }

    // MARK: The panel

    private func sidePanel(phone: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                BackButton { store.send("wheel_close") }
                Text(store.tr("Колесо навыков", "Skill wheel"))
                    .font(Theme.heading(phone ? 20 : 24))
                    .foregroundStyle(Theme.gold)
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
            }
            if let hero {
                heroLine(hero, phone: phone)
            }
            if !classNames.isEmpty {
                Menu {
                    ForEach(classNames.indices, id: \.self) { i in
                        Button(isHeroClass(i) ? "\(classNames[i]) ★" : classNames[i]) { Task { await show(i) } }
                    }
                } label: {
                    HStack {
                        Text(classNames[min(classIndex, classNames.count - 1)])
                            .font(Theme.heading(phone ? 17 : 19))
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                        Spacer(minLength: 4)
                        Image(systemName: "chevron.down")
                    }
                    .foregroundStyle(Theme.khakiText)
                    .padding(.horizontal, 12)
                    .frame(maxWidth: .infinity, minHeight: Theme.touch)
                    .gameField()
                }
                // The Mac's pop-up button chrome would frame the field again.
                .menuStyle(.button)
                .buttonStyle(.plain)
                .menuIndicator(.hidden)
            }
            ScrollView {
                details(phone: phone)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if marks.shown {
                legend
            }
        }
        .padding(phone ? 8 : 12)
        .frame(maxHeight: .infinity, alignment: .top)
        .panel()
    }

    private func isHeroClass(_ i: Int) -> Bool {
        guard let book, let c = book.classID(hero?.heroClass), book.classes.indices.contains(i) else { return false }
        return book.classes[i].id == c
    }

    private func heroLine(_ hero: WheelHero, phone: Bool) -> some View {
        HStack(spacing: 6) {
            Text(hero.name ?? store.tr("Герой", "Hero"))
                .font(Theme.heading(phone ? 16 : 18))
                .foregroundStyle(Theme.text)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            if let level = hero.level {
                Text(store.tr("уровень", "level") + " \(level)")
                    .font(Theme.body(phone ? 13 : 14))
                    .foregroundStyle(Theme.textDim)
            }
            Spacer(minLength: 0)
            if !marks.shown, let book, let c = book.classID(hero.heroClass),
               let i = book.classes.firstIndex(where: { $0.id == c }) {
                // Another class's wheel: back to the hero's.
                Button(store.tr("Его класс", "Their class")) { Task { await show(i) } }
                    .font(Theme.body(13, weight: .bold))
                    .foregroundStyle(Theme.gold)
                    .buttonStyle(PressDim())
            }
        }
    }

    private var legend: some View {
        HStack(spacing: 10) {
            legendItem(HeroMarks.learntColor, store.tr("изучено", "learnt"))
            legendItem(HeroMarks.openColor, store.tr("можно взять", "can take"))
            if !marks.offer.isEmpty {
                legendItem(HeroMarks.offerColor, store.tr("предлагают", "offered"))
            }
        }
        .font(Theme.body(12))
        .foregroundStyle(Theme.textDim)
        .lineLimit(1)
        .minimumScaleFactor(0.7)
    }

    private func legendItem(_ color: Color, _ label: String) -> some View {
        HStack(spacing: 4) {
            Circle().strokeBorder(color, lineWidth: 2.5).frame(width: 14, height: 14)
            Text(label)
        }
    }

    @ViewBuilder private func details(phone: Bool) -> some View {
        if let wheel, let id = selected, let node = wheel.nodes[id] {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 10) {
                    GameImage(ref: node.row.icon, placeholder: "seal")
                        .frame(width: 48, height: 48)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(texts.names[id] ?? "")
                            .font(Theme.heading(phone ? 18 : 20))
                            .foregroundStyle(Theme.gold)
                            .fixedSize(horizontal: false, vertical: true)
                        Text(kindLine(node, wheel))
                            .font(Theme.body(14))
                            .foregroundStyle(Theme.textDim)
                        if let state = heroState(node) {
                            Text(state.0).font(Theme.body(14, weight: .bold)).foregroundStyle(state.1)
                        }
                    }
                }
                let current = marks.mastery[id].map { node.row.slot(mastery: $0) }
                ForEach(Array((texts.details[id] ?? []).enumerated()), id: \.offset) { _, d in
                    if !d.title.isEmpty {
                        Text(d.level == current ? "\(d.title) ✓" : d.title)
                            .font(Theme.body(15, weight: .bold))
                            .foregroundStyle(d.level == current ? HeroMarks.learntColor : Theme.khakiText)
                    }
                    Text(d.text)
                        .font(Theme.body(phone ? 14 : 15))
                        .foregroundStyle(Theme.text)
                        .fixedSize(horizontal: false, vertical: true)
                }
                links(store.tr("Требует", "Needs"), node.needs, wheel)
                links(store.tr("Открывает", "Opens"), wheel.opens[id] ?? [], wheel)
                if node.ring == 0 {
                    links(store.tr("Способности", "Perks"),
                          wheel.drawOrder.filter { wheel.nodes[$0]?.ring ?? 0 > 0 && wheel.nodes[$0]?.row.basic == id },
                          wheel)
                }
            }
        } else if wheel != nil {
            Text(store.tr("Нажмите на навык или способность: здесь появится, что она даёт и что для неё нужно. Щипок или двойное касание — увеличить.",
                          "Tap a skill or a perk to see what it does and what it needs. Pinch or double-tap to zoom."))
                .font(Theme.body(phone ? 14 : 15))
                .foregroundStyle(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// The hero's side of a node, when a hero is on the wheel.
    private func heroState(_ n: WheelNode) -> (String, Color)? {
        guard marks.shown else { return nil }
        if marks.offer.contains(n.id) { return (store.tr("Предлагают сейчас", "Offered now"), HeroMarks.offerColor) }
        if let m = marks.mastery[n.id] {
            return (store.tr("Изучено", "Learnt") + ": \(HeroMarks.roman(m)) / \(HeroMarks.roman(n.row.masteries))",
                    HeroMarks.learntColor)
        }
        if marks.perks.contains(n.id) { return (store.tr("Изучено", "Learnt"), HeroMarks.learntColor) }
        if marks.open.contains(n.id) { return (store.tr("Можно взять", "Can take"), HeroMarks.openColor) }
        return (store.tr("Не изучено", "Not learnt"), Theme.textDim)
    }

    private func kindLine(_ n: WheelNode, _ w: SkillWheel) -> String {
        switch n.row.kind {
        case .skill:
            let racial = n.row.owner == w.classID
            var s = racial ? store.tr("Расовый навык", "Racial skill") : store.tr("Навык", "Skill")
            if let c = w.chances[n.id] { s += " · " + store.tr("шанс", "chance") + " \(c)%" }
            return s
        case .ultimate: return store.tr("Высшая способность", "Ultimate")
        default:
            let skill = texts.names[n.row.basic] ?? ""
            return skill.isEmpty ? store.tr("Способность", "Perk") : skill
        }
    }

    @ViewBuilder private func links(_ title: String, _ ids: [String], _ w: SkillWheel) -> some View {
        if !ids.isEmpty {
            Text(title).font(Theme.body(15, weight: .bold)).foregroundStyle(Theme.khakiText).padding(.top, 4)
            ForEach(ids, id: \.self) { other in
                Button {
                    selected = other
                } label: {
                    HStack(spacing: 8) {
                        GameImage(ref: w.nodes[other]?.row.icon, placeholder: "seal")
                            .frame(width: 30, height: 30)
                        Text(texts.names[other] ?? other)
                            .font(Theme.body(15))
                            .foregroundStyle(Theme.text)
                            .multilineTextAlignment(.leading)
                        Spacer(minLength: 0)
                    }
                    .frame(minHeight: 36)
                    .contentShape(Rectangle())
                }
                .buttonStyle(PressDim())
            }
        }
    }
}

/// The wheel itself, in its own units: rings, lines for the chosen node's
/// chain, the icons.
private struct WheelCanvas: View {
    let wheel: SkillWheel
    let texts: WheelTexts
    let marks: HeroMarks
    @Binding var selected: String?

    var body: some View {
        let chain = selected.map { wheel.chain($0) } ?? []
        let opens = Set(selected.flatMap { wheel.opens[$0] } ?? [])
        let lit = chain.union(opens).union(selected.map { [$0] } ?? [])
        ZStack(alignment: .topLeading) {
            backdrop
            lines(lit)
            ForEach(wheel.drawOrder, id: \.self) { id in
                if let n = wheel.nodes[id] {
                    icon(n, dim: selected != nil && !lit.contains(id) && n.ring != 0)
                }
            }
        }
        .frame(width: wheel.size, height: wheel.size, alignment: .topLeading)
    }

    private var backdrop: some View {
        let c = wheel.size / 2
        let geo = WheelGeometry(sectors: wheel.skills.count)
        return Canvas { ctx, _ in
            let center = CGPoint(x: c, y: c)
            ctx.fill(Path(ellipseIn: CGRect(x: 0, y: 0, width: wheel.size, height: wheel.size)),
                     with: .color(Theme.panel.opacity(0.92)))
            ctx.stroke(Path(ellipseIn: CGRect(x: 2, y: 2, width: wheel.size - 4, height: wheel.size - 4)),
                       with: .color(Theme.goldDim), lineWidth: 3)
            for r in [geo.skillRadius, geo.firstRow - WheelGeometry.perkIcon / 2 - 8] {
                ctx.stroke(Path(ellipseIn: CGRect(x: c - r, y: c - r, width: 2 * r, height: 2 * r)),
                           with: .color(Theme.goldDim.opacity(0.5)), lineWidth: 1)
            }
            // The slices between the skills.
            for i in 0..<wheel.skills.count {
                let a = geo.center(i) + geo.width / 2
                var p = Path()
                p.move(to: CGPoint(x: c + 110 * cos(a), y: c + 110 * sin(a)))
                p.addLine(to: CGPoint(x: c + (c - 6) * cos(a), y: c + (c - 6) * sin(a)))
                ctx.stroke(p, with: .color(Theme.goldDim.opacity(0.35)), lineWidth: 1)
            }
            ctx.stroke(Path(ellipseIn: CGRect(x: center.x - 100, y: center.y - 100, width: 200, height: 200)),
                       with: .color(Theme.goldDim.opacity(0.6)), lineWidth: 1.5)
        }
        .frame(width: wheel.size, height: wheel.size)
        .allowsHitTesting(false)
    }

    private func lines(_ lit: Set<String>) -> some View {
        Canvas { ctx, _ in
            guard selected != nil else { return }
            for id in lit {
                guard let n = wheel.nodes[id] else { continue }
                let to = wheel.point(n)
                for need in n.needs where lit.contains(need) {
                    guard let m = wheel.nodes[need] else { continue }
                    var p = Path()
                    p.move(to: wheel.point(m))
                    p.addLine(to: to)
                    ctx.stroke(p, with: .color(Theme.gold), style: StrokeStyle(lineWidth: 3, lineCap: .round))
                }
            }
            // A perk's skill.
            if let s = selected, let n = wheel.nodes[s], n.ring > 0, let skill = wheel.nodes[n.row.basic] {
                var p = Path()
                p.move(to: wheel.point(skill))
                p.addLine(to: wheel.point(n))
                ctx.stroke(p, with: .color(Theme.accent.opacity(0.7)),
                           style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [5, 5]))
            }
        }
        .frame(width: wheel.size, height: wheel.size)
        .allowsHitTesting(false)
    }

    /// A node's ring: chosen, offered, learnt, could take, or the plain one.
    private func ring(_ n: WheelNode, chosen: Bool) -> (Color, CGFloat) {
        if chosen { return (Theme.gold, 3.5) }
        if marks.offer.contains(n.id) { return (HeroMarks.offerColor, 3.5) }
        if marks.learnt(n.id) { return (HeroMarks.learntColor, 2.5) }
        if marks.open.contains(n.id) { return (HeroMarks.openColor, 2.5) }
        return (n.row.kind == .ultimate ? Theme.accent : Color.black.opacity(0.6), 1.5)
    }

    private func icon(_ n: WheelNode, dim: Bool) -> some View {
        let size = n.ring == 0 ? WheelGeometry.skillIcon : n.ring < 0 ? 64 : WheelGeometry.perkIcon
        let p = wheel.point(n)
        let chosen = selected == n.id
        let (color, width) = ring(n, chosen: chosen)
        // A hero's perk not learnt: the game's own greyed picture of it (a
        // perk's first texture), unless the hero could take it now.
        let unlearnt = marks.shown && !marks.learnt(n.id)
        let grey = unlearnt && n.ring != 0 && !marks.open.contains(n.id) && !marks.offer.contains(n.id)
        let ref = grey && n.row.textures.count > 1 && !n.row.textures[0].isEmpty ? n.row.textures[0] : n.row.icon
        return Button {
            selected = chosen ? nil : n.id
        } label: {
            // Round: around the wheel square icons at a slant would touch.
            GameImage(ref: ref, placeholder: "seal", fill: true)
                .frame(width: size, height: size)
                .clipShape(Circle())
                .overlay {
                    Circle().strokeBorder(color, lineWidth: width)
                }
                .shadow(color: marks.offer.contains(n.id) ? HeroMarks.offerColor.opacity(0.9) : .clear, radius: 6)
                .opacity(dim ? 0.35 : unlearnt && n.ring == 0 && !marks.open.contains(n.id) ? 0.6 : 1)
                .overlay(alignment: .top) {
                    if n.ring == 0, let m = marks.mastery[n.id] {
                        Text(HeroMarks.roman(m))
                            .font(.system(size: 12, weight: .heavy, design: .serif))
                            .foregroundStyle(Theme.gold)
                            .padding(.horizontal, 4)
                            .background(Color.black.opacity(0.75), in: Capsule())
                            .offset(y: -8)
                    }
                }
                .overlay(alignment: .bottom) {
                    if n.ring == 0, let c = wheel.chances[n.id] {
                        Text("\(c)%")
                            .font(.system(size: 13, weight: .bold))
                            .foregroundStyle(Theme.gold)
                            .padding(.horizontal, 3)
                            .background(Color.black.opacity(0.7), in: RoundedRectangle(cornerRadius: 3))
                            .offset(y: 9)
                    }
                }
        }
        .buttonStyle(PressDim())
        .accessibilityLabel(texts.names[n.id] ?? n.id)
        .position(p)
    }
}
