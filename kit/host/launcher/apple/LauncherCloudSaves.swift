// LauncherCloudSaves.swift - the launcher's half of the synced saves: the
// player's choice and folder, and the folder handed to the game (the host's
// cloud_saves.h does the syncing, in RECOMP_CLOUD_SAVES_DIR).
//
// No iCloud entitlement is needed, so any build (a DMG's, a re-signed IPA's)
// can do it. A Mac app outside the sandbox writes straight into iCloud Drive's
// folder, ~/Library/Mobile Documents/com~apple~CloudDocs, which the system
// syncs. On iOS the player picks the folder in the Files app's picker, and a
// security-scoped bookmark of it keeps the app's access across starts.
import Foundation
import SwiftUI

#if canImport(AppKit)
import AppKit
#endif

extension LauncherModel {
    static let cloudSavesKey = "launcher.cloudSaves"
    /// macOS: the folder's path when the player chose one; iOS: its bookmark.
    static let cloudFolderKey = "launcher.cloudSavesFolder"

    #if os(macOS)
    /// iCloud Drive's own folder, when iCloud Drive is on.
    static var iCloudDrive: URL? {
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Mobile Documents/com~apple~CloudDocs", isDirectory: true)
        var dir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &dir) && dir.boolValue ? url : nil
    }
    #endif

    static func savedCloudFolder() -> URL? {
        #if os(macOS)
        if let path = UserDefaults.standard.string(forKey: cloudFolderKey), !path.isEmpty {
            return URL(fileURLWithPath: path, isDirectory: true)
        }
        return iCloudDrive?.appendingPathComponent("HommaRe", isDirectory: true)
        #else
        guard let data = UserDefaults.standard.data(forKey: cloudFolderKey) else { return nil }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
        else { return nil }
        if stale, url.startAccessingSecurityScopedResource() {
            defer { url.stopAccessingSecurityScopedResource() }
            if let fresh = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                UserDefaults.standard.set(fresh, forKey: cloudFolderKey)
            }
        }
        return url
        #endif
    }

    /// How the folder reads in the settings: "iCloud Drive › HommaRe" for one
    /// in iCloud Drive, else its name (iOS) or its path (macOS).
    static func cloudFolderName(_ url: URL) -> String {
        let marker = "/Mobile Documents/com~apple~CloudDocs"
        let path = url.standardizedFileURL.path
        if let range = path.range(of: marker) {
            let rest = path[range.upperBound...].split(separator: "/")
            return ([L10n.cloudSaves] + rest.map(String.init)).joined(separator: " › ")
        }
        #if os(iOS)
        return url.lastPathComponent
        #else
        return L10n.location(url)
        #endif
    }

    /// The checkbox: on asks for a folder first where there is none.
    func setCloudSaves(_ on: Bool) {
        guard on else {
            cloudSaves = false
            UserDefaults.standard.set(false, forKey: Self.cloudSavesKey)
            return
        }
        guard cloudFolder != nil else {
            #if os(macOS)
            notice = L10n.cloudNoDrive
            #endif
            chooseCloudFolder()
            return
        }
        cloudSaves = true
        UserDefaults.standard.set(true, forKey: Self.cloudSavesKey)
    }

    func chooseCloudFolder() {
        #if os(macOS)
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = L10n.choose
        panel.directoryURL = cloudFolder?.deletingLastPathComponent() ?? Self.iCloudDrive
        if panel.runModal() == .OK, let url = panel.url {
            useCloudFolder(url)
        }
        #else
        cloudPickerShown = true
        #endif
    }

    /// The folder the player chose (on iOS from the picker, readable for now).
    func useCloudFolder(_ url: URL) {
        #if os(macOS)
        UserDefaults.standard.set(url.path, forKey: Self.cloudFolderKey)
        #else
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            notice = L10n.cloudLost
            return
        }
        UserDefaults.standard.set(data, forKey: Self.cloudFolderKey)
        #endif
        cloudFolder = url
        cloudSaves = true
        UserDefaults.standard.set(true, forKey: Self.cloudSavesKey)
    }

    /// Play: the folder goes to the game in RECOMP_CLOUD_SAVES_DIR (and to the
    /// modules the app hands over to, the same process). iOS keeps the
    /// security scope open for the rest of the run.
    func handCloudSavesToGame() {
        guard cloudSaves else { return }
        guard let url = cloudFolder else {
            fputs("[launcher] cloud saves: no folder\n", stderr)
            return
        }
        #if os(iOS)
        // A folder picked in the Files app (its bookmark's URL): readable
        // while the scope is open. A plain path (the test hook) has none.
        let scoped = url.startAccessingSecurityScopedResource()
        guard scoped || FileManager.default.isReadableFile(atPath: url.path) else {
            fputs("[launcher] cloud saves: the folder cannot be opened\n", stderr)
            return
        }
        #else
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        #endif
        setenv("RECOMP_CLOUD_SAVES_DIR", url.path, 1)
        fputs("[launcher] cloud saves: \(url.path)\n", stderr)
    }
}

/// The settings' row: the checkbox, Choose, the folder.
struct CloudSavesRow: View {
    @Bindable var model: LauncherModel
    let skin: Skin
    var size: CGFloat = 22

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 14) {
                SkinCheck(skin: skin, title: L10n.cloudSavesOn, on: model.cloudSaves, size: size) {
                    model.setCloudSaves(!model.cloudSaves)
                }
                Button(L10n.choose) { model.chooseCloudFolder() }
                    .buttonStyle(SkinButtonStyle(skin: skin, kind: .small))
            }
            if let folder = model.cloudFolder {
                Text(L10n.cloudFolder(LauncherModel.cloudFolderName(folder)))
                    .font(SkinFont.text(size - 7))
                    .foregroundStyle(SkinColor.inkMuted)
                    .lineLimit(1)
                    .truncationMode(.middle)
            } else {
                #if os(iOS)
                Text(L10n.cloudPickHint)
                    .font(SkinFont.text(size - 7))
                    .foregroundStyle(SkinColor.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
                #endif
            }
        }
        #if os(iOS)
        .fileImporter(isPresented: $model.cloudPickerShown, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result { model.useCloudFolder(url) }
        }
        #endif
    }
}
