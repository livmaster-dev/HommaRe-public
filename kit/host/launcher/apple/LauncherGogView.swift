// LauncherGogView.swift - the GOG.com panel: sign in on GOG's own page, the
// account's games, download one straight into the app (LauncherGog.swift).
// The player's installer files are one button away, for a copy that is not
// in a GOG.com library or a device that has the installers already. The web
// view keeps nothing: cookies and the page's storage go with it, only the
// tokens GOG hands back are kept (the Keychain).
import SwiftUI
import WebKit

/// The panel over the launcher, in the game's look where its textures are here.
struct GogPanel: View {
    @Bindable var model: LauncherModel
    var skin: Skin?
    /// A phone held upright: one column.
    var tall = false
    /// The installer files instead (the Files app on iOS, a file panel on the Mac).
    var useInstallers: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: tall ? 12 : 16) {
            Text(L10n.gogTitle)
                .font(skin != nil ? SkinFont.menu(tall ? 30 : 38) : .system(size: 22, weight: .semibold, design: .serif))
                .foregroundStyle(skin != nil ? SkinColor.navy : Theme.text)
            note(L10n.gogIntro)
            // Scrolled only when it does not fit: a scroll view lays the
            // buttons' glow out in a box of its own.
            ViewThatFits(in: .vertical) {
                library
                ScrollView { library }
            }
            // The page of the game's look is as tall as it is; the dark card hugs what it holds.
            if skin != nil { Spacer(minLength: 0) }
            HStack(spacing: 14) {
                button(L10n.gogUseInstallers, primary: false, action: useInstallers)
                Spacer(minLength: 0)
                button(L10n.cancel, primary: false) {
                    withAnimation(.easeOut(duration: 0.2)) { model.gogShown = false }
                }
                .keyboardShortcut(.cancelAction)
            }
        }
        .modifier(GogPanelBackground(skin: skin, tall: tall))
    }

    /// The account and its games, or the way to sign in.
    private var library: some View {
        VStack(alignment: .leading, spacing: 14) {
            if let account = model.gogAccount {
                HStack(alignment: .firstTextBaseline, spacing: 12) {
                    label(account.isEmpty ? L10n.gogSignedIn : L10n.gogSignedInAs(account))
                    Spacer(minLength: 0)
                    button(L10n.gogSignOut, primary: false) { model.gogSignOut() }
                }
                HStack(alignment: .firstTextBaseline, spacing: 12) {
                    label(L10n.gogGameLanguage)
                    languageMenu
                    Spacer(minLength: 0)
                }
                divider
                ForEach(model.config.installers ?? []) { installer in
                    row(installer)
                    divider
                }
            } else {
                button(L10n.gogSignIn, primary: true) {
                    model.gogError = nil
                    model.gogLoginShown = true
                }
                .disabled(model.gogBusy)
            }
            if model.gogBusy {
                HStack(spacing: 10) {
                    ProgressView().controlSize(.small)
                    note(model.gogAccount == nil ? L10n.gogSigningIn : L10n.gogChecking)
                }
            }
            if let error = model.gogError {
                Text(error)
                    .font(skin != nil ? SkinFont.text(15) : .system(size: 13))
                    .foregroundStyle(skin != nil ? SkinColor.brown : Color(red: 0.98, green: 0.66, blue: 0.30))
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// One installer's games: whether the account owns them, how big, Download.
    @ViewBuilder private func row(_ installer: InstallerSpec) -> some View {
        let games = model.entries.filter { installer.games.contains($0.id) }
        let installed = !games.isEmpty && games.allSatisfy { $0.state == .ready }
        let interrupted = games.contains { $0.isInterrupted }
        let owned = model.gogOwned?.contains(installer.gogId)
        HStack(alignment: .center, spacing: 14) {
            VStack(alignment: .leading, spacing: 3) {
                label(installer.title?.text ?? installer.folder)
                note([installed ? L10n.ready : owned == true ? L10n.gogOwned : owned == false ? L10n.gogNotOwned : nil,
                      model.gogSizes[installer.id].map(L10n.gogDownloadSize)].compactMap { $0 }.joined(separator: " · "))
            }
            Spacer(minLength: 0)
            if !installed {
                button(interrupted ? L10n.resumeInstall : L10n.gogDownload, primary: true) {
                    model.downloadFromGog(installer)
                }
                .disabled(owned != true || model.importing != nil)
            }
        }
    }

    private var languageMenu: some View {
        Menu {
            ForEach(model.gogLanguages) { language in
                Button {
                    model.gogLanguage = language.code
                } label: {
                    if model.gogLanguage == language.code {
                        Label(language.name, systemImage: "checkmark")
                    } else {
                        Text(language.name)
                    }
                }
            }
        } label: {
            HStack(spacing: 8) {
                Text(L10n.languages.first { $0.code == model.gogLanguage }?.name ?? model.gogLanguage)
                    .font(skin != nil ? SkinFont.menu(22) : .system(size: 15, weight: .semibold))
                    .lineLimit(1)
                    .fixedSize()
                Image(systemName: "chevron.down").font(.system(size: 12, weight: .bold))
            }
            .foregroundStyle(skin != nil ? SkinColor.navy : Theme.gold)
            .contentShape(Rectangle())
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        #if os(macOS)
        .menuIndicator(.hidden)
        #endif
        .fixedSize()
    }

    private func label(_ text: String) -> some View {
        Text(text)
            .font(skin != nil ? SkinFont.menu(22) : .system(size: 15, weight: .semibold))
            .foregroundStyle(skin != nil ? SkinColor.navy : Theme.text)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func note(_ text: String) -> some View {
        Text(text)
            .font(skin != nil ? SkinFont.text(15) : .system(size: 13))
            .foregroundStyle(skin != nil ? SkinColor.inkMuted : Theme.dim)
            .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder private var divider: some View {
        if skin != nil {
            SkinDivider()
        } else {
            Rectangle().fill(Theme.stroke).frame(height: 1)
        }
    }

    @ViewBuilder private func button(_ title: String, primary: Bool, action: @escaping () -> Void) -> some View {
        if let skin {
            Button(title, action: action).buttonStyle(SkinButtonStyle(skin: skin, kind: primary ? .large : .small))
        } else if primary {
            Button(title, action: action).buttonStyle(GoldButtonStyle())
        } else {
            Button(title, action: action).buttonStyle(GhostButtonStyle())
        }
    }
}

/// The options page's parchment in the game's look, a dark card otherwise.
private struct GogPanelBackground: ViewModifier {
    var skin: Skin?
    var tall: Bool

    func body(content: Content) -> some View {
        if let skin {
            content
                .padding(.top, tall ? 56 : 60)
                .padding(.leading, tall ? 56 : 66)
                .padding(.trailing, tall ? 58 : 74)
                .padding(.bottom, tall ? 50 : 56)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .background {
                    if tall {
                        skin.sliced(.page, 64, 50, 50, 100)
                    } else {
                        skin.image(.page).resizable()
                    }
                }
                .shadow(color: .black.opacity(0.6), radius: 15, y: 18)
        } else {
            content
                .padding(22)
                .frame(maxWidth: 560)
                .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Theme.ink))
                .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(Theme.stroke))
                .shadow(color: .black.opacity(0.6), radius: 40)
        }
    }
}

/// GOG's sign-in page over the whole launcher, with a way back.
struct GogLoginOverlay: View {
    @Bindable var model: LauncherModel

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(L10n.gogSignIn)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Theme.text)
                Spacer()
                Button(L10n.cancel) { model.gogLoginShown = false }
                    .buttonStyle(GhostButtonStyle())
                    .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .background(Theme.ink)
            GogWebView(url: GogSession.loginURL) { code in model.gogSignedIn(code: code) }
        }
        .background(Theme.ink.ignoresSafeArea())
        .transition(.opacity)
    }
}

#if canImport(AppKit)
typealias GogViewRepresentable = NSViewRepresentable
#else
typealias GogViewRepresentable = UIViewRepresentable
#endif

/// GOG's sign-in page; hands over the code when the page lands on GOG's
/// "signed in" address, which is never loaded.
struct GogWebView: GogViewRepresentable {
    let url: URL
    let onCode: (String) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(onCode) }

    private func make(_ context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        let w = WKWebView(frame: .zero, configuration: config)
        w.navigationDelegate = context.coordinator
        w.uiDelegate = context.coordinator
        // GOG's page offers its full sign-in (and the Galaxy layout) to a desktop browser.
        w.customUserAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15"
        w.load(URLRequest(url: url))
        return w
    }

    #if canImport(AppKit)
    func makeNSView(context: Context) -> WKWebView { make(context) }
    func updateNSView(_ v: WKWebView, context: Context) {}
    #else
    func makeUIView(context: Context) -> WKWebView { make(context) }
    func updateUIView(_ v: WKWebView, context: Context) {}
    #endif

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
        let onCode: (String) -> Void
        private var sent = false

        init(_ onCode: @escaping (String) -> Void) { self.onCode = onCode }

        func webView(_ w: WKWebView, decidePolicyFor a: WKNavigationAction,
                     decisionHandler: @escaping @MainActor (WKNavigationActionPolicy) -> Void) {
            if let u = a.request.url, let code = GogSession.code(from: u) {
                decisionHandler(.cancel)
                if !sent {
                    sent = true
                    onCode(code)
                }
                return
            }
            decisionHandler(.allow)
        }

        /// A sign-in through another service (Steam, Discord...) opens a
        /// window of its own: shown in this one, so its way back lands here.
        func webView(_ w: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                     for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
            if action.targetFrame == nil { w.load(action.request) }
            return nil
        }
    }
}
