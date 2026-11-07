// LauncherStrings.swift - the launcher's own words, in the languages the GOG
// installers come in (en ru cs de es fr it hu pl zh-Hans), picked from the
// player's preferred languages or chosen in Settings. The texts live in
// launcher-strings.json, built into the app as LauncherCatalog.swift
// (kit/tools/launcher_strings.py --write); English stands in for a text a
// language lacks. Game titles and blurbs come from launcher-games.json.
import Foundation

enum L10n {
    /// A language the launcher speaks: its code and its own name.
    struct Language: Identifiable, Hashable {
        var code: String
        var name: String
        var id: String { code }
    }

    /// The languages the launcher speaks, in the catalog's order.
    static let languages: [Language] = catalog.languages

    /// The player's choice in Settings (a code of `languages`), nil to follow the system.
    static var chosen: String? = UserDefaults.standard.string(forKey: "launcher.language") {
        didSet { UserDefaults.standard.set(chosen, forKey: "launcher.language") }
    }

    static var language: String {
        // RECOMP_LAUNCHER_LANG=<code> picks one for screenshots and tests.
        if let forced = ProcessInfo.processInfo.environment["RECOMP_LAUNCHER_LANG"], known(forced) {
            return forced
        }
        if let chosen, known(chosen) { return chosen }
        return system
    }

    private static func known(_ code: String) -> Bool {
        languages.contains { $0.code == code }
    }

    /// The first of the player's preferred languages the launcher speaks:
    /// "zh-Hans-CN" is zh-Hans, traditional Chinese is not; English otherwise.
    private static let system: String = {
        for id in Locale.preferredLanguages {
            let parts = id.split(separator: "-").map(String.init)
            guard let first = parts.first?.lowercased() else { continue }
            if first == "zh" {
                let script = Locale(identifier: id).language.script?.identifier
                if script == "Hans" || (script == nil && parts.contains { ["CN", "SG"].contains($0) }) {
                    return "zh-Hans"
                }
                continue
            }
            if known(first) { return first }
        }
        return "en"
    }()

    private struct Catalog {
        var languages: [Language] = []
        var strings: [String: [String: String]] = [:]
    }

    private static let catalog: Catalog = {
        var c = Catalog()
        guard let root = try? JSONSerialization.jsonObject(with: Data(LauncherCatalog.json.utf8)) as? [String: Any]
        else { return c }
        for l in root["languages"] as? [[String: Any]] ?? [] {
            if let code = l["code"] as? String, let name = l["name"] as? String { c.languages.append(Language(code: code, name: name)) }
        }
        for (key, value) in root["strings"] as? [String: [String: Any]] ?? [:] {
            c.strings[key] = value.compactMapValues { $0 as? String }
        }
        return c
    }()

    /// The text `key` names in the launcher's language, its {placeholders}
    /// filled from `args`.
    static func t(_ key: String, _ args: [String: String] = [:]) -> String {
        let entry = catalog.strings[key]
        var text = entry?[language] ?? entry?["en"] ?? key
        for (name, value) in args {
            text = text.replacingOccurrences(of: "{\(name)}", with: value)
        }
        return text
    }

    static var play: String { t("play") }
    static var games: String { t("games") }
    static var choose: String { t("choose") }
    static var chooseFolder: String { t("chooseFolder") }
    static var importFolder: String { t("importFolder") }
    static var showInFinder: String { t("showInFinder") }
    static var saves: String { t("saves") }
    static var quit: String { t("quit") }
    static var searching: String { t("searching") }
    static var refresh: String { t("refresh") }
    static var cancel: String { t("cancel") }
    static var ok: String { t("ok") }
    static var install: String { t("install") }
    static var installFromSetup: String { t("installFromSetup") }
    static var resumeInstall: String { t("resumeInstall") }
    static var pickInstallerMessage: String { t("pickInstallerMessage") }

    static var pickMessage: String { t("pickMessage") }

    static var chooseGame: String { t("chooseGame") }
    static var settings: String { t("settings") }
    static var installFromGog: String { t("installFromGog") }
    static var done: String { t("done") }
    static var languageLabel: String { t("languageLabel") }
    static var languageSystem: String { t("languageSystem") }
    static var frameLimit: String { t("frameLimit") }
    static var frameLimitGame: String { t("frameLimitGame") }
    static var energySaver: String { t("energySaver") }
    static var renderQuality: String { t("renderQuality") }
    static var renderNative: String { t("renderNative") }
    static var renderQualityChoice: String { t("renderQualityChoice") }
    static var renderBalanced: String { t("renderBalanced") }
    static var renderPerformance: String { t("renderPerformance") }
    static var idleFrames: String { t("idleFrames") }
    static var auto: String { t("auto") }
    static var always: String { t("always") }
    static var never: String { t("never") }
    static var on: String { t("on") }
    static var off: String { t("off") }
    static var uiBounds: String { t("uiBounds") }
    static var uiBoundsScreen: String { t("uiBoundsScreen") }
    static var startupVideos: String { t("startupVideos") }
    static var showVideos: String { t("showVideos") }
    static var skipVideos: String { t("skipVideos") }
    static var cameraButtons: String { t("cameraButtons") }
    static var commandPanel: String { t("commandPanel") }
    static var hideButtons: String { t("hideButtons") }
    static var battleCells: String { t("battleCells") }
    static var touchMode: String { t("touchMode") }
    static var touchNative: String { t("touchNative") }
    static var touchCursor: String { t("touchCursor") }
    static var phoneInterface: String { t("phoneInterface") }
    static var silentMode: String { t("silentMode") }
    static var respect: String { t("respect") }
    static var ignore: String { t("ignore") }
    static var trackpad: String { t("trackpad") }
    static var metalHud: String { t("metalHud") }
    static var metalHudShow: String { t("metalHudShow") }
    static var nextStart: String { t("nextStart") }
    static var gpuSkin: String { t("gpuSkin") }
    static var gpuSkinOn: String { t("gpuSkinOn") }
    static var menus: String { t("menus") }
    static var menusNative: String { t("menusNative") }
    static var menusGame: String { t("menusGame") }
    static var trackpadHeight: String { t("trackpadHeight") }
    static var trackpadTablet: String { t("trackpadTablet") }
    static var trackpadGame: String { t("trackpadGame") }
    static var settingNotSaved: String { t("settingNotSaved") }
    static var gameFolders: String { t("gameFolders") }
    static var noFolders: String { t("noFolders") }
    static var installsGoTo: String { t("installsGoTo") }
    static var addFolder: String { t("addFolder") }
    static var savesFolder: String { t("savesFolder") }
    static var cloudSaves: String { t("cloudSaves") }
    static var cloudSavesOn: String { t("cloudSavesOn") }
    static func cloudFolder(_ folder: String) -> String { t("cloudFolder", ["folder": folder]) }
    static var cloudPickHint: String { t("cloudPickHint") }
    static var cloudNoDrive: String { t("cloudNoDrive") }
    static var cloudLost: String { t("cloudLost") }
    static var show: String { t("show") }
    static var readyToPlay: String { t("readyToPlay") }
    static var phaseCopying: String { t("phaseCopying") }
    static var phaseChecking: String { t("phaseChecking") }
    static var phaseUnpacking: String { t("phaseUnpacking") }
    static var phaseVerifying: String { t("phaseVerifying") }
    static var phasePatching: String { t("phasePatching") }
    static var portInTheWorks: String { t("portInTheWorks") }
    static var installTitle: String { t("installTitle") }
    static var importTitle: String { t("importTitle") }
    static var version: String { t("version") }

    static func fileOf(_ done: Int, _ total: Int) -> String {
        t("fileOf", ["done": "\(done)", "total": "\(total)"])
    }

    static var ready: String { t("ready") }
    static var notFound: String { t("notFound") }
    static var wrongVersion: String { t("wrongVersion") }
    static var incomplete: String { t("incomplete") }
    static var interrupted: String { t("interrupted") }

    static func interruptedDetail(_ setup: String) -> String {
        // A download from GOG.com (InstallMarker.setup "GOG.com") unpacks nothing.
        if setup == "GOG.com" { return t("gogInterruptedDetail") }
        return t("interruptedDetail", ["setup": (setup as NSString).lastPathComponent])
    }

    static var notFoundDetail: String {
        #if os(iOS) && RECOMP_INNOEXTRACT_LIB
        t("notFoundDetail.iosInstaller")
        #elseif os(iOS)
        t("notFoundDetail.ios")
        #else
        t("notFoundDetail.mac")
        #endif
    }

    static func wrongVersionDetail(_ digest: String) -> String {
        t("wrongVersionDetail", ["digest": String(digest.prefix(12))])
    }

    static func incompleteDetail(_ missing: [String]) -> String {
        t("incompleteDetail", ["list": missing.joined(separator: ", ")])
    }

    static var comingSoonDetail: String { t("comingSoonDetail") }

    static func noGameIn(_ path: String) -> String { t("noGameIn", ["path": path]) }

    static var installerOnPhone: String { t("installerOnPhone") }

    static func importFailed(_ reason: String) -> String { t("importFailed", ["reason": reason]) }

    static func installFailed(_ reason: String) -> String { t("installFailed", ["reason": reason]) }

    static var noHelper: String { t("noHelper") }

    static func missingSlice(_ exe: String, _ slice: String) -> String {
        t("missingSlice", ["exe": exe, "slice": slice])
    }

    static func renamedSlice(_ exe: String, _ slice: String, _ found: String) -> String {
        t("renamedSlice", ["exe": exe, "slice": slice, "found": found])
    }

    static func missingSetup(_ bin: String, _ exe: String) -> String {
        t("missingSetup", ["bin": bin, "exe": exe])
    }

    static func renamedSetup(_ bin: String, _ exe: String, _ found: String) -> String {
        t("renamedSetup", ["bin": bin, "exe": exe, "found": found])
    }

    static func noInstallerIn(_ name: String) -> String { t("noInstallerIn", ["name": name]) }

    /// iOS: the Files app lets the app read only what the player picked.
    static func pickBoth(_ picked: String, _ other: String) -> String {
        t("pickBoth", ["picked": picked, "other": other])
    }

    static func notInstaller(_ name: String) -> String { t("notInstaller", ["name": name]) }

    static var dropHint: String { t("dropHint") }

    static func unknownInstaller(_ name: String) -> String { t("unknownInstaller", ["name": name]) }

    static func otherInstallerBuild(_ name: String) -> String { t("otherInstallerBuild", ["name": name]) }

    static func setupGone(_ path: String) -> String { t("setupGone", ["path": path]) }

    static func helperFailed(_ status: Int) -> String { t("helperFailed", ["status": "\(status)"]) }

    static func verifyFailed(_ paths: [String]) -> String {
        let list = paths.prefix(3).joined(separator: ", ") + (paths.count > 3 ? "…" : "")
        return t("verifyFailed", ["count": "\(paths.count)", "list": list])
    }

    static func installing(_ name: String) -> String { t("installing", ["name": name]) }

    static func checking(_ name: String) -> String { t("checking", ["name": name]) }

    static func verifying(_ name: String) -> String { t("verifying", ["name": name]) }

    /// A GOG update (patch_….exe) being applied, and how it went.
    static func patching(_ name: String) -> String { t("patching", ["name": name]) }

    static func patchApplied(_ name: String) -> String { t("patchApplied", ["name": name]) }

    static func patchUpToDate(_ name: String) -> String { t("patchUpToDate", ["name": name]) }

    static func patchNeedsGame(_ patch: String, _ name: String) -> String {
        t("patchNeedsGame", ["patch": patch, "name": name])
    }

    static func unknownPatch(_ name: String) -> String { t("unknownPatch", ["name": name]) }

    /// Sizes in the launcher's language, not only the system's.
    private static func bytes(_ count: Int64) -> String {
        count.formatted(.byteCount(style: .file).locale(Locale(identifier: language)))
    }

    static func progressOf(_ done: Int64, _ total: Int64) -> String {
        t("progressOf", ["done": bytes(done), "total": bytes(total)])
    }

    static func importing(_ name: String) -> String { t("importing", ["name": name]) }

    static func noSpace(_ need: Int64, _ free: Int64) -> String {
        t("noSpace", ["need": bytes(need), "free": bytes(free)])
    }

    /// Where a game lives, for people: the folder's name on iOS (always the
    /// app's own storage there), the path with ~ for the home folder on the Mac.
    static func location(_ root: URL) -> String {
        #if os(iOS)
        return t("location.ios", ["folder": root.lastPathComponent])
        #else
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let path = root.path
        return path.hasPrefix(home + "/") ? "~" + path.dropFirst(home.count) : path
        #endif
    }

    static var defaultLegal: String { t("defaultLegal") }

    // The GOG.com panel and download (LauncherGog.swift, LauncherGogView.swift).
    static var gogTitle: String { t("gogTitle") }
    static var gogIntro: String { t("gogIntro") }
    static var gogSignIn: String { t("gogSignIn") }
    static var gogSignOut: String { t("gogSignOut") }
    static var gogSignedIn: String { t("gogSignedIn") }
    static func gogSignedInAs(_ name: String) -> String { t("gogSignedInAs", ["name": name]) }
    static var gogSigningIn: String { t("gogSigningIn") }
    static var gogChecking: String { t("gogChecking") }
    static var gogOwned: String { t("gogOwned") }
    static var gogNotOwned: String { t("gogNotOwned") }
    static var gogDownload: String { t("gogDownload") }
    static var gogGameLanguage: String { t("gogGameLanguage") }
    static var gogUseInstallers: String { t("gogUseInstallers") }
    static func gogDownloadSize(_ size: Int64) -> String { t("gogDownloadSize", ["size": bytes(size)]) }
    static func gogSignInFailed(_ reason: String) -> String { t("gogSignInFailed", ["reason": reason]) }
    static var gogExpired: String { t("gogExpired") }
    static var gogSignInFirst: String { t("gogSignInFirst") }
    static var gogKeychain: String { t("gogKeychain") }
    static func gogNotInLibrary(_ name: String) -> String { t("gogNotInLibrary", ["name": name]) }
    static func gogNoBuild(_ name: String) -> String { t("gogNoBuild", ["name": name]) }
    static var gogNoLink: String { t("gogNoLink") }

    static func gogMissingFiles(_ paths: [String]) -> String {
        t("gogMissingFiles", ["list": paths.prefix(3).joined(separator: ", ") + (paths.count > 3 ? "…" : "")])
    }

    static func gogVerifyFailed(_ paths: [String]) -> String {
        let list = paths.prefix(3).joined(separator: ", ") + (paths.count > 3 ? "…" : "")
        return t("gogVerifyFailed", ["count": "\(paths.count)", "list": list])
    }

    static var gogDamaged: String { t("gogDamaged") }
    static func gogHttp(_ host: String, _ code: Int) -> String { t("gogHttp", ["host": host, "code": "\(code)"]) }
    static func gogUnexpected(_ host: String) -> String { t("gogUnexpected", ["host": host]) }
    static func downloading(_ name: String) -> String { t("downloading", ["name": name]) }
    static var phaseDownloading: String { t("phaseDownloading") }
}
