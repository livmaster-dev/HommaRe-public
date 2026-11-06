// LauncherGog.swift - installing a game straight from the player's GOG.com
// library, without an installer: the player signs in on GOG's own page (a web
// view, LauncherGogView.swift), the app checks the account owns the product,
// then downloads the build's GOG Galaxy (generation 2) depot files for one
// language into the app's Games folder, where the launcher finds them as it
// finds an install from an installer. No Files app, no innoextract.
//
// What is kept is what an install from the offline installer keeps: the
// depot files minus launcher-games.json "exclude". GOG's own checksums (MD5
// of each chunk, packed and unpacked) are checked as it arrives; where the
// app has an install manifest for the language (launcher-manifests/), each
// file's SHA-256 must be the manifest's too. A file is written in place, chunk
// after chunk, so an interrupted download resumes from the last chunk that
// checks out. Until every file is there .recomp-install.json marks the folder
// as an unfinished install, as LauncherInstaller.swift does.
//
// The GOG sign-in, token, CDN link and chunk code is ported from CyPort's
// shim/StoresCore.swift (github.com/Thijs2004/CyPort), under its licence:
//
//   MIT License
//
//   Copyright (c) 2026 Thijs Mussig
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
import CryptoKit
import Foundation
import Security

/// Where an install from GOG.com comes from (InstallMarker.gog): resuming it
/// downloads the same build in the same language.
struct GogSource: Codable, Hashable {
    var product: String
    var build: String
    /// The launcher's language code (L10n.languages) of the game's files.
    var language: String
}

struct GogFailure: LocalizedError {
    var errorDescription: String?
    /// GOG no longer takes the saved sign-in.
    var signedOut = false
    /// The HTTP status a server answered with (0: none).
    var status = 0
}

// MARK: - Network

enum GogNet {
    static let session: URLSession = {
        let c = URLSessionConfiguration.default
        c.timeoutIntervalForRequest = 60
        c.httpMaximumConnectionsPerHost = 8
        c.requestCachePolicy = .reloadIgnoringLocalCacheData
        c.urlCache = nil
        return URLSession(configuration: c)
    }()

    /// The body of `url`, tried again on a dropped connection or a server error.
    static func data(_ url: URL, headers: [String: String] = [:], tries: Int = 3) async throws -> Data {
        var last: Error = GogFailure(errorDescription: url.host ?? "")
        for attempt in 0..<tries {
            if attempt > 0 { try await Task.sleep(nanoseconds: UInt64(attempt) * 1_500_000_000) }
            var r = URLRequest(url: url)
            for (k, v) in headers { r.setValue(v, forHTTPHeaderField: k) }
            do {
                let (d, resp) = try await session.data(for: r)
                let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
                if (200..<300).contains(code) { return d }
                last = GogFailure(errorDescription: L10n.gogHttp(url.host ?? "", code), signedOut: code == 401,
                                  status: code)
                if [400, 401, 403, 404].contains(code) { throw last }
            } catch let e as GogFailure {
                throw e
            } catch {
                if Task.isCancelled || error is CancellationError || (error as? URLError)?.code == .cancelled {
                    throw CancellationError()
                }
                last = error
            }
        }
        throw last
    }

    static func json(_ url: URL, headers: [String: String] = [:]) async throws -> [String: Any] {
        let d = try await data(url, headers: headers)
        guard let o = try JSONSerialization.jsonObject(with: d) as? [String: Any] else {
            throw GogFailure(errorDescription: L10n.gogUnexpected(url.host ?? ""))
        }
        return o
    }

    /// The bytes of a zlib stream (GOG's chunks and build data), nil when `data` is none.
    static func inflate(_ data: Data, expected: Int? = nil) -> Data? {
        guard data.count > 6 else { return nil }
        let cmf = data[data.startIndex], flg = data[data.startIndex + 1]
        guard cmf & 0x0f == 8, (UInt16(cmf) << 8 | UInt16(flg)) % 31 == 0 else { return nil }
        guard let out = try? (Data(data.dropFirst(2)) as NSData).decompressed(using: .zlib) as Data else { return nil }
        if let expected, out.count != expected { return nil }
        return out
    }

    static func md5(_ d: Data) -> String {
        Insecure.MD5.hash(data: d).map { String(format: "%02x", $0) }.joined()
    }
}

// MARK: - Sign-in

/// The GOG sign-in, kept in the Keychain (never in a file or the log): the
/// tokens GOG Galaxy's own client gets, refreshed before they run out.
actor GogSession {
    static let shared = GogSession()

    // GOG Galaxy's public client, which GOG's sign-in page sends back to
    // embed.gog.com with a code (the same one other open GOG clients use).
    static let clientID = "46899977096215655"
    static let clientSecret = "9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9"
    static let redirect = "https://embed.gog.com/on_login_success?origin=client"

    static var loginURL: URL {
        var c = URLComponents(string: "https://auth.gog.com/auth")!
        c.queryItems = [.init(name: "client_id", value: clientID), .init(name: "redirect_uri", value: redirect),
                        .init(name: "response_type", value: "code"), .init(name: "layout", value: "client2")]
        return c.url!
    }

    /// The code in the page the sign-in lands on, or nil.
    static func code(from url: URL) -> String? {
        guard url.host == "embed.gog.com", url.path == "/on_login_success" else { return nil }
        return URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "code" }?.value
    }

    /// The signed-in account's name, nil when no one is signed in.
    nonisolated static var account: String? {
        guard let t = GogKeychain.load() else { return nil }
        return t["name"] as? String ?? ""
    }

    nonisolated static func signOut() { GogKeychain.delete() }

    private func token(_ grant: [String: String]) async throws {
        var c = URLComponents(string: "https://auth.gog.com/token")!
        c.queryItems = ([("client_id", Self.clientID), ("client_secret", Self.clientSecret)]
                        + grant.map { ($0.key, $0.value) }).map { URLQueryItem(name: $0.0, value: $0.1) }
        let o = try await GogNet.json(c.url!)
        guard let a = o["access_token"] as? String, let r = o["refresh_token"] as? String else {
            throw GogFailure(errorDescription: L10n.gogUnexpected("auth.gog.com"))
        }
        var t: [String: Any] = ["access": a, "refresh": r,
                                "expires": Date().timeIntervalSince1970 + ((o["expires_in"] as? Double) ?? 3600)]
        if let n = GogKeychain.load()?["name"] { t["name"] = n }
        guard GogKeychain.save(t) else { throw GogFailure(errorDescription: L10n.gogKeychain) }
    }

    /// Trades the sign-in page's code for tokens; the account's name.
    func signIn(code: String) async throws -> String {
        try await token(["grant_type": "authorization_code", "code": code, "redirect_uri": Self.redirect])
        var name = ""
        if let u = try? await GogNet.json(URL(string: "https://embed.gog.com/userData.json")!, headers: try headers()),
           let n = u["username"] as? String {
            name = n
            var t = GogKeychain.load() ?? [:]
            t["name"] = n
            _ = GogKeychain.save(t)
        }
        return name
    }

    /// The Authorization header, the token renewed when it is about to run out.
    func headers() async throws -> [String: String] {
        guard let t = GogKeychain.load(), let r = t["refresh"] as? String else {
            throw GogFailure(errorDescription: L10n.gogSignInFirst, signedOut: true)
        }
        if (t["expires"] as? Double ?? 0) - Date().timeIntervalSince1970 < 300 {
            do {
                try await token(["grant_type": "refresh_token", "refresh_token": r])
            } catch let e as GogFailure where (400..<500).contains(e.status) {
                // GOG refused the refresh token: the sign-in is gone.
                GogKeychain.delete()
                throw GogFailure(errorDescription: L10n.gogExpired, signedOut: true)
            }
            // Offline or a server error: the sign-in is still good, try later.
        }
        return ["Authorization": "Bearer \(GogKeychain.load()?["access"] as? String ?? "")"]
    }

    /// The products the account owns (GOG ids).
    func owned() async throws -> Set<String> {
        do {
            let o = try await GogNet.json(URL(string: "https://embed.gog.com/user/data/games")!, headers: try headers())
            return Set((o["owned"] as? [Any] ?? []).map { "\($0)" })
        } catch let e as GogFailure where e.signedOut {
            GogKeychain.delete()
            throw GogFailure(errorDescription: L10n.gogExpired, signedOut: true)
        }
    }
}

/// The sign-in's tokens in the Keychain. An app signed without a Keychain
/// group (an unsigned simulator build, some sideloading tools) cannot keep
/// them there: they are then kept in memory, for this launch only.
enum GogKeychain {
    private static var service: String { (Bundle.main.bundleIdentifier ?? "recomp") + ".gog" }
    private static let account = "gog"
    private static let lock = NSLock()
    nonisolated(unsafe) private static var memory: Data?

    private static var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    static func save(_ value: [String: Any]) -> Bool {
        guard let d = try? JSONSerialization.data(withJSONObject: value) else { return false }
        SecItemDelete(query as CFDictionary)
        var add = query
        add[kSecValueData as String] = d
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(add as CFDictionary, nil)
        lock.withLock { memory = status == errSecSuccess ? nil : d }
        if status != errSecSuccess {
            FileHandle.standardError.write(Data("[launcher] gog: no keychain (\(status)), the sign-in lasts this launch\n".utf8))
        }
        return true
    }

    static func load() -> [String: Any]? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        let found = SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess ? out as? Data : nil
        guard let d = found ?? lock.withLock({ memory }) else { return nil }
        return try? JSONSerialization.jsonObject(with: d) as? [String: Any]
    }

    static func delete() {
        SecItemDelete(query as CFDictionary)
        lock.withLock { memory = nil }
    }
}

/// GOG's signed CDN links, one per product, renewed before they run out.
actor GogLinks {
    private var links: [String: (format: String, params: [String: String], expires: Double)] = [:]

    func url(product: String, path: String, renew: Bool) async throws -> URL {
        if renew || links[product].map({ $0.expires - Date().timeIntervalSince1970 < 120 }) ?? true {
            let u = URL(string: "https://content-system.gog.com/products/\(product)/secure_link?generation=2&_version=2&path=/")!
            let o = try await GogNet.json(u, headers: try await GogSession.shared.headers())
            let all = o["urls"] as? [[String: Any]] ?? []
            let eps = all.filter { !(($0["fallback_only"] as? Bool) ?? false) }
                .sorted { ($0["priority"] as? Int ?? 0) > ($1["priority"] as? Int ?? 0) }
            guard let e = eps.first ?? all.first, let f = e["url_format"] as? String,
                  let p = e["parameters"] as? [String: Any] else {
                throw GogFailure(errorDescription: L10n.gogNoLink)
            }
            links[product] = (f, p.mapValues { "\($0)" },
                              (p["expires_at"] as? Double) ?? Date().timeIntervalSince1970 + 3000)
        }
        let l = links[product]!
        var p = l.params
        p["path"] = (p["path"] ?? "") + path
        var s = l.format.contains("{path}") ? l.format : l.format + "{path}"
        for (k, v) in p { s = s.replacingOccurrences(of: "{\(k)}", with: v) }
        guard let u = URL(string: s) else { throw GogFailure(errorDescription: L10n.gogNoLink) }
        return u
    }
}

// MARK: - What gets downloaded

/// One product's files for one language, as the depots of its build describe them.
struct GogPlan {
    struct Chunk {
        var md5: String
        var size: Int
        var packedMd5: String
    }

    struct File {
        /// The manifest's spelling when the app has one for the language.
        var path: String
        var size: Int64
        var chunks: [Chunk]
        /// The manifest's SHA-256, when there is one.
        var sha256: String?
        /// The GOG product whose CDN holds its chunks.
        var product: String
    }

    var installer: InstallerSpec
    var source: GogSource
    var manifestName: String?
    var files: [File]
    var dest: URL

    var bytes: Int64 { files.reduce(0) { $0 + $1.size } }
}

enum GogDepots {
    struct Build {
        var id: String
        var version: String
        var link: URL
    }

    /// GOG's language of the game's files for a launcher language code.
    static func depotLanguage(_ code: String) -> String {
        let map = ["en": "en-US", "ru": "ru-RU", "cs": "cs-CZ", "de": "de-DE", "es": "es-ES", "fr": "fr-FR",
                   "it": "it-IT", "hu": "hu-HU", "pl": "pl-PL", "zh-Hans": "zh-Hans"]
        return map[code] ?? code
    }

    /// The product's Windows builds (public, no sign-in), newest first.
    static func builds(_ product: String) async throws -> [Build] {
        let o = try await GogNet.json(URL(string: "https://content-system.gog.com/products/\(product)/os/windows/builds?generation=2")!)
        return (o["items"] as? [[String: Any]] ?? []).compactMap { i in
            guard let l = i["link"] as? String, let u = URL(string: l) else { return nil }
            return Build(id: "\(i["build_id"] ?? "")", version: i["version_name"] as? String ?? "", link: u)
        }
    }

    /// A build's or depot's description: zlib-packed JSON (sometimes plain).
    static func meta(_ d: Data) throws -> [String: Any] {
        let raw = d.first == UInt8(ascii: "{") ? d : GogNet.inflate(d) ?? d
        guard let o = try? JSONSerialization.jsonObject(with: raw) as? [String: Any] else {
            throw GogFailure(errorDescription: L10n.gogUnexpected("gog.com"))
        }
        return o
    }

    private static let cacheLock = NSLock()
    nonisolated(unsafe) private static var cache: [String: (Build, [String: Any])] = [:]

    /// The build the installer's manifests describe (launcher-games.json
    /// "gogBuild"), else the newest, and its description.
    static func build(for installer: InstallerSpec, id: String? = nil) async throws -> (Build, [String: Any]) {
        let want = id ?? installer.gogBuild
        let key = installer.gogId + "/" + (want ?? "")
        if let hit = cacheLock.withLock({ cache[key] }) { return hit }
        let all = try await builds(installer.gogId)
        guard let b = want.map({ w in all.first { $0.id == w } }) ?? all.first else {
            throw GogFailure(errorDescription: L10n.gogNoBuild(installer.title?.text ?? installer.folder))
        }
        let out = (b, try meta(try await GogNet.data(b.link)))
        cacheLock.withLock { cache[key] = out }
        return out
    }

    private static func depots(_ build: [String: Any], language: String) -> [[String: Any]] {
        (build["depots"] as? [[String: Any]] ?? []).filter { d in
            let langs = d["languages"] as? [String] ?? ["*"]
            return langs.contains("*") || langs.contains(language)
        }
    }

    /// About how much the language's depots hold (more than is kept).
    static func estimate(_ build: [String: Any], language: String) -> Int64 {
        depots(build, language: depotLanguage(language)).reduce(0) { $0 + ((($1["size"] as? NSNumber)?.int64Value) ?? 0) }
    }

    /// The files of `installer` in `language` (a launcher code) and where they go.
    static func plan(installer: InstallerSpec, language: String, buildID: String? = nil, config: LauncherConfig,
                     manifestDir: URL?, base: URL, progress: (String) -> Bool) async throws -> GogPlan {
        let (build, b) = try await build(for: installer, id: buildID)
        let host = build.link.host ?? "gog-cdn-fastly.gog.com"
        // The manifest of the same build in this language ("gog-<id>-ru.json").
        let manifestName = installer.manifests.first { $0.hasSuffix("-\(language).json") }
        var manifest: InstallManifest?
        if let name = manifestName, let dir = manifestDir,
           let data = FileManager.default.contents(atPath: dir.appendingPathComponent(name).path) {
            manifest = try? JSONDecoder().decode(InstallManifest.self, from: data)
        }
        var known: [String: InstallManifest.File] = [:]
        for f in manifest?.files ?? [] { known[f.path.lowercased()] = f }
        let exclude = config.exclude ?? []
        var files: [String: GogPlan.File] = [:], order: [String] = []
        for d in depots(b, language: depotLanguage(language)) {
            guard let m = d["manifest"] as? String, m.count > 4 else { continue }
            guard progress(m) else { throw CancellationError() }
            let product = "\(d["productId"] ?? installer.gogId)"
            let u = URL(string: "https://\(host)/content-system/v2/meta/\(m.prefix(2))/\(m.dropFirst(2).prefix(2))/\(m)")!
            let depot = try meta(try await GogNet.data(u))
            for it in (depot["depot"] as? [String: Any])?["items"] as? [[String: Any]] ?? [] {
                guard (it["type"] as? String) == "DepotFile", let raw = it["path"] as? String else { continue }
                let path = raw.replacingOccurrences(of: "\\", with: "/")
                let parts = path.split(separator: "/")
                guard !parts.isEmpty, !parts.contains(where: { $0 == ".." || $0 == "." }),
                      !Importer.excluded(path, exclude) else { continue }
                var chunks: [GogPlan.Chunk] = [], size: Int64 = 0
                for c in it["chunks"] as? [[String: Any]] ?? [] {
                    guard let md = c["md5"] as? String, let pm = c["compressedMd5"] as? String,
                          let s = (c["size"] as? NSNumber)?.intValue else { continue }
                    chunks.append(GogPlan.Chunk(md5: md, size: s, packedMd5: pm))
                    size += Int64(s)
                }
                let key = path.lowercased()
                let m = known[key]
                if files[key] == nil { order.append(key) }
                // A later depot (the language's) wins over the common one.
                files[key] = GogPlan.File(path: m?.path ?? path, size: size, chunks: chunks, sha256: m?.sha256,
                                          product: product)
            }
        }
        if let manifest {
            // GOG Galaxy's own installer extras (app/) are not in the depots.
            let missing = manifest.files.filter { !$0.path.lowercased().hasPrefix("app/") }
                .filter { files[$0.path.lowercased()]?.size != $0.size }
            if !missing.isEmpty {
                throw GogFailure(errorDescription: L10n.gogMissingFiles(missing.map(\.path)))
            }
        }
        guard !files.isEmpty else { throw GogFailure(errorDescription: L10n.gogMissingFiles([])) }
        return GogPlan(installer: installer, source: GogSource(product: installer.gogId, build: build.id, language: language),
                       manifestName: manifestName, files: order.compactMap { files[$0] },
                       dest: base.appendingPathComponent(installer.folder, isDirectory: true))
    }
}

// MARK: - Download

enum GogDownloader {
    /// Chunks fetched ahead of the one being written.
    static let parallel = 8

    /// Stands in for GOG's CDN in the launcher's tests: a chunk's packed bytes.
    nonisolated(unsafe) static var testPacked: ((GogPlan.Chunk) async throws -> Data)?

    /// `path` under `dest` through what is already there, each part matched
    /// without case (an iOS file system tells "Data" from "data", and the
    /// depots spell some folders both ways); parts not there yet keep `path`'s spelling.
    static func place(_ path: String, in dest: URL) -> URL {
        var url = dest
        for part in path.split(separator: "/").map(String.init) {
            url = Detector.entry(url, part) ?? url.appendingPathComponent(part)
        }
        return url
    }

    private static func size(_ url: URL) -> Int64 {
        (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.int64Value ?? 0
    }

    /// How much of `f` is already at `url` and checks out: whole chunks with
    /// GOG's MD5, and the SHA-256 of that much so far.
    private static func scan(_ f: GogPlan.File, at url: URL, onBytes: (Int) -> Bool) throws -> (Int, Int64, SHA256) {
        var hasher = SHA256()
        let have = size(url)
        guard have > 0, let h = FileHandle(forReadingAtPath: url.path) else { return (0, 0, hasher) }
        defer { try? h.close() }
        var done = 0, offset: Int64 = 0
        for c in f.chunks where offset + Int64(c.size) <= have {
            guard let d = try h.read(upToCount: c.size), d.count == c.size, GogNet.md5(d) == c.md5 else { break }
            hasher.update(data: d)
            offset += Int64(c.size)
            done += 1
            guard onBytes(c.size) else { throw CancellationError() }
        }
        return (done, offset, hasher)
    }

    private static func fetch(_ c: GogPlan.Chunk, product: String, links: GogLinks) async throws -> Data {
        var last: Error = GogFailure(errorDescription: L10n.gogDamaged)
        for attempt in 0..<5 {
            do {
                let z: Data
                if let test = testPacked {
                    z = try await test(c)
                } else {
                    let path = "/\(c.packedMd5.prefix(2))/\(c.packedMd5.dropFirst(2).prefix(2))/\(c.packedMd5)"
                    let url = try await links.url(product: product, path: path, renew: attempt > 1)
                    z = try await GogNet.data(url, tries: 1)
                }
                guard GogNet.md5(z) == c.packedMd5, let plain = GogNet.inflate(z, expected: c.size),
                      GogNet.md5(plain) == c.md5 else {
                    throw GogFailure(errorDescription: L10n.gogDamaged)
                }
                return plain
            } catch is CancellationError {
                throw CancellationError()
            } catch let e as GogFailure where e.signedOut {
                throw e
            } catch {
                last = error
            }
            try await Task.sleep(nanoseconds: UInt64(attempt + 1) * 700_000_000)
        }
        throw last
    }

    /// Downloads and checks what `plan` lists that is not already there and
    /// whole. Each callback reports the progress and checks for a cancel.
    static func run(_ plan: GogPlan, progress: (ImportProgress) -> Bool) async -> Result<Void, Error> {
        let fm = FileManager.default
        let dest = plan.dest
        var p = ImportProgress(name: plan.installer.title?.text ?? plan.installer.folder, phase: .checking)
        var last = Date.distantPast
        func report(_ force: Bool = false) throws {
            let now = Date()
            guard force || now.timeIntervalSince(last) > 0.2 else { return }
            last = now
            guard progress(p), !Task.isCancelled else { throw CancellationError() }
        }
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            #if os(iOS)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var d = dest
            try? d.setResourceValues(values)
            #endif
            let marker = InstallMarker(installer: plan.installer.id, setup: "GOG.com", manifest: plan.manifestName ?? "",
                                       games: plan.installer.games, gog: plan.source)
            let markerURL = dest.appendingPathComponent(InstallMarker.name)
            try JSONEncoder().encode(marker).write(to: markerURL)

            // What is there already: whole files and the start of others.
            var start: [(chunk: Int, offset: Int64)] = []
            var hashers: [Int: SHA256] = [:]
            let present = plan.files.map { Detector.entry(dest, $0.path) }
            p.filesTotal = plan.files.count
            p.bytesTotal = zip(plan.files, present).reduce(0) { sum, fp in sum + min(fp.0.size, fp.1.map(size) ?? 0) }
            try report(true)
            var broken: [String] = []
            for (i, f) in plan.files.enumerated() {
                p.current = f.path
                var s = (chunk: 0, offset: Int64(0))
                if let url = present[i] {
                    let (n, offset, h) = try scan(f, at: url) { bytes in
                        p.bytesDone += Int64(bytes)
                        return (try? report()) != nil
                    }
                    s = (n, offset)
                    hashers[i] = h
                    if n == f.chunks.count, offset == f.size {
                        if let want = f.sha256, h.finalize().map({ String(format: "%02x", $0) }).joined() != want {
                            // Whole by GOG's sums but not the manifest's: fetched again.
                            s = (0, 0)
                            hashers[i] = SHA256()
                        } else if size(url) != f.size {
                            try? FileHandle(forWritingTo: url).truncate(atOffset: UInt64(f.size))
                        }
                    }
                } else if f.chunks.isEmpty {
                    let url = place(f.path, in: dest)
                    try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                    fm.createFile(atPath: url.path, contents: nil)
                }
                start.append(s)
                p.filesDone = i + 1
                try report()
            }
            // The chunks still to fetch, in file order.
            var jobs: [(file: Int, chunk: Int)] = []
            for (i, f) in plan.files.enumerated() where start[i].chunk < f.chunks.count {
                for c in start[i].chunk..<f.chunks.count { jobs.append((i, c)) }
            }
            let need = jobs.reduce(Int64(0)) { $0 + Int64(plan.files[$1.file].chunks[$1.chunk].size) }
            if need > 0, let free = (try? dest.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
                .volumeAvailableCapacityForImportantUsage, free > 0, free < need + (64 << 20) {
                return .failure(GogFailure(errorDescription: L10n.noSpace(need, free)))
            }

            p.phase = .downloading
            p.filesTotal = plan.files.count
            p.filesDone = plan.files.indices.filter { start[$0].chunk == plan.files[$0].chunks.count }.count
            p.bytesTotal = plan.bytes
            p.bytesDone = plan.bytes - need
            try report(true)
            let links = GogLinks()
            var window: [Task<Data, Error>] = []
            var next = 0
            defer { for t in window { t.cancel() } }
            var handle: FileHandle?, current = -1
            defer { try? handle?.close() }
            for job in jobs {
                while window.count < parallel, next < jobs.count {
                    let j = jobs[next]
                    let c = plan.files[j.file].chunks[j.chunk], product = plan.files[j.file].product
                    window.append(Task { try await fetch(c, product: product, links: links) })
                    next += 1
                }
                let f = plan.files[job.file]
                if job.file != current {
                    try handle?.close()
                    handle = nil
                    let url = place(f.path, in: dest)
                    try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                    if !fm.fileExists(atPath: url.path) { fm.createFile(atPath: url.path, contents: nil) }
                    let h = try FileHandle(forWritingTo: url)
                    try h.truncate(atOffset: UInt64(start[job.file].offset))
                    try h.seekToEnd()
                    handle = h
                    current = job.file
                    if start[job.file].offset == 0 { hashers[job.file] = SHA256() }
                    p.current = f.path
                }
                let data = try await window.removeFirst().value
                do {
                    try handle?.write(contentsOf: data)
                } catch {
                    throw GogFailure(errorDescription: error.localizedDescription)
                }
                hashers[job.file]?.update(data: data)
                p.bytesDone += Int64(data.count)
                if job.chunk == f.chunks.count - 1 {
                    try handle?.synchronize()
                    try handle?.close()
                    handle = nil
                    current = -1
                    if let want = f.sha256, let h = hashers[job.file],
                       h.finalize().map({ String(format: "%02x", $0) }).joined() != want {
                        broken.append(f.path)
                        try? fm.removeItem(at: place(f.path, in: dest))
                    }
                    hashers[job.file] = nil
                    p.filesDone += 1
                }
                try report()
            }
            try report(true)
            if !broken.isEmpty {
                return .failure(GogFailure(errorDescription: L10n.gogVerifyFailed(broken)))
            }
            try fm.removeItem(at: markerURL)
            return .success(())
        } catch {
            return .failure(error)
        }
    }
}
