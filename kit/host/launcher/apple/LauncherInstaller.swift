// LauncherInstaller.swift - installing a game from the player's GOG offline
// installer (setup_*.exe and its *-N.bin, Inno Setup). innoextract unpacks
// only the files the install manifest lists into the app's Games folder: on
// the Mac the bundled helper (Contents/Helpers, kit/third_party/innoextract),
// on iOS, where an app cannot run another program, the same innoextract built
// into the app as a library (third_party/innoextract/build-ios.sh; the build
// sets RECOMP_INNOEXTRACT_LIB when it links it). Every file is checked against
// the manifest's SHA-256 before the game counts as installed. An interrupted
// install keeps what it unpacked and resumes. A GOG update (patch_*.exe) the
// player has beside the installer, or picks for an installed game, brings the
// manifest's files it carries (GogInstaller.unpackPatch).
import CryptoKit
import Foundation
#if os(macOS)
import Darwin
#elseif RECOMP_INNOEXTRACT_LIB
import RecompInnoextract
#endif

/// One GOG installer the app can install from (launcher-games.json "installers").
struct InstallerSpec: Decodable, Identifiable, Hashable {
    var id: String
    /// What innoextract --gog-game-id reports for it.
    var gogId: String
    /// The install folder under the app's Games folder.
    var folder: String
    var title: LocalizedText?
    /// Manifests in launcher-manifests/, one per build of the installer
    /// (languages, GOG revisions); the one whose files the installer holds wins.
    var manifests: [String]
    /// The games it installs (GameSpec.id).
    var games: [String]
    /// The GOG Galaxy build the manifests describe, which an install from
    /// GOG.com downloads (LauncherGog.swift); the newest when unset.
    var gogBuild: String?
}

/// Every file an install keeps, with its size and SHA-256 (no game data).
struct InstallManifest: Decodable {
    struct File: Decodable {
        var path: String
        var size: Int64
        var sha256: String
    }

    var gogId: String?
    var installer: String?
    var files: [File]
}

/// Sits in an install folder until every file checked out: the launcher shows
/// its games as an unfinished install and resumes from it.
struct InstallMarker: Codable {
    static let name = ".recomp-install.json"
    var installer: String
    var setup: String
    var manifest: String
    var games: [String]
    /// The innoextract that was unpacking into the folder (Mac).
    var helperPid: Int32?
    /// Bookmarks to what the player picked in the Files app that holds the
    /// installer (iOS), so a later launch may read it again to resume.
    var access: [Data]?
    /// An install downloaded from GOG.com rather than unpacked from an
    /// installer (`setup` is then "GOG.com"): resumed by downloading again.
    var gog: GogSource?

    static func read(_ root: URL) -> InstallMarker? {
        guard let data = FileManager.default.contents(atPath: root.appendingPathComponent(name).path) else { return nil }
        return try? JSONDecoder().decode(InstallMarker.self, from: data)
    }
}

/// Finds GOG offline installers in what the player picked or dropped: an
/// installer's .exe, any of its .bin slices, or folders holding them (up to two
/// levels down). Each setup_x.exe needs setup_x-1.bin beside it; innoextract
/// finds the slices by that name, so a slice a browser renamed on download
/// ("setup_x-1 (1).bin") is reported with a hint rather than used.
enum InstallerFinder {
    enum Problem: Hashable, Error {
        /// `exe` has no `expected` slice beside it; `renamed` looks like it.
        case missingSlice(exe: URL, expected: String, renamed: URL?)
        /// `bin` has no `expected` installer beside it; `renamed` looks like it.
        case missingSetup(bin: URL, expected: String, renamed: URL?)
    }

    struct Found {
        /// The installers' .exe files, in name order.
        var setups: [URL] = []
        /// GOG updates (patch_*.exe), in name order: applied after the installers.
        var patches: [URL] = []
        var problems: [Problem] = []
        /// Folders that held no installer at all, and files that are none.
        var emptyFolders: [URL] = []
    }

    /// "setup_x-1.bin" -> ("setup_x", 1); nil for other names.
    static func slice(_ name: String) -> (base: String, index: Int)? {
        let lower = name.lowercased()
        guard lower.hasSuffix(".bin") else { return nil }
        let stem = String(name.dropLast(4))
        guard let dash = stem.lastIndex(of: "-"), let n = Int(stem[stem.index(after: dash)...]), n > 0 else { return nil }
        return (String(stem[..<dash]), n)
    }

    /// A name with the copy counters browsers and Finder add ("x (1)",
    /// "x copy") taken out, for spotting a renamed file.
    static func normalized(_ stem: String) -> String {
        var s = stem.lowercased()
        for pattern in [#"\s+\(\d+\)"#, #"\s+copy(\s+\d+)?"#, #"\s+копия(\s+\d+)?"#] {
            s = s.replacingOccurrences(of: pattern, with: "", options: .regularExpression)
        }
        return s.trimmingCharacters(in: .whitespaces)
    }

    static func isSetupName(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.hasPrefix("setup_") && lower.hasSuffix(".exe")
    }

    /// GOG's update to a newer build of a game ("patch_x_2.1_v2_(28566)_to_2.1_v3_GOG_(83649).exe"):
    /// one program, no .bin slices.
    static func isPatchName(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.hasPrefix("patch_") && lower.hasSuffix(".exe")
    }

    private static func files(in dir: URL) -> [URL] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return [] }
        return names.filter { !$0.hasPrefix(".") }.sorted { $0.localizedStandardCompare($1) == .orderedAscending }
            .compactMap { name in
                let u = dir.appendingPathComponent(name)
                var isDir: ObjCBool = false
                return fm.fileExists(atPath: u.path, isDirectory: &isDir) && !isDir.boolValue ? u : nil
            }
    }

    private static func isDirectory(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue
    }

    /// Whether `exe` has its first slice beside it, or what is wrong.
    static func checkSlice(of exe: URL) -> Problem? {
        let dir = exe.deletingLastPathComponent()
        let base = exe.deletingPathExtension().lastPathComponent
        let expected = base + "-1.bin"
        if Detector.entry(dir, expected) != nil { return nil }
        let want = normalized(base)
        let renamed = files(in: dir).first { url in
            guard let s = slice(url.lastPathComponent) ?? slice(normalizedBinName(url.lastPathComponent)),
                  s.index == 1 else { return false }
            return normalized(s.base) == want
        }
        return .missingSlice(exe: exe, expected: expected, renamed: renamed)
    }

    /// "setup_x-1 (1).bin" -> "setup_x-1.bin", so the slice number still parses.
    private static func normalizedBinName(_ name: String) -> String {
        guard name.lowercased().hasSuffix(".bin") else { return name }
        return normalized(String(name.dropLast(4))) + ".bin"
    }

    /// The installer a slice belongs to, or what is wrong.
    static func setup(for bin: URL) -> Result<URL, Problem>? {
        guard let s = slice(bin.lastPathComponent) ?? slice(normalizedBinName(bin.lastPathComponent)) else { return nil }
        let dir = bin.deletingLastPathComponent()
        let expected = s.base + ".exe"
        if let exe = Detector.entry(dir, expected) { return .success(exe) }
        let want = normalized(s.base)
        let renamed = files(in: dir).first { url in
            url.pathExtension.lowercased() == "exe" && normalized(url.deletingPathExtension().lastPathComponent) == want
        }
        return .failure(.missingSetup(bin: bin, expected: expected, renamed: renamed))
    }

    static func find(in urls: [URL]) -> Found {
        var found = Found()
        var exes: [URL] = []
        func addExe(_ exe: URL) {
            let s = exe.standardizedFileURL
            if isPatchName(s.lastPathComponent) {
                if !found.patches.contains(s) { found.patches.append(s) }
            } else if !exes.contains(s) {
                exes.append(s)
            }
        }
        func addProblem(_ p: Problem) {
            if !found.problems.contains(p) { found.problems.append(p) }
        }
        for url in urls {
            if isDirectory(url) {
                var level = [url], any = false
                for _ in 0..<3 {
                    var next: [URL] = []
                    for dir in level {
                        let here = files(in: dir)
                        for f in here where isSetupName(f.lastPathComponent) || isPatchName(f.lastPathComponent) {
                            addExe(f)
                            any = true
                        }
                        // A slice whose installer is missing or renamed.
                        for f in here where slice(f.lastPathComponent) != nil || slice(normalizedBinName(f.lastPathComponent)) != nil {
                            switch setup(for: f) {
                            case .success(let exe)?: addExe(exe)
                            case .failure(let p)?: addProblem(p)
                            case nil: continue
                            }
                            any = true
                        }
                        next += Detector.children(of: dir)
                    }
                    level = next
                }
                if !any { found.emptyFolders.append(url) }
            } else if url.pathExtension.lowercased() == "exe" {
                addExe(url)
            } else if let r = setup(for: url) {
                switch r {
                case .success(let exe): addExe(exe)
                case .failure(let p): addProblem(p)
                }
            } else {
                found.emptyFolders.append(url)
            }
        }
        for exe in exes {
            if let p = checkSlice(of: exe) {
                // Some other program (not setup_*.exe, no slice in sight) is
                // not an installer at all.
                if case .missingSlice(_, _, nil) = p, !isSetupName(exe.lastPathComponent) {
                    found.emptyFolders.append(exe)
                } else {
                    addProblem(p)
                }
            } else {
                found.setups.append(exe)
            }
        }
        // A renamed installer shows up from both of its files; tell it once.
        let reported = Set(found.problems.compactMap { p -> URL? in
            if case .missingSlice(let exe, _, _) = p { return exe.standardizedFileURL }
            return nil
        })
        found.problems.removeAll { p in
            if case .missingSetup(_, _, let renamed?) = p { return reported.contains(renamed.standardizedFileURL) }
            return false
        }
        found.setups.sort { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
        found.patches.sort { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
        return found
    }

    /// The player's words for `problem`.
    static func describe(_ problem: Problem) -> String {
        switch problem {
        case .missingSlice(let exe, let expected, let renamed):
            if let renamed {
                // A slice with its own clean name: the installer is the renamed one.
                if let s = slice(renamed.lastPathComponent), normalized(s.base) == s.base.lowercased() {
                    return L10n.renamedSetup(renamed.lastPathComponent, s.base + ".exe", exe.lastPathComponent)
                }
                return L10n.renamedSlice(exe.lastPathComponent, expected, renamed.lastPathComponent)
            }
            return L10n.missingSlice(exe.lastPathComponent, expected)
        case .missingSetup(let bin, let expected, let renamed):
            if let renamed {
                return L10n.renamedSetup(bin.lastPathComponent, expected, renamed.lastPathComponent)
            }
            return L10n.missingSetup(bin.lastPathComponent, expected)
        }
    }
}

#if os(macOS) || RECOMP_INNOEXTRACT_LIB
enum GogInstaller {
    struct Failure: LocalizedError {
        var errorDescription: String?
    }

    struct Plan {
        var installer: InstallerSpec
        var manifestName: String
        var manifest: InstallManifest
        var setup: URL
        var dest: URL
        /// InstallMarker.access (iOS).
        var access: [Data] = []
        /// GOG updates found with the installer: an older build of it unpacks
        /// files the manifest (the newest build) does not have, which they bring.
        var patches: [URL] = []
    }

    #if os(macOS)
    private static let lock = NSLock()
    nonisolated(unsafe) private static var running: Process?

    /// The innoextract the app carries; RECOMP_INNOEXTRACT names another one.
    static func helper() -> URL? {
        let fm = FileManager.default
        if let p = ProcessInfo.processInfo.environment["RECOMP_INNOEXTRACT"], !p.isEmpty {
            return fm.isExecutableFile(atPath: p) ? URL(fileURLWithPath: p) : nil
        }
        let contents = Bundle.main.bundleURL.appendingPathComponent("Contents")
        for dir in ["Helpers", "MacOS"] {
            let u = contents.appendingPathComponent(dir).appendingPathComponent("innoextract")
            if fm.isExecutableFile(atPath: u.path) { return u }
        }
        return nil
    }

    /// Stops the helper when the launcher goes away mid-install.
    static func stopRunning() {
        lock.lock()
        let p = running
        lock.unlock()
        if let p, p.isRunning { p.terminate() }
    }

    /// Runs the helper to the end and returns what it printed.
    static func capture(_ helper: URL, _ args: [String]) throws -> String {
        let p = Process()
        p.executableURL = helper
        p.arguments = ["--color=0", "--progress=0"] + args
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        try p.run()
        var errData = Data()
        let errDone = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            errData = err.fileHandleForReading.readDataToEndOfFile()
            errDone.signal()
        }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        errDone.wait()
        guard p.terminationStatus == 0 else {
            throw Failure(errorDescription: lastLines(errData) ?? L10n.helperFailed(Int(p.terminationStatus)))
        }
        return String(decoding: data, as: UTF8.self)
    }

    private static func lastLines(_ data: Data) -> String? {
        let lines = String(decoding: data, as: UTF8.self).split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        return lines.isEmpty ? nil : lines.suffix(2).joined(separator: " ")
    }

    /// The paths a listing names (` - "bin/x.exe" [ru-RU], "bin2/x.exe" (1 MiB)`).
    static func listedPaths(_ listing: String) -> Set<String> {
        var out = Set<String>()
        for line in listing.split(whereSeparator: \.isNewline) where line.hasPrefix(" - \"") {
            for (i, part) in line.split(separator: "\"", omittingEmptySubsequences: false).enumerated()
            where i % 2 == 1 && !part.hasSuffix("/") {
                out.insert(part.lowercased())
            }
        }
        return out
    }
    #else
    /// Nothing to stop: a cancelled install ends in the launcher's own task.
    static func stopRunning() {}

    /// Runs a library call with room for its error text.
    private static func callLib(_ body: (UnsafeMutablePointer<CChar>, Int) -> Int32) throws {
        var error = [CChar](repeating: 0, count: 1024)
        let status = error.withUnsafeMutableBufferPointer { body($0.baseAddress!, $0.count) }
        switch Int(status) {
        case Int(RECOMP_INNO_OK):
            return
        case Int(RECOMP_INNO_CANCELLED):
            throw CancellationError()
        default:
            let text = String(cString: error)
            throw Failure(errorDescription: text.isEmpty ? L10n.helperFailed(Int(status)) : text)
        }
    }

    /// Makes sure the installer's files are on the device: a file the Files
    /// app shows from iCloud Drive or another provider may still be in the cloud.
    static func download(_ setup: URL) {
        let fm = FileManager.default
        let dir = setup.deletingLastPathComponent()
        let base = setup.deletingPathExtension().lastPathComponent
        var files = [setup]
        for n in 1...99 {
            let bin = dir.appendingPathComponent("\(base)-\(n).bin")
            guard fm.fileExists(atPath: bin.path)
                    || fm.fileExists(atPath: dir.appendingPathComponent(".\(bin.lastPathComponent).icloud").path)
            else { break }
            files.append(bin)
        }
        for url in files {
            var error: NSError?
            NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &error) { _ in }
        }
    }
    #endif

    /// The installer's GOG.com game id ("" when it has none).
    static func gogId(_ setup: URL) throws -> String {
        #if os(macOS)
        guard let helper = helper() else { throw Failure(errorDescription: L10n.noHelper) }
        let idText = try capture(helper, ["--gog-game-id", setup.path])
        return String(idText.components(separatedBy: "GOG.com game ID is ").dropFirst().first?
            .prefix(while: \.isNumber) ?? "")
        #else
        var id = [CChar](repeating: 0, count: 64)
        let size = id.count
        try callLib { error, errorSize in recomp_inno_gog_id(setup.path, &id, size, error, errorSize) }
        return String(cString: id)
        #endif
    }

    /// Every file path the installer holds, lower-cased.
    static func listing(_ setup: URL) throws -> Set<String> {
        #if os(macOS)
        guard let helper = helper() else { throw Failure(errorDescription: L10n.noHelper) }
        return listedPaths(try capture(helper, ["--list", setup.path]))
        #else
        final class Paths { var set = Set<String>() }
        let paths = Paths()
        try callLib { error, errorSize in
            recomp_inno_list(setup.path, { ctx, path in
                guard let ctx, let path else { return }
                Unmanaged<Paths>.fromOpaque(ctx).takeUnretainedValue().set.insert(String(cString: path).lowercased())
            }, Unmanaged.passUnretained(paths).toOpaque(), error, errorSize)
        }
        return paths.set
        #endif
    }

    /// SHA-256 of one file of `setup` (`path` in the install folder), unpacked
    /// alone; nil when the installer has none.
    static func digest(of path: String, in setup: URL) throws -> String? {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("recomp-probe-\(UUID().uuidString)", isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: dir) }
        #if os(macOS)
        guard let helper = helper() else { throw Failure(errorDescription: L10n.noHelper) }
        _ = try capture(helper, ["--extract", "--include", "/" + path, "--output-dir", dir.path, setup.path])
        #else
        let name = strdup(path)
        defer { free(name) }
        let paths: [UnsafePointer<CChar>?] = [UnsafePointer(name)]
        try paths.withUnsafeBufferPointer { list in
            try callLib { error, errorSize in
                recomp_inno_extract(setup.path, dir.path, list.baseAddress, 1, nil, nil, error, errorSize)
            }
        }
        #endif
        // The installer's spelling, which need not be the manifest's.
        let leaf = (path as NSString).lastPathComponent.lowercased()
        guard let file = fm.enumerator(at: dir, includingPropertiesForKeys: nil)?.compactMap({ $0 as? URL })
                .first(where: { $0.lastPathComponent.lowercased() == leaf }),
              let data = fm.contents(atPath: file.path) else { return nil }
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    /// Which installer `setup` is, which manifest describes it and where it
    /// goes; a reason for the player when it is none the app knows.
    static func plan(setup: URL, config: LauncherConfig, manifestDir: URL?, base: URL) -> Result<Plan, Error> {
        #if os(macOS)
        guard helper() != nil else { return .failure(Failure(errorDescription: L10n.noHelper)) }
        #endif
        if let problem = InstallerFinder.checkSlice(of: setup) {
            return .failure(Failure(errorDescription: InstallerFinder.describe(problem)))
        }
        do {
            let gogId = try gogId(setup)
            guard let installer = (config.installers ?? []).first(where: { $0.gogId == gogId }) else {
                return .failure(Failure(errorDescription: L10n.unknownInstaller(setup.lastPathComponent)))
            }
            let listed = try listing(setup)
            let fitting = installer.manifests.compactMap { name -> (String, InstallManifest)? in
                guard let dir = manifestDir,
                      let data = FileManager.default.contents(atPath: dir.appendingPathComponent(name).path),
                      let manifest = try? JSONDecoder().decode(InstallManifest.self, from: data),
                      manifest.files.allSatisfy({ listed.contains($0.path.lowercased()) })
                else { return nil }
                return (name, manifest)
            }
            // Two languages of a build can hold the same paths (English and
            // French do): GOG's description of the installer, which names its
            // language, tells them apart.
            var chosen = fitting.first
            if fitting.count > 1 {
                let info = "goggame-\(gogId).info"
                let sum = try digest(of: info, in: setup)
                chosen = fitting.first { $0.1.files.contains { $0.path.lowercased() == info && $0.sha256 == sum } }
            }
            if let (name, manifest) = chosen {
                return .success(Plan(installer: installer, manifestName: name, manifest: manifest, setup: setup,
                                     dest: base.appendingPathComponent(installer.folder, isDirectory: true)))
            }
            return .failure(Failure(errorDescription: L10n.otherInstallerBuild(setup.lastPathComponent)))
        } catch {
            return .failure(error)
        }
    }

    private static func size(_ url: URL) -> Int64? {
        (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.int64Value
    }

    /// SHA-256 of `url`, reporting each megabyte; nil when cancelled or unreadable.
    private static func digest(_ url: URL, onChunk: (Int) -> Bool) -> String? {
        guard let h = FileHandle(forReadingAtPath: url.path) else { return nil }
        defer { try? h.close() }
        var hasher = SHA256()
        while let chunk = try? h.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
            guard onChunk(chunk.count) else { return nil }
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// Checks `files` under `dest` against the manifest; removes the ones that
    /// differ and returns the ones that need unpacking.
    private static func check(_ files: [InstallManifest.File], in dest: URL, phase: ImportProgress.Phase,
                              p: inout ImportProgress, progress: (ImportProgress) -> Bool) throws -> [InstallManifest.File] {
        let present = files.filter { f in Detector.entry(dest, f.path).flatMap(size) == f.size }
        p.phase = phase
        p.filesDone = 0
        p.filesTotal = present.count
        p.bytesDone = 0
        p.bytesTotal = present.reduce(0) { $0 + $1.size }
        var good = Set<String>()
        for f in present {
            p.current = f.path
            guard progress(p) else { throw CancellationError() }
            guard let url = Detector.entry(dest, f.path) else { continue }
            var last = Date()
            let sum = digest(url) { n in
                p.bytesDone += Int64(n)
                if Date().timeIntervalSince(last) > 0.2 {
                    last = Date()
                    return progress(p)
                }
                return true
            }
            guard let sum else { throw CancellationError() }
            if sum == f.sha256 {
                good.insert(f.path)
            } else {
                try? FileManager.default.removeItem(at: url)
            }
            p.filesDone += 1
        }
        _ = progress(p)
        return files.filter { !good.contains($0.path) }
    }

    /// Unpacks and checks the install `plan` describes. Files already there
    /// and intact are kept, so a cancelled or interrupted run resumes.
    static func run(_ plan: Plan, progress: (ImportProgress) -> Bool) -> Result<Void, Error> {
        #if os(macOS)
        guard let helper = helper() else { return .failure(Failure(errorDescription: L10n.noHelper)) }
        #endif
        let fm = FileManager.default
        let dest = plan.dest
        let name = plan.installer.title?.text ?? plan.installer.folder
        var p = ImportProgress(name: name)
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            #if os(macOS)
            // A helper left from a launcher that died mid-install stops at its
            // next file (its output pipe is gone); make sure it is gone first.
            if let pid = InstallMarker.read(dest)?.helperPid { stopStale(pid, helper: helper) }
            #endif
            var marker = InstallMarker(installer: plan.installer.id, setup: plan.setup.path, manifest: plan.manifestName,
                                       games: plan.installer.games, access: plan.access.isEmpty ? nil : plan.access)
            let markerURL = dest.appendingPathComponent(InstallMarker.name)
            try JSONEncoder().encode(marker).write(to: markerURL)

            var pending = try check(plan.manifest.files, in: dest, phase: .checking, p: &p, progress: progress)
            if !pending.isEmpty {
                let need = pending.reduce(0) { $0 + $1.size }
                if let free = (try? dest.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
                    .volumeAvailableCapacityForImportantUsage, free > 0, free < need + (64 << 20) {
                    return .failure(Failure(errorDescription: L10n.noSpace(need, free)))
                }
                for f in pending {
                    if let stale = Detector.entry(dest, f.path) { try? fm.removeItem(at: stale) }
                }
                #if os(macOS)
                try extract(pending, plan: plan, helper: helper, p: &p, progress: progress) { pid in
                    marker.helperPid = pid
                    try? JSONEncoder().encode(marker).write(to: markerURL)
                }
                #else
                try extract(pending, plan: plan, p: &p, progress: progress)
                #endif
                pending = try check(pending, in: dest, phase: .verifying, p: &p, progress: progress)
                for patch in plan.patches where !pending.isEmpty {
                    p.phase = .patching
                    p.current = patch.lastPathComponent
                    guard progress(p) else { throw CancellationError() }
                    let unpacked = try unpackPatch(patch, into: dest.appendingPathComponent(patchStagingName, isDirectory: true))
                    defer { try? fm.removeItem(at: unpacked.dir) }
                    _ = try apply(unpacked, files: pending, root: dest)
                    pending = try check(pending, in: dest, phase: .verifying, p: &p, progress: progress)
                }
                if !pending.isEmpty {
                    return .failure(Failure(errorDescription: L10n.verifyFailed(pending.map(\.path))))
                }
            }
            try fm.removeItem(at: dest.appendingPathComponent(InstallMarker.name))
            return .success(())
        } catch {
            return .failure(error)
        }
    }

    /// `path` under `dest` through what is already there, each part matched
    /// without case; parts not there yet keep the manifest's spelling.
    private static func place(_ path: String, in dest: URL) -> URL {
        var url = dest
        for part in path.split(separator: "/").map(String.init) {
            url = Detector.entry(url, part) ?? url.appendingPathComponent(part)
        }
        return url
    }

    // MARK: GOG updates

    /// Where a GOG update is unpacked, inside the install folder.
    static let patchStagingName = ".recomp-patch"

    /// A GOG update (patch_*.exe), unpacked: an Inno Setup program without a
    /// GOG id or slices, which carries each file of the newer build it brings
    /// whole, zlib-compressed, under tmp/ (its own script, which the launcher
    /// does not run, puts them in place and edits the registry). A file it
    /// carries is a manifest's when its SHA-256 is the manifest's, so the
    /// manifest says where it goes.
    struct Patch {
        var url: URL
        var dir: URL
        /// SHA-256 -> the file, inflated, in `dir`.
        var files: [String: URL]
    }

    /// The bytes of a zlib stream (a two-byte header, raw DEFLATE, a
    /// checksum), or nil when `data` is none.
    static func inflate(_ data: Data) -> Data? {
        guard data.count > 6 else { return nil }
        let cmf = data[data.startIndex], flg = data[data.startIndex + 1]
        guard cmf & 0x0f == 8, cmf >> 4 <= 7, flg & 0x20 == 0, (UInt16(cmf) << 8 | UInt16(flg)) % 31 == 0 else {
            return nil
        }
        return try? (Data(data.dropFirst(2)) as NSData).decompressed(using: .zlib) as Data
    }

    /// Unpacks what `patch` carries into `dir` (emptied first), each file
    /// inflated and named by its SHA-256.
    static func unpackPatch(_ patch: URL, into dir: URL) throws -> Patch {
        let fm = FileManager.default
        try? fm.removeItem(at: dir)
        let raw = dir.appendingPathComponent("tmp-unpacked", isDirectory: true)
        try fm.createDirectory(at: raw, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: raw) }
        #if os(macOS)
        guard let helper = helper() else { throw Failure(errorDescription: L10n.noHelper) }
        _ = try capture(helper, ["--extract", "--include", "/tmp", "--output-dir", raw.path, patch.path])
        #else
        let tmp = strdup("tmp")
        defer { free(tmp) }
        let paths: [UnsafePointer<CChar>?] = [UnsafePointer(tmp)]
        try paths.withUnsafeBufferPointer { list in
            try callLib { error, errorSize in
                recomp_inno_extract(patch.path, raw.path, list.baseAddress, 1, nil, nil, error, errorSize)
            }
        }
        #endif
        var files: [String: URL] = [:]
        if let walk = fm.enumerator(at: raw, includingPropertiesForKeys: [.isRegularFileKey]) {
            for case let url as URL in walk {
                guard (try? url.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true,
                      let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { continue }
                let bytes = inflate(data) ?? data
                let sum = SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
                if files[sum] != nil { continue }
                let out = dir.appendingPathComponent(sum)
                try bytes.write(to: out)
                files[sum] = out
            }
        }
        return Patch(url: patch, dir: dir, files: files)
    }

    /// Puts each of `files` that `patch` carries in place under `root` where
    /// the file there is not that one already; how many it replaced.
    static func apply(_ patch: Patch, files: [InstallManifest.File], root: URL) throws -> Int {
        let fm = FileManager.default
        var replaced = 0
        for f in files {
            guard let source = patch.files[f.sha256] else { continue }
            let target = place(f.path, in: root)
            if size(target) == f.size, digest(target, onChunk: { _ in true }) == f.sha256 { continue }
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            let partial = target.deletingLastPathComponent().appendingPathComponent(".\(target.lastPathComponent).patch")
            try? fm.removeItem(at: partial)
            try fm.copyItem(at: source, to: partial)
            if fm.fileExists(atPath: target.path) {
                _ = try fm.replaceItemAt(target, withItemAt: partial)
            } else {
                try fm.moveItem(at: partial, to: target)
            }
            replaced += 1
        }
        return replaced
    }

    /// What a GOG update did to one install: the installer's title and how
    /// many files it replaced.
    struct PatchResult {
        var title: String
        var replaced: Int
    }

    /// Applies `patch` to the installs in `base` whose manifests know a file
    /// it carries (the manifest the install's files fit best, by size).
    static func applyPatch(_ patch: URL, config: LauncherConfig, manifestDir: URL?, base: URL,
                           progress: (ImportProgress) -> Bool) -> Result<[PatchResult], Error> {
        let fm = FileManager.default
        var p = ImportProgress(name: patch.lastPathComponent, phase: .patching, current: patch.lastPathComponent)
        guard progress(p) else { return .failure(CancellationError()) }
        let staging = base.appendingPathComponent(patchStagingName, isDirectory: true)
        do {
            try fm.createDirectory(at: base, withIntermediateDirectories: true)
            let unpacked = try unpackPatch(patch, into: staging)
            defer { try? fm.removeItem(at: staging) }
            var results: [PatchResult] = []
            var missing: [String] = []
            for installer in config.installers ?? [] {
                let manifests = installer.manifests.compactMap { name -> InstallManifest? in
                    guard let dir = manifestDir,
                          let data = fm.contents(atPath: dir.appendingPathComponent(name).path) else { return nil }
                    return try? JSONDecoder().decode(InstallManifest.self, from: data)
                }.filter { m in m.files.contains { unpacked.files[$0.sha256] != nil } }
                guard !manifests.isEmpty else { continue }
                let title = installer.title?.text ?? installer.folder
                let root = base.appendingPathComponent(installer.folder, isDirectory: true)
                guard fm.fileExists(atPath: root.path) else {
                    missing.append(title)
                    continue
                }
                func fits(_ m: InstallManifest) -> Int { m.files.filter { size(place($0.path, in: root)) == $0.size }.count }
                let fit = manifests.max { fits($0) < fits($1) }!
                p.name = title
                guard progress(p) else { throw CancellationError() }
                results.append(PatchResult(title: title, replaced: try apply(unpacked, files: fit.files, root: root)))
            }
            if results.isEmpty {
                return .failure(Failure(errorDescription: missing.isEmpty
                    ? L10n.unknownPatch(patch.lastPathComponent)
                    : L10n.patchNeedsGame(patch.lastPathComponent, missing.joined(separator: ", "))))
            }
            return .success(results)
        } catch {
            return .failure(error)
        }
    }

    #if os(macOS)
    /// Ends `pid` when it is still this app's innoextract.
    private static func stopStale(_ pid: Int32, helper: URL) {
        var buf = [CChar](repeating: 0, count: 4096)
        guard pid > 0, proc_pidpath(pid, &buf, UInt32(buf.count)) > 0,
              URL(fileURLWithPath: String(cString: buf)).resolvingSymlinksInPath().path
                == helper.resolvingSymlinksInPath().path
        else { return }
        kill(pid, SIGTERM)
        for _ in 0..<40 where kill(pid, 0) == 0 { Thread.sleep(forTimeInterval: 0.05) }
    }

    private static func extract(_ files: [InstallManifest.File], plan: Plan, helper: URL, p: inout ImportProgress,
                                progress: (ImportProgress) -> Bool, started: (Int32) -> Void) throws {
        let proc = Process()
        proc.executableURL = helper
        // -I /path keeps exactly the listed files (innoextract matches without case).
        proc.arguments = ["--extract", "--color=0", "--progress=0", "--output-dir", plan.dest.path]
            + files.flatMap { ["--include", "/" + $0.path] } + [plan.setup.path]
        proc.qualityOfService = .userInitiated
        let out = Pipe(), err = Pipe()
        proc.standardOutput = out
        proc.standardError = err
        let state = NSLock()
        var current = "", errText = Data()
        out.fileHandleForReading.readabilityHandler = { h in
            let text = String(decoding: h.availableData, as: UTF8.self)
            // innoextract names each file as it starts on it: ` - "data/data.pak"`.
            if let line = text.split(whereSeparator: \.isNewline).last(where: { $0.hasPrefix(" - \"") }) {
                let name = line.split(separator: "\"").dropFirst().first.map(String.init) ?? ""
                state.lock(); current = name; state.unlock()
            }
        }
        err.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            state.lock(); errText.append(d); state.unlock()
        }
        defer {
            out.fileHandleForReading.readabilityHandler = nil
            err.fileHandleForReading.readabilityHandler = nil
        }
        try proc.run()
        started(proc.processIdentifier)
        lock.lock(); running = proc; lock.unlock()
        defer { lock.lock(); running = nil; lock.unlock() }

        p.phase = .extracting
        p.filesTotal = files.count
        p.bytesTotal = files.reduce(0) { $0 + $1.size }
        let targets = files.map { (plan.dest.appendingPathComponent($0.path), $0.size) }
        func poll() {
            var done = 0, bytes: Int64 = 0
            for (url, want) in targets {
                let have = min(size(url) ?? 0, want)
                bytes += have
                if have == want { done += 1 }
            }
            p.filesDone = done
            p.bytesDone = bytes
            state.lock(); p.current = current; state.unlock()
        }
        while proc.isRunning {
            Thread.sleep(forTimeInterval: 0.25)
            poll()
            if !progress(p) {
                proc.terminate()
                proc.waitUntilExit()
                throw CancellationError()
            }
        }
        proc.waitUntilExit()
        poll()
        _ = progress(p)
        guard proc.terminationStatus == 0 else {
            state.lock(); let text = errText; state.unlock()
            throw Failure(errorDescription: lastLines(text) ?? L10n.helperFailed(Int(proc.terminationStatus)))
        }
    }
    #else
    /// Where innoextract unpacks on iOS, inside the install folder.
    static let stagingName = ".recomp-unpack"

    /// Moves what innoextract unpacked into `staging` to the manifest's paths
    /// under `dest`. An iOS file system tells "Data" from "data", and
    /// innoextract spells a folder the way the installer's first file in it
    /// does, which changes between runs that unpack different files (a
    /// resume); unpacking into an empty folder and moving each file into the
    /// folders already there keeps one copy of each. Partly written files are
    /// moved too: the next check finds them short and unpacks them again.
    private static func adoptStaged(_ staging: URL, into dest: URL, files: [InstallManifest.File]) {
        let fm = FileManager.default
        guard let walk = fm.enumerator(atPath: staging.path) else { return }
        var wanted: [String: String] = [:]
        for f in files { wanted[f.path.lowercased()] = f.path }
        while let rel = walk.nextObject() as? String {
            guard walk.fileAttributes?[.type] as? FileAttributeType == .typeRegular,
                  let path = wanted[rel.lowercased()] else { continue }
            let target = place(path, in: dest)
            try? fm.removeItem(at: target)
            try? fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.moveItem(at: staging.appendingPathComponent(rel), to: target)
        }
        try? fm.removeItem(at: staging)
    }

    /// Unpacks `files` with innoextract in this process (an iOS app runs no
    /// helpers). Each callback reports the bytes so far and checks for a
    /// cancel; the player sees it at most five times a second.
    private static func extract(_ files: [InstallManifest.File], plan: Plan, p: inout ImportProgress,
                                progress: (ImportProgress) -> Bool) throws {
        let staging = plan.dest.appendingPathComponent(stagingName, isDirectory: true)
        // What a launcher that died mid-install left there, then this run's.
        adoptStaged(staging, into: plan.dest, files: plan.manifest.files)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { adoptStaged(staging, into: plan.dest, files: plan.manifest.files) }
        p.phase = .extracting
        p.filesDone = 0
        p.filesTotal = files.count
        p.bytesDone = 0
        p.bytesTotal = files.reduce(0) { $0 + $1.size }
        _ = progress(p)
        final class State {
            var p: ImportProgress
            /// Dropped before withoutActuallyEscaping returns (it checks).
            var progress: ((ImportProgress) -> Bool)?
            var started = 0
            var last = Date.distantPast
            var going = true
            init(_ p: ImportProgress, _ progress: @escaping (ImportProgress) -> Bool) {
                self.p = p
                self.progress = progress
            }
        }
        let paths = files.map { strdup($0.path) }
        defer { paths.forEach { free($0) } }
        let cPaths = paths.map { UnsafePointer($0) }
        let start = p
        p = try withoutActuallyEscaping(progress) { progress -> ImportProgress in
            let state = State(start, progress)
            defer { state.progress = nil }
            try callLib { error, errorSize in
                recomp_inno_extract(plan.setup.path, staging.path, cPaths, cPaths.count, { ctx, current, bytes in
                    guard let ctx else { return 1 }
                    let state = Unmanaged<State>.fromOpaque(ctx).takeUnretainedValue()
                    // innoextract names each file as it starts on it.
                    if let current {
                        state.p.current = String(cString: current)
                        state.p.filesDone = state.started
                        state.started += 1
                    }
                    state.p.bytesDone = min(Int64(clamping: bytes), state.p.bytesTotal)
                    let now = Date()
                    if current != nil || now.timeIntervalSince(state.last) > 0.2 {
                        state.last = now
                        state.going = state.progress?(state.p) ?? false
                    }
                    return state.going ? 1 : 0
                }, Unmanaged.passUnretained(state).toOpaque(), error, errorSize)
            }
            return state.p
        }
        p.filesDone = files.count
        p.bytesDone = p.bytesTotal
        _ = progress(p)
    }
    #endif
}
#endif
