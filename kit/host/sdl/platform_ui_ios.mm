// platform_ui_ios.mm - the SDL host on iPadOS: one fullscreen Metal window, the
// game in Documents/game (imported by the launcher, from the bundle of a
// developer build or from files the player chose), the launcher's platform,
// and the app lifecycle mapped onto audio and presentation.
#include "platform_ui.h"

#include "../../platform/os.h"
#include "../../runtime/layout.h"
#include "../../runtime/native_seam.h"
#include "../audio.h"
#include "../launcher/launcher_sdl.h"
#include "../present.h"
#include "game_config.h"

// A game module (RECOMP_GAME_MODULE) is loaded into an app that has these
// classes too, and Objective-C has one class per name in a process: each
// module's copies take its game's name.
#if defined(RECOMP_GAME_MODULE)
#define RECOMP_OBJC_CAT_(a, b) a##_##b
#define RECOMP_OBJC_CAT(a, b) RECOMP_OBJC_CAT_(a, b)
#define RecompPickerDelegate RECOMP_OBJC_CAT(RecompPickerDelegate, RECOMP_GAME_SYMBOL)
#define RecompCameraBar RECOMP_OBJC_CAT(RecompCameraBar, RECOMP_GAME_SYMBOL)
#define RecompRoundButton RECOMP_OBJC_CAT(RecompRoundButton, RECOMP_GAME_SYMBOL)
#define RecompSplitRing RECOMP_OBJC_CAT(RecompSplitRing, RECOMP_GAME_SYMBOL)
#define RecompPanelCommand RECOMP_OBJC_CAT(RecompPanelCommand, RECOMP_GAME_SYMBOL)
#define RecompPanelArt RECOMP_OBJC_CAT(RecompPanelArt, RECOMP_GAME_SYMBOL)
#define RecompControlPanel RECOMP_OBJC_CAT(RecompControlPanel, RECOMP_GAME_SYMBOL)
#endif

#if !__has_feature(objc_arc)
#error "platform_ui_ios.mm is written for ARC (host/CMakeLists.txt)"
#endif

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <sys/stat.h>
#include <vector>
#include <dlfcn.h>
#include <filesystem>
#include <objc/runtime.h>
#include <atomic>
#include <thread>
#include <unistd.h>

#include "../../mods/settings_file.h"
#include <fstream>
#include <stdio.h>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string bundle_dir() {
    char path[4096];
    if (os_exe_path(path, sizeof path) != 0)
        return "";
    return fs::path(path).parent_path().string();
}

std::string documents_dir() {
    NSArray *paths =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return paths.count ? std::string([paths[0] fileSystemRepresentation]) : "";
}

// The run before's log, kept where a player can reach it (the Files app shows
// the app's Documents): Documents/Logs/host-<when it ended>.log, the newest
// ten. A run that crashed is otherwise gone two launches later.
void archive_previous_log(const std::string &prev) {
    std::error_code ec;
    if (!fs::exists(prev, ec) || fs::file_size(prev, ec) == 0)
        return;
    NSArray *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (!docs.count)
        return;
    const fs::path logs = fs::path([docs[0] fileSystemRepresentation]) / "Logs";
    fs::create_directories(logs, ec);
    struct stat st{};
    if (stat(prev.c_str(), &st) != 0)
        return;
    char name[64];
    struct tm tm{};
    localtime_r(&st.st_mtime, &tm);
    strftime(name, sizeof name, "host-%Y-%m-%d_%H%M%S.log", &tm);
    fs::copy_file(prev, logs / name, fs::copy_options::overwrite_existing, ec);
    std::vector<fs::path> kept;
    for (const auto &entry : fs::directory_iterator(logs, ec))
        if (entry.path().filename().string().rfind("host-", 0) == 0)
            kept.push_back(entry.path());
    std::sort(kept.begin(), kept.end()); // the names sort by time
    for (size_t i = 0; i + 10 < kept.size(); ++i)
        fs::remove(kept[i], ec);
}

// Started from the home screen the app has no console, and the host's log
// went nowhere: it is kept in Library/Caches/host.log (the run before in
// host-prev.log, and in Documents/Logs) and still reaches a console when one
// is attached. Once a process, a game module's copy included
// (RECOMP_HOST_LOG_TEE marks it).
void tee_log_to_file() {
    if (getenv("RECOMP_HOST_LOG_TEE"))
        return;
    setenv("RECOMP_HOST_LOG_TEE", "1", 1);
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (!dirs.count)
        return;
    const std::string dir = [dirs[0] fileSystemRepresentation];
    const std::string path = dir + "/host.log";
    std::error_code ec;
    fs::rename(path, dir + "/host-prev.log", ec);
    archive_previous_log(dir + "/host-prev.log");
    FILE *file = fopen(path.c_str(), "w");
    int fds[2];
    if (!file || pipe(fds) != 0) {
        if (file)
            fclose(file);
        return;
    }
    const int console = dup(STDERR_FILENO);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    // The fault handler waits on this pipe before the process ends, or the
    // report it writes dies in it unread (os_posix.cpp fault_drain_log).
    setenv("RECOMP_HOST_LOG_PIPE_FD", std::to_string(fds[0]).c_str(), 1);
    std::thread([fd = fds[0], file, console]() {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) {
            fwrite(buf, 1, (size_t)n, file);
            fflush(file);
            if (console >= 0)
                (void)write(console, buf, (size_t)n);
        }
    }).detach();
}

// The window's root view, for the reserved regions and safe area UIKit gives
// it. Main thread.
UIView *game_root_view() {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        for (UIWindow *w in ((UIWindowScene *)scene).windows)
            if (w.isKeyWindow && w.rootViewController.view)
                return w.rootViewController.view;
    }
    return nil;
}

} // namespace

void platform_ui_init_hints() {
    tee_log_to_file();
    // iPhones rotate freely to portrait, unless the game plays only in
    // landscape (game.toml [ios] portrait); iPads stay landscape (the
    // Info.plist keys match, so the OS never offers portrait to an iPad).
    SDL_SetHint(SDL_HINT_ORIENTATIONS,
                UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad || !RECOMP_IOS_PORTRAIT
                    ? "LandscapeLeft LandscapeRight"
                    : "LandscapeLeft LandscapeRight Portrait");
    // The mapper turns fingers into mouse and key events itself.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    // RECOMP_* switches for a device with no shell: Documents/switches.txt,
    // put there with devicectl (see tools/ios_logs.py for the container).
    recomp_env_apply_file((documents_dir() + "/switches.txt").c_str());
}

// Documents/game when it is ready. Anything else - no game yet, a bundled copy
// still to import, a wrong or partial import - is the launcher's to show.
GamePath platform_ui_resolve_game(const char *flag, std::string *error) {
    GamePath g;
    if (error)
        error->clear();
    // The app that loaded this game's module (host/sdl/main.cpp) names the
    // executable its launcher picked.
    if (flag && *flag) {
        g.exe = flag;
        g.source = GamePathSource::Flag;
        return g;
    }
    const std::string docs = documents_dir() + "/game";
    const launcher::Status st = launcher::check(launcher::spec_from_config(), docs);
    if (st.state == launcher::State::Ready) {
        g.exe = st.exe;
        g.source = GamePathSource::Saved;
    }
    return g;
}

// ---------------------------------------------------------------------------
// The launcher's iPadOS platform: the document picker for a folder or a ZIP,
// imports into Documents/game (kept out of iCloud backups), the Files app and
// Finder as another way in, and a share sheet for exported saves.
// ---------------------------------------------------------------------------
@interface RecompPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) launcher::PickDone done;
@property(nonatomic) bool scoped;
@end

namespace {
std::vector<NSURL *> g_scoped; // security-scoped URLs held for an import
NSMutableArray *g_delegates;   // picker delegates alive until they answer
UIBackgroundTaskIdentifier g_background = UIBackgroundTaskInvalid;
} // namespace

@implementation RecompPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    std::vector<launcher::Picked> picked;
    for (NSURL *url in urls) {
        if (self.scoped && [url startAccessingSecurityScopedResource])
            g_scoped.push_back(url);
        launcher::Picked p;
        p.path = url.fileSystemRepresentation;
        p.name = url.lastPathComponent.UTF8String;
        picked.push_back(p);
    }
    if (self.done)
        self.done(picked, "");
    [g_delegates removeObject:self];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (self.done)
        self.done({}, "");
    [g_delegates removeObject:self];
}
@end

namespace {

UIViewController *top_controller(SDL_Window *window) {
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *vc = uiwindow.rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;
    return vc;
}

class IosPlatform final : public launcher::Platform {
  public:
    explicit IosPlatform(SDL_Window *window) : window_(window) {}

    launcher::PlatformInfo info() override {
        launcher::PlatformInfo i;
        i.plays_in_place = false;
        i.can_pick_folder = true;
        i.can_pick_zip = true;
        i.can_open_folder = false;
        i.touch = true;
        i.import_root = documents_dir() + "/game";
        i.profile_dir = host_layout().profile_dir;
        i.drop_hint =
            "Or copy the game folder into this app's files with Finder (iPad connected to a Mac) "
            "or the Files app, then open the app again.";
        const fs::path bundled = fs::path(bundle_dir()) / "game";
        std::error_code ec;
        if (fs::is_regular_file(bundled / RECOMP_EXECUTABLE, ec))
            i.auto_import = bundled.string();
        return i;
    }
    void present(UIDocumentPickerViewController *picker, launcher::PickDone done, bool scoped) {
        RecompPickerDelegate *delegate = [RecompPickerDelegate new];
        delegate.done = std::move(done);
        delegate.scoped = scoped;
        if (!g_delegates)
            g_delegates = [NSMutableArray new];
        [g_delegates addObject:delegate];
        picker.delegate = delegate;
        picker.modalPresentationStyle = UIModalPresentationFormSheet;
        [top_controller(window_) presentViewController:picker animated:YES completion:nil];
    }
    void pick_folder(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
        present(picker, std::move(done), true);
    }
    void pick_zip(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP ]];
        present(picker, std::move(done), true);
    }
    void pick_saves(launcher::PickDone done) override {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP ]
                                                                        asCopy:YES];
        present(picker, std::move(done), false);
    }
    // Written to a temporary file first; export_ready offers it to the player.
    void pick_export(const std::string &suggested, launcher::PickDone done) override {
        launcher::Picked p;
        p.path = std::string(NSTemporaryDirectory().fileSystemRepresentation) + "/" + suggested;
        p.name = suggested;
        done({p}, "");
    }
    void export_ready(const launcher::Picked &p) override {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.path.c_str()]];
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForExportingURLs:@[ url ] asCopy:YES];
        present(picker, nullptr, false);
    }
    std::vector<std::string> candidates(const launcher::Spec &spec) override {
        // A game folder the player copied into Documents under any name.
        std::vector<std::string> out;
        const std::string docs = documents_dir();
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(docs, ec)) {
            const std::string name = entry.path().filename().string();
            if (!entry.is_directory(ec) || name == "game" || name[0] == '.')
                continue;
            // The folder itself, so a move can remove all of it afterwards.
            if (!launcher::find_root(spec, entry.path().string()).empty())
                out.push_back(entry.path().string());
        }
        return out;
    }
    // A folder the player copied into Documents is moved into Documents/game.
    bool movable(const launcher::Picked &p) override {
        const std::string docs = documents_dir() + "/";
        return !p.path.empty() && p.path.compare(0, docs.size(), docs) == 0 &&
               p.path.find('/', docs.size()) == std::string::npos && p.path != docs + "game";
    }
    void open_url(const std::string &url) override {
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (u)
            [[UIApplication sharedApplication] openURL:u options:@{} completionHandler:nil];
    }
    void import_activity(bool active, const launcher::Progress *progress) override {
        if (progress)
            return;
        dispatch_block_t update = ^{
          [UIApplication sharedApplication].idleTimerDisabled = active;
          if (active && g_background == UIBackgroundTaskInvalid)
              g_background = [[UIApplication sharedApplication]
                  beginBackgroundTaskWithName:@"Import game"
                            expirationHandler:^{
                              [[UIApplication sharedApplication] endBackgroundTask:g_background];
                              g_background = UIBackgroundTaskInvalid;
                            }];
          else if (!active && g_background != UIBackgroundTaskInvalid) {
              [[UIApplication sharedApplication] endBackgroundTask:g_background];
              g_background = UIBackgroundTaskInvalid;
          }
        };
        if ([NSThread isMainThread])
            update();
        else
            dispatch_async(dispatch_get_main_queue(), update);
    }
    void protect_import(const std::string &root) override {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:root.c_str()]
                                isDirectory:YES];
        NSError *err = nil;
        if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&err])
            fprintf(stderr, "[ios] could not exclude %s from backup: %s\n", root.c_str(),
                    err.localizedDescription.UTF8String);
    }
    void release(const launcher::Picked &p) override {
        for (auto it = g_scoped.begin(); it != g_scoped.end(); ++it)
            if (p.path == (*it).fileSystemRepresentation) {
                [*it stopAccessingSecurityScopedResource];
                g_scoped.erase(it);
                return;
            }
    }

  private:
    SDL_Window *window_;
};

} // namespace

namespace launcher {
std::unique_ptr<Platform> make_platform(SDL_Window *window) {
    return std::make_unique<IosPlatform>(window);
}
} // namespace launcher

namespace {
// SDL3 does not queue the app lifecycle events; it hands them to event
// watchers on the UIKit callstack that raised them (SDL_SendAppEvent). The
// host's event loop therefore never sees them, and this watcher is the only
// place the background transition can be acted on before iOS freezes the
// process.
bool lifecycle_watch(void *, SDL_Event *e) {
    platform_ui_handle_lifecycle(*e);
    return true;
}
} // namespace

// A test of the lifecycle with nobody at the device (RECOMP_IOS_LIFECYCLE_STRESS
// = a period in ms, e.g. in Documents/switches.txt): UIKit's own notifications
// are posted as a pulled-down Control Center would raise them - inactive for
// half the period, then active again - and every fourth time the app goes to
// the background and back as well. SDL's observers turn them into the events
// lifecycle_watch takes, so the whole path runs.
static void start_lifecycle_stress() {
    const char *v = getenv("RECOMP_IOS_LIFECYCLE_STRESS");
    const int period = v ? atoi(v) : 0;
    if (period <= 0)
        return;
    fprintf(stderr, "[ios] lifecycle stress: a Control Center pull every %d ms\n", period);
    static int round = 0;
    auto post = [](NSNotificationName name) {
        [NSNotificationCenter.defaultCenter postNotificationName:name
                                                          object:UIApplication.sharedApplication];
    };
    [NSTimer
        scheduledTimerWithTimeInterval:period / 1000.0
                               repeats:YES
                                 block:^(NSTimer *) {
                                   const bool away = ++round % 4 == 0;
                                   post(UIApplicationWillResignActiveNotification);
                                   if (away)
                                       post(UIApplicationDidEnterBackgroundNotification);
                                   const int64_t half = (int64_t)period * 500000ll;
                                   dispatch_after(
                                       dispatch_time(DISPATCH_TIME_NOW, half),
                                       dispatch_get_main_queue(), ^{
                                         if (away)
                                             post(UIApplicationWillEnterForegroundNotification);
                                         post(UIApplicationDidBecomeActiveNotification);
                                         if (round % 20 == 0)
                                             fprintf(stderr, "[ios] lifecycle stress: %d pulls\n",
                                                     round);
                                       });
                                 }];
}

SDL_Window *platform_ui_create_window(const char *title, int, int, int, int,
                                      SDL_WindowFlags surface_flag, int *window_mode) {
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(lifecycle_watch, nullptr);
        watching = true;
        start_lifecycle_stress();
    }
    SDL_Window *w = SDL_CreateWindow(
        title, 0, 0, surface_flag | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_mode)
        *window_mode = 2;
    // No vertical bar (iOS 27.1: a foldable iPhone puts the status bar, hidden
    // or not, in a column along one side, and keeps that side's safe area for
    // it): the game is a full-screen picture, as Apple's guidance for a
    // non-scrolling layout says. Added to the window's own view controller
    // class (SDL's, a game module's copy of it), which does not define it.
    if (@available(iOS 27.1, *)) {
        UIWindow *uiwindow = w ? (__bridge UIWindow *)SDL_GetPointerProperty(
                                     SDL_GetWindowProperties(w), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr)
                               : nil;
        UIViewController *vc = uiwindow.rootViewController;
        if (vc) {
            IMP disabled = imp_implementationWithBlock(^NSInteger(id) {
              return UIVerticalBarBehaviorDisabled;
            });
            const bool added = class_addMethod(vc.class, @selector(preferredVerticalBarBehavior), disabled, "q@:");
            [vc setNeedsUpdateOfVerticalBarConfiguration];
            fprintf(stderr, "[ios] vertical bar %s\n", added ? "disabled" : "left as the view controller has it");
        }
    }
    return w;
}

// The game may be closed from the app switcher without reaching its own exit,
// so it saves before the app goes away (runtime/native_seam.h
// recomp_app_background). Its own thread does that in its next frame, which
// may need this (main) thread, so the wait runs on a background queue under a
// background task that keeps the process alive until it is over. One save at
// a time: another request while one is under way is the same save.
static std::atomic<bool> g_saving{false};
static void save_before_background() {
    if (g_saving.exchange(true))
        return;
    UIApplication *app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    task = [app beginBackgroundTaskWithName:@"save settings"
                          expirationHandler:^{
                            [app endBackgroundTask:task];
                            task = UIBackgroundTaskInvalid;
                          }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      recomp_app_background();
      g_saving.store(false);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (task != UIBackgroundTaskInvalid) {
            [app endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        }
      });
    });
}

// Where the app stands, by UIKit's three states. SDL3 hands both "will resign
// active" (the scene's and the application's, so often twice) to
// SDL_EVENT_WILL_ENTER_BACKGROUND, though a pulled-down Control Center or
// Notification Center, a call banner or the app switcher only make the app
// inactive: it stays on screen and may draw. Only DID_ENTER_BACKGROUND means
// it is away. So inactive pauses the sound and nothing else; the save waits a
// little (a glance at Control Center does not write the profile mid-frame) and
// happens at once when the app really goes to the background, where the
// presentation stops too (no GPU work in the background).
enum class AppState { Active, Inactive, Background };
static AppState g_app_state = AppState::Active; // main thread
static uint64_t g_away_generation = 0;          // main thread: one per time away
static bool g_saved_this_time = false;          // main thread
// A game module run from this app (the multi-game app's launcher hands over
// to libgame.<id>.dylib, main.cpp run_game_module) has its own copy of this
// file and its own watcher: the launcher's copy then leaves the events to it.
static bool g_lifecycle_handed_over = false;
static constexpr int64_t kInactiveSaveDelayNs = 2000ll * 1000000ll;

static void save_once() {
    if (g_saved_this_time)
        return;
    g_saved_this_time = true;
    save_before_background(); // the game may never see its own exit
}

void platform_ui_hand_over_lifecycle() {
    g_lifecycle_handed_over = true;
}

bool platform_ui_handle_lifecycle(const SDL_Event &e) {
    if (g_lifecycle_handed_over)
        return false;
    switch (e.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        if (g_app_state != AppState::Active)
            return true; // the second "will resign active" of the pair
        g_app_state = AppState::Inactive;
        g_saved_this_time = false;
        host_note_app_active(0);
        {
            const uint64_t away = ++g_away_generation;
            fprintf(stderr, "[ios] inactive: pausing audio\n");
            host_audio_pause(true);
            // Still away after a while: the player may be leaving for good
            // (the app switcher), so the settings are written now.
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kInactiveSaveDelayNs),
                           dispatch_get_main_queue(), ^{
                             if (away == g_away_generation && g_app_state == AppState::Inactive) {
                                 fprintf(stderr, "[ios] inactive for a while: saving settings\n");
                                 save_once();
                             }
                           });
        }
        return true;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        if (g_app_state == AppState::Background)
            return true;
        if (g_app_state == AppState::Active) {
            ++g_away_generation;
            g_saved_this_time = false;
        }
        g_app_state = AppState::Background;
        host_note_app_active(0);
        fprintf(stderr, "[ios] did enter background: saving, suspending presentation and audio\n");
        save_once();
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        fprintf(stderr, "[ios] will enter foreground\n");
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (g_app_state == AppState::Active)
            return true; // the second "did become active" of the pair
        fprintf(stderr, "[ios] active again (%s): resuming audio%s\n",
                g_app_state == AppState::Background ? "from the background" : "was inactive",
                g_app_state == AppState::Background ? " and presentation" : "");
        if (g_app_state == AppState::Background)
            host_present_suspend(false);
        host_audio_pause(false);
        g_app_state = AppState::Active;
        host_note_app_active(1);
        ++g_away_generation; // a pending inactive save is off
        return true;
    case SDL_EVENT_TERMINATING:
        fprintf(stderr, "[ios] terminating\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    default:
        return false;
    }
}

bool platform_ui_keypad_wanted() {
    return !SDL_HasKeyboard();
}

bool platform_ui_touch_device() {
    return true;
}

bool platform_ui_phone() {
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
}

int platform_ui_cutout_side() {
    if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone)
        return 0;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        switch (((UIWindowScene *)scene).effectiveGeometry.interfaceOrientation) {
        case UIInterfaceOrientationLandscapeRight: // the home side on the right:
            return -1;                             // the top, with the housing, on the left
        case UIInterfaceOrientationLandscapeLeft:
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

static bool cutout_in_corner_now(int w_pt, int h_pt);

// Asked every turn: the answer is kept for the window's size, and asked again
// now and then (UIKit's regions may come after the first layout).
bool platform_ui_cutout_in_corner(int w_pt, int h_pt) {
    static int seen_w = -1, seen_h = -1, turns = 0;
    static bool answer = false;
    if (w_pt != seen_w || h_pt != seen_h || ++turns >= 120) {
        seen_w = w_pt;
        seen_h = h_pt;
        turns = 0;
        answer = cutout_in_corner_now(w_pt, h_pt);
    }
    return answer;
}

static bool cutout_in_corner_now(int w_pt, int h_pt) {
    if (const char *v = getenv("RECOMP_CUTOUT_IN_CORNER"); v && *v)
        return v[0] != '0';
    // What UIKit says is occluded (iOS 27.1 reserved regions): a camera in the
    // top or bottom part of a side is a corner's.
    if (@available(iOS 27.1, *)) {
        if (UIView *v = game_root_view()) {
            NSArray<UIViewReservedRegion *> *regions =
                [v reservedRegionsOfKind:UIViewReservedRegionKind.occlusionRegionKind
                                 options:UIViewReservedRegionQueryOptionsIncludeInactive];
            static NSUInteger told = NSUIntegerMax;
            if (regions.count != told) {
                told = regions.count;
                for (UIViewReservedRegion *r in regions)
                    fprintf(stderr, "[ios] occluded region %.0f,%.0f %.0fx%.0f in %.0fx%.0f%s\n", r.frame.origin.x,
                            r.frame.origin.y, r.frame.size.width, r.frame.size.height, v.bounds.size.width,
                            v.bounds.size.height, r.isActive ? "" : " (inactive)");
                if (!regions.count)
                    fprintf(stderr, "[ios] no occluded regions reported\n");
            }
            const CGSize b = v.bounds.size;
            bool corner = false, middle = false;
            for (UIViewReservedRegion *r in regions) {
                const CGRect f = r.frame;
                const bool side = f.origin.x <= 1 || CGRectGetMaxX(f) >= b.width - 1;
                if (!side || b.height <= 0)
                    continue;
                const double cy = CGRectGetMidY(f) / b.height;
                (cy < 0.3 || cy > 0.7 ? corner : middle) = true;
            }
            if (corner || middle)
                return corner && !middle;
        }
    }
    // Otherwise by the screen's shape: a Dynamic Island in the middle of a
    // side is a long phone's (19.5:9); a shorter screen's camera (a foldable's
    // closed one, nearly 3:2) is a hole in a corner.
    const int lo = std::min(w_pt, h_pt), hi = std::max(w_pt, h_pt);
    return lo > 0 && hi < 1.8 * lo;
}

bool platform_ui_pointer_capture_supported() {
    return false;
}

int platform_ui_default_overlay() {
    return 0;
}

void platform_ui_process_exit(int code) {
    // UIApplicationMain never returns, so the game's own Exit would leave its
    // last frame on the screen; the game asked to end, and this ends it.
    fprintf(stderr, "[ios] the game exited (%d); ending the app\n", code);
    fflush(stderr);
    exit(code);
}

// ---------------------------------------------------------------------------
// Haptics: a light impact tick for on-screen control presses, and a Core
// Haptics continuous player standing in for game rumble on a device with no
// controller. Both must run on the main thread.
// ---------------------------------------------------------------------------
namespace {
UIImpactFeedbackGenerator *g_tap_generator;        // prepared once, reused for every tap
CHHapticEngine *g_haptic_engine;                   // created once, started lazily
id<CHHapticAdvancedPatternPlayer> g_rumble_player; // the current 30 s rumble, or nil

bool device_supports_haptics() {
    static const bool supported = CHHapticEngine.capabilitiesForHardware.supportsHaptics;
    return supported;
}

// The engine, created on first use. A stop or reset (background, audio
// session interruption) drops the player, since it dies with the engine.
CHHapticEngine *haptic_engine() {
    if (!g_haptic_engine) {
        NSError *error = nil;
        g_haptic_engine = [[CHHapticEngine alloc] initAndReturnError:&error];
        if (error) {
            fprintf(stderr, "[ios] CHHapticEngine init failed: %s\n",
                    error.localizedDescription.UTF8String);
            return nil;
        }
        // Core Haptics calls both handlers on its own background queue; every
        // other read/write of g_rumble_player runs on the main queue (the
        // functions below), so hop there before touching it.
        g_haptic_engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
          dispatch_async(dispatch_get_main_queue(), ^{
            g_rumble_player = nil;
          });
        };
        g_haptic_engine.resetHandler = ^{
          dispatch_async(dispatch_get_main_queue(), ^{
            g_rumble_player = nil;
          });
        };
    }
    return g_haptic_engine;
}
} // namespace

void platform_ui_haptic_tap() {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_tap_generator) {
          g_tap_generator =
              [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
          [g_tap_generator prepare];
      }
      [g_tap_generator impactOccurred];
    });
}

void platform_ui_device_rumble(uint16_t low, uint16_t high) {
    const float intensity = std::max(low, high) / 65535.0f;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!device_supports_haptics())
          return;
      if (intensity <= 0.0f) {
          [g_rumble_player cancelAndReturnError:nil];
          g_rumble_player = nil;
          return;
      }
      NSError *error = nil;
      if (g_rumble_player) {
          // Already rumbling: retune it in place rather than restart it.
          CHHapticDynamicParameter *param = [[CHHapticDynamicParameter alloc]
              initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                            value:intensity
                     relativeTime:0];
          [g_rumble_player sendParameters:@[ param ] atTime:CHHapticTimeImmediate error:&error];
          if (!error)
              return;
          g_rumble_player = nil; // the player died under us; fall through and rebuild it
      }
      CHHapticEngine *engine = haptic_engine();
      if (!engine)
          return;
      [engine startAndReturnError:&error];
      if (error)
          return;
      CHHapticEventParameter *intensity_param = [[CHHapticEventParameter alloc]
          initWithParameterID:CHHapticEventParameterIDHapticIntensity
                        value:intensity];
      CHHapticEventParameter *sharpness_param = [[CHHapticEventParameter alloc]
          initWithParameterID:CHHapticEventParameterIDHapticSharpness
                        value:0.3f];
      CHHapticEvent *event =
          [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                        parameters:@[ intensity_param, sharpness_param ]
                                      relativeTime:0
                                          duration:30.0];
      CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ]
                                                              parameters:@[]
                                                                   error:&error];
      if (error)
          return;
      id<CHHapticAdvancedPatternPlayer> player = [engine createAdvancedPlayerWithPattern:pattern
                                                                                   error:&error];
      if (error || !player)
          return;
      [player startAtTime:CHHapticTimeImmediate error:&error];
      if (!error)
          g_rumble_player = player;
    });
}

// ---------------------------------------------------------- camera buttons --
// Round, dark buttons with a gold rim, as the game's own map buttons, in a
// column at the top right. UIKit takes their touches, so none reaches the map.
@interface RecompCameraBar : UIStackView
@property(nonatomic) void (*press)(int);
@end

@implementation RecompCameraBar
- (instancetype)init {
    self = [super initWithFrame:CGRectZero];
    self.axis = UILayoutConstraintAxisVertical;
    self.spacing = 14;
    self.translatesAutoresizingMaskIntoConstraints = NO;
    // In RECOMP_CAMERA_* order (runtime/native_seam.h).
    NSArray<NSString *> *symbols =
        @[ @"plus", @"minus", @"arrow.counterclockwise", @"arrow.clockwise" ];
    NSArray<NSString *> *labels = @[ @"Zoom in", @"Zoom out", @"Turn left", @"Turn right" ];
    UIImageSymbolConfiguration *weight =
        [UIImageSymbolConfiguration configurationWithPointSize:24 weight:UIImageSymbolWeightBold];
    for (NSUInteger i = 0; i < symbols.count; ++i) {
        UIButton *b = [UIButton buttonWithType:UIButtonTypeCustom];
        [b setImage:[UIImage systemImageNamed:symbols[i] withConfiguration:weight]
           forState:UIControlStateNormal];
        b.tintColor = [UIColor colorWithRed:0.98 green:0.90 blue:0.66 alpha:1];
        b.backgroundColor = [UIColor colorWithRed:0.08 green:0.06 blue:0.04 alpha:0.62];
        b.layer.cornerRadius = 30;
        b.layer.borderWidth = 2.5;
        b.layer.borderColor = [UIColor colorWithRed:0.80 green:0.64 blue:0.30 alpha:0.95].CGColor;
        b.accessibilityLabel = labels[i];
        b.tag = (NSInteger)i + 1;
        [b.widthAnchor constraintEqualToConstant:60].active = YES;
        [b.heightAnchor constraintEqualToConstant:60].active = YES;
        [b addTarget:self action:@selector(down:) forControlEvents:UIControlEventTouchDown];
        [b addTarget:self
                      action:@selector(up:)
            forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                             UIControlEventTouchCancel];
        [b addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
        [self addArrangedSubview:b];
    }
    return self;
}
- (void)down:(UIButton *)b {
    b.transform = CGAffineTransformMakeScale(0.9, 0.9);
    b.alpha = 0.8;
}
- (void)up:(UIButton *)b {
    b.transform = CGAffineTransformIdentity;
    b.alpha = 1;
}
- (void)tapped:(UIButton *)b {
    platform_ui_haptic_tap();
    if (self.press)
        self.press((int)b.tag);
}
@end

// The game's own art for them, when it ships it (touch-ui/camera-panel/ in the
// bundle, game.toml's repository folder): a framed column of round buttons and
// the blue corner arrow that folds the panel away, as the minimap's does.
// layout.json places each sprite on the panel's canvas, in source pixels:
// {"canvas": [w, h], "sprites": {name: {x, y, w, h, center: [x, y], radius}}}.
@interface RecompRoundButton : UIButton
@property(nonatomic) CGPoint hitCenter; // in the button's own points
@property(nonatomic) CGFloat hitRadius;
@end

@implementation RecompRoundButton
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent *)event {
    const CGFloat dx = p.x - self.hitCenter.x, dy = p.y - self.hitCenter.y;
    return dx * dx + dy * dy <= self.hitRadius * self.hitRadius;
}
@end

// One command of the game's document (runtime/native_seam.h
// recomp_hotkeys_state), as the panel shows it.
@interface RecompPanelCommand : NSObject
@property(nonatomic, copy) NSString *ident, *label, *brief, *keys, *kind, *symbol;
@property(nonatomic) int quick;
@end

@implementation RecompPanelCommand
@end

// The art of the panel (touch-ui/camera-panel/ in the bundle): the frame, the
// camera's buttons, the corner arrow, a command's button (btn-<id>) or the
// blank one a system symbol is drawn on, and "more".
// layout.json, in source pixels: "canvas" [w, h]; "sprites" places the frame,
// the camera's buttons and the arrow on the canvas {x, y, w, h, center, radius};
// "caps" [top, bottom] the frame's rows kept when it stretches, "caps_x" [left,
// right] its columns kept when it widens; "buttons" the
// command buttons' {w, h, center, radius}, which the panel stacks.
@interface RecompPanelArt : NSObject
@property(nonatomic, copy) NSString *folder;
@property(nonatomic, strong) NSDictionary *sprites, *buttons;
@property(nonatomic) CGSize canvas;
@property(nonatomic) CGFloat capTop, capBottom, capLeft, capRight;
+ (instancetype)artInFolder:(NSString *)folder;
- (UIImage *)image:(NSString *)name state:(NSString *)state;
@end

@implementation RecompPanelArt
+ (instancetype)artInFolder:(NSString *)folder {
    NSData *json =
        [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"layout.json"]];
    NSDictionary *layout =
        json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
    NSDictionary *sprites = layout[@"sprites"];
    NSArray *canvas = layout[@"canvas"];
    if (![sprites isKindOfClass:NSDictionary.class] || canvas.count != 2 || !sprites[@"frame"] ||
        !sprites[@"btn-collapse"] || !sprites[@"btn-zoom-in"])
        return nil;
    RecompPanelArt *art = [[RecompPanelArt alloc] init];
    art.folder = folder;
    art.sprites = sprites;
    art.buttons = [layout[@"buttons"] isKindOfClass:NSDictionary.class] ? layout[@"buttons"] : @{};
    art.canvas = CGSizeMake([canvas[0] doubleValue], [canvas[1] doubleValue]);
    NSArray *caps = layout[@"caps"];
    art.capTop = caps.count == 2 ? [caps[0] doubleValue] : 0;
    art.capBottom = caps.count == 2 ? [caps[1] doubleValue] : 0;
    NSArray *caps_x = layout[@"caps_x"];
    art.capLeft = caps_x.count == 2 ? [caps_x[0] doubleValue] : 0;
    art.capRight = caps_x.count == 2 ? [caps_x[1] doubleValue] : 0;
    return art;
}
- (UIImage *)image:(NSString *)name state:(NSString *)state {
    NSString *file = state ? [NSString stringWithFormat:@"%@-%@.png", name, state]
                           : [name stringByAppendingString:@".png"];
    return [UIImage imageWithContentsOfFile:[self.folder stringByAppendingPathComponent:file]];
}
@end

// A command's button face: its own art, or the blank button with the
// command's system symbol in gold.
static UIImage *command_face(RecompPanelArt *art, RecompPanelCommand *c, NSString *state,
                             NSString **name_out) {
    NSString *name = [@"btn-" stringByAppendingString:c.ident];
    UIImage *img = art.buttons[c.ident] ? [art image:name state:state] : nil;
    if (img) {
        *name_out = c.ident;
        return img;
    }
    *name_out = @"blank";
    UIImage *blank = [art image:@"btn-blank" state:state];
    NSDictionary *b = art.buttons[@"blank"];
    if (!blank || !b)
        return nil;
    UIImage *symbol = [UIImage systemImageNamed:c.symbol.length ? c.symbol : @"questionmark"];
    if (!symbol)
        return blank;
    NSArray *center = b[@"center"];
    const CGFloat cx = [center[0] doubleValue], cy = [center[1] doubleValue];
    const CGFloat side = [b[@"radius"] doubleValue] * 0.9;
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = 1;
    return [[[UIGraphicsImageRenderer alloc] initWithSize:blank.size format:format]
        imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
          [blank drawAtPoint:CGPointZero];
          UIImage *gold = [symbol imageWithTintColor:[UIColor colorWithRed:0.86
                                                                     green:0.74
                                                                      blue:0.48
                                                                     alpha:1]
                                       renderingMode:UIImageRenderingModeAlwaysOriginal];
          const CGSize s = gold.size;
          const CGFloat k = side / MAX(s.width, s.height);
          [gold drawInRect:CGRectMake(cx - s.width * k / 2, cy - s.height * k / 2, s.width * k,
                                      s.height * k)];
        }];
}

// A command's name and keys over the panel for a moment (a long press).
static void show_command_tip(UIView *anchor, RecompPanelCommand *c) {
    UIView *root = anchor.window.rootViewController.view;
    if (!root)
        return;
    UILabel *tip = [[UILabel alloc] init];
    tip.text = c.keys.length ? [NSString stringWithFormat:@"%@ · %@", c.label, c.keys] : c.label;
    tip.font = [UIFont boldSystemFontOfSize:14];
    tip.textColor = [UIColor colorWithRed:0.98 green:0.90 blue:0.66 alpha:1];
    tip.backgroundColor = [UIColor colorWithRed:0.08 green:0.06 blue:0.04 alpha:0.9];
    tip.textAlignment = NSTextAlignmentCenter;
    tip.layer.cornerRadius = 8;
    tip.layer.masksToBounds = YES;
    [tip sizeToFit];
    CGRect r = CGRectInset(tip.frame, -10, -6);
    const CGRect a = [anchor convertRect:anchor.bounds toView:root];
    const BOOL right = CGRectGetMidX(a) < root.bounds.size.width / 2;
    r.origin.x = right ? CGRectGetMaxX(a) + 8 : CGRectGetMinX(a) - 8 - r.size.width;
    r.origin.y = CGRectGetMidY(a) - r.size.height / 2;
    tip.frame = r;
    tip.userInteractionEnabled = NO;
    [root addSubview:tip];
    [UIView animateWithDuration:0.25
        delay:1.6
        options:0
        animations:^{
          tip.alpha = 0;
        }
        completion:^(BOOL) {
          [tip removeFromSuperview];
        }];
}

// The control panel: the game's frame around a grid of round buttons, each
// with its name under it - the camera's four over a 3D map, then every
// command the screen offers, in the player's order. The blue corner arrow
// folds it all away, as the minimap's does. A command held shows its full
// name and keys, and dragged it moves to another command's place; the order is
// kept per screen. Built anew whenever its cells, size or place change.
@interface RecompControlPanel : UIView
@property(nonatomic) CGSize size;     // in points
@property(nonatomic) CGPoint arrowAt; // the arrow's centre in the panel's points
@property(nonatomic) void (*camera)(int);
@property(nonatomic, copy) void (^pressed)(RecompPanelCommand *command);
@property(nonatomic, copy) void (^moved)(NSString *ident, NSString *before);
- (instancetype)initWithArt:(RecompPanelArt *)art
                      phone:(BOOL)phone
                     camera:(BOOL)camera
                   commands:(NSArray<RecompPanelCommand *> *)commands
                       cols:(NSInteger)cols;
- (void)setActive:(NSSet<NSString *> *)active;
+ (CGSize)sizeForArt:(RecompPanelArt *)art
               phone:(BOOL)phone
                cols:(NSInteger)cols
                rows:(NSInteger)rows;
@end

@implementation RecompControlPanel {
    NSMutableArray<UIView *> *folding_; // the frame and the cells
    NSMutableDictionary<NSString *, RecompRoundButton *> *commandButtons_;
    NSMutableArray<RecompPanelCommand *> *commands_; // in the panel's order
    NSMutableArray<UIView *> *cells_;                // each command's cell
    RecompRoundButton *arrow_;
    CGPoint fold_at_; // the arrow's centre: the panel folds into it
    CGPoint drag_from_, drag_grab_;
    CGFloat d_; // a button's diameter, in points
    BOOL collapsed_;
}
// Diameter of a button on screen: about the size of the game's own round map
// buttons on a 13-inch iPad; on a phone-sized window Apple's 44 pt minimum.
static CGFloat camera_button_pt(BOOL phone) {
    return phone ? 44 : 64;
}
static NSString *const kCollapsedKey = @"RecompCameraPanelCollapsed";
static NSArray<NSString *> *const kCameraNames =
    @[ @"btn-zoom-in", @"btn-zoom-out", @"btn-rotate-left", @"btn-rotate-right" ];

// A cell: the button and two short lines of its name under it, in points.
static CGSize cell_pt(BOOL phone) {
    const CGFloat d = camera_button_pt(phone);
    return CGSizeMake(d + (phone ? 16 : 24), d + 3 + (phone ? 24 : 30));
}
static CGFloat label_font_pt(BOOL phone) {
    return phone ? 9.5 : 12;
}

// The frame's margins around its grid, in the art's pixels: where its first
// camera button stands from its edges (the bottom one leaves the corner
// arrow room), and the arrow's centre from the frame's bottom-left corner.
static UIEdgeInsets frame_margins_px(RecompPanelArt *art) {
    NSDictionary *f = art.sprites[@"frame"], *b = art.sprites[@"btn-rotate-left"];
    NSArray *c = b[@"center"];
    const CGFloat r = [b[@"radius"] doubleValue];
    const CGFloat left = [c[0] doubleValue] - r - [f[@"x"] doubleValue];
    const CGFloat top = [c[1] doubleValue] - r - [f[@"y"] doubleValue];
    return UIEdgeInsetsMake(top, left, top + 40, left);
}

+ (CGFloat)scaleForArt:(RecompPanelArt *)art phone:(BOOL)phone {
    return camera_button_pt(phone) / (2 * [art.sprites[@"btn-zoom-in"][@"radius"] doubleValue]);
}

// The panel's size for a grid of cols x rows, the arrow standing out at the
// bottom-left corner included.
+ (CGSize)sizeForArt:(RecompPanelArt *)art
               phone:(BOOL)phone
                cols:(NSInteger)cols
                rows:(NSInteger)rows {
    const CGFloat s = [self scaleForArt:art phone:phone];
    const UIEdgeInsets m = frame_margins_px(art);
    const CGSize cell = cell_pt(phone);
    return CGSizeMake(cols * cell.width + (m.left + m.right) * s,
                      rows * cell.height + (m.top + m.bottom) * s);
}

- (instancetype)initWithArt:(RecompPanelArt *)art
                      phone:(BOOL)phone
                     camera:(BOOL)camera
                   commands:(NSArray<RecompPanelCommand *> *)commands
                       cols:(NSInteger)cols {
    UIImage *frame_image = [art image:@"frame" state:nil];
    if (!frame_image)
        return nil;
    const NSInteger count =
        (camera ? (NSInteger)kCameraNames.count : 0) + (NSInteger)commands.count;
    cols = MAX(1, MIN(cols, MAX(count, 1)));
    const NSInteger rows = MAX(1, (count + cols - 1) / cols);
    const CGFloat s = [RecompControlPanel scaleForArt:art phone:phone];
    const UIEdgeInsets m = frame_margins_px(art);
    const CGSize cell = cell_pt(phone), size = [RecompControlPanel sizeForArt:art
                                                                        phone:phone
                                                                         cols:cols
                                                                         rows:rows];
    self = [super initWithFrame:CGRectMake(0, 0, size.width, size.height)];
    _size = size;
    self.translatesAutoresizingMaskIntoConstraints = NO;
    folding_ = [NSMutableArray array];
    commandButtons_ = [NSMutableDictionary dictionary];
    commands_ = [NSMutableArray array];
    cells_ = [NSMutableArray array];
    // The frame, stretched at the art's own size so its ornamented corners
    // keep their proportions, then scaled down as a whole.
    const CGSize frame_px = CGSizeMake(size.width / s, size.height / s);
    UIImage *resizable =
        [frame_image resizableImageWithCapInsets:UIEdgeInsetsMake(art.capTop, art.capLeft,
                                                                  art.capBottom, art.capRight)
                                    resizingMode:UIImageResizingModeStretch];
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = 1;
    UIImage *stretched = [[[UIGraphicsImageRenderer alloc] initWithSize:frame_px format:format]
        imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
          [resizable drawInRect:CGRectMake(0, 0, frame_px.width, frame_px.height)];
        }];
    UIImageView *frame = [[UIImageView alloc] initWithImage:stretched];
    frame.frame = self.bounds;
    frame.userInteractionEnabled = YES; // a touch on the frame stops here, off the map
    [self addSubview:frame];
    [folding_ addObject:frame];
    const CGFloat d = camera_button_pt(phone);
    d_ = d;
    UIColor *gold = [UIColor colorWithRed:0.98 green:0.90 blue:0.66 alpha:1];
    // Cell k's box: row-major from the frame's top-left margin.
    auto cell_rect = ^CGRect(NSInteger k) {
      return CGRectMake(m.left * s + (k % cols) * cell.width, m.top * s + (k / cols) * cell.height,
                        cell.width, cell.height);
    };
    // A round button in a cell, its disc d across and its name under it.
    auto button = ^RecompRoundButton *(UIImage *normal, UIImage *pushed, CGPoint center_px,
                                       CGSize size_px, CGFloat radius_px, NSInteger k,
                                       NSString *name) {
      if (!normal)
          return (RecompRoundButton *)nil;
      const CGRect box = cell_rect(k);
      const CGFloat bs = d / (2 * radius_px); // this sprite's scale
      RecompRoundButton *b = [RecompRoundButton buttonWithType:UIButtonTypeCustom];
      [b setImage:normal forState:UIControlStateNormal];
      [b setImage:pushed ?: normal forState:UIControlStateHighlighted];
      [b setImage:pushed ?: normal forState:UIControlStateSelected];
      b.adjustsImageWhenHighlighted = NO;
      b.hitCenter = CGPointMake(center_px.x * bs, center_px.y * bs);
      b.hitRadius = MAX(d / 2, cell.width / 2);
      [b addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
      // In a holder the size of its cell, which a drag moves with its name.
      UIView *holder = [[UIView alloc] initWithFrame:box];
      b.frame = CGRectMake(box.size.width / 2 - center_px.x * bs, d / 2 - center_px.y * bs,
                           size_px.width * bs, size_px.height * bs);
      [holder addSubview:b];
      if (name.length) {
          UILabel *label =
              [[UILabel alloc] initWithFrame:CGRectMake(0, d + 2, cell.width, cell.height - d - 2)];
          label.text = name;
          label.font = [UIFont boldSystemFontOfSize:label_font_pt(phone)];
          label.textColor = gold;
          label.textAlignment = NSTextAlignmentCenter;
          label.numberOfLines = 2;
          label.adjustsFontSizeToFitWidth = YES;
          label.minimumScaleFactor = 0.75;
          label.lineBreakMode = NSLineBreakByTruncatingTail;
          label.shadowColor = [UIColor colorWithWhite:0 alpha:0.85];
          label.shadowOffset = CGSizeMake(0, 1);
          label.userInteractionEnabled = NO;
          [holder addSubview:label];
      }
      [self addSubview:holder];
      [folding_ addObject:holder];
      return b;
    };
    NSInteger k = 0;
    if (camera) {
        // In the art's order, top down; tags are RECOMP_CAMERA_* (runtime/native_seam.h).
        NSArray<NSString *> *labels = @[ @"Zoom in", @"Zoom out", @"Turn left", @"Turn right" ];
        NSDictionary *sprites = art.sprites;
        NSArray<NSString *> *top_down =
            [kCameraNames sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
              return [@([sprites[a][@"y"] doubleValue]) compare:@([sprites[b][@"y"] doubleValue])];
            }];
        for (NSString *name in top_down) {
            NSDictionary *sp = sprites[name];
            NSArray *c = sp[@"center"];
            RecompRoundButton *b =
                button([art image:name state:@"normal"], [art image:name state:@"pushed"],
                       CGPointMake([c[0] doubleValue] - [sp[@"x"] doubleValue],
                                   [c[1] doubleValue] - [sp[@"y"] doubleValue]),
                       CGSizeMake([sp[@"w"] doubleValue], [sp[@"h"] doubleValue]),
                       [sp[@"radius"] doubleValue], k++, nil);
            if (!b)
                return nil;
            const NSUInteger i = [kCameraNames indexOfObject:name];
            b.tag = (NSInteger)i + 1;
            b.accessibilityLabel = labels[i];
        }
    }
    for (RecompPanelCommand *c in commands) {
        NSString *art_name = nil;
        UIImage *normal = command_face(art, c, @"normal", &art_name);
        UIImage *pushed = command_face(art, c, @"pushed", &art_name);
        NSDictionary *bd = art.buttons[art_name];
        NSArray *bc = bd[@"center"];
        if (!bd || bc.count != 2)
            continue;
        RecompRoundButton *b =
            button(normal, pushed, CGPointMake([bc[0] doubleValue], [bc[1] doubleValue]),
                   CGSizeMake([bd[@"w"] doubleValue], [bd[@"h"] doubleValue]),
                   [bd[@"radius"] doubleValue], k++, c.brief.length ? c.brief : c.label);
        if (!b)
            continue;
        b.tag = 100 + (NSInteger)commands_.count;
        b.accessibilityLabel = c.label;
        b.accessibilityHint = c.keys;
        UILongPressGestureRecognizer *hold =
            [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(held:)];
        hold.minimumPressDuration = 0.35;
        [b addGestureRecognizer:hold];
        [commands_ addObject:c];
        [cells_ addObject:b.superview];
        commandButtons_[c.ident] = b;
    }
    // The arrow at the frame's bottom-left corner, as the camera panel's art
    // has it; small on the art, a finger gets 44 pt all the same.
    NSDictionary *a = art.sprites[@"btn-collapse"], *fr = art.sprites[@"frame"];
    NSArray *ac = a[@"center"];
    const CGPoint arrow_center = CGPointMake(
        ([ac[0] doubleValue] - [fr[@"x"] doubleValue]) * s,
        size.height - ([fr[@"y"] doubleValue] + [fr[@"h"] doubleValue] - [ac[1] doubleValue]) * s);
    UIImage *an = [art image:@"btn-collapse" state:@"normal"];
    const CGPoint in_sprite = CGPointMake([ac[0] doubleValue] - [a[@"x"] doubleValue],
                                          [ac[1] doubleValue] - [a[@"y"] doubleValue]);
    arrow_ = [RecompRoundButton buttonWithType:UIButtonTypeCustom];
    arrow_.frame = CGRectMake(arrow_center.x - in_sprite.x * s, arrow_center.y - in_sprite.y * s,
                              [a[@"w"] doubleValue] * s, [a[@"h"] doubleValue] * s);
    [arrow_ setImage:an forState:UIControlStateNormal];
    [arrow_ setImage:[art image:@"btn-collapse" state:@"pushed"] ?: an
            forState:UIControlStateHighlighted];
    arrow_.adjustsImageWhenHighlighted = NO;
    arrow_.hitCenter = CGPointMake(in_sprite.x * s, in_sprite.y * s);
    arrow_.hitRadius = MAX(22, [a[@"radius"] doubleValue] * s);
    [arrow_ addTarget:self action:@selector(tapped:) forControlEvents:UIControlEventTouchUpInside];
    arrow_.tag = 0;
    arrow_.accessibilityLabel = @"Hide the control panel";
    [self addSubview:arrow_];
    _arrowAt = arrow_center;
    fold_at_ = arrow_center;
    collapsed_ = [NSUserDefaults.standardUserDefaults boolForKey:kCollapsedKey];
    [self showCollapsed:collapsed_ animated:NO];
    return self;
}

// A touch on the panel's corners outside the frame goes on to the game; so
// does any touch while it is folded but on its arrow.
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)event {
    UIView *v = [super hitTest:p withEvent:event];
    if (v == self)
        return nil;
    if (collapsed_ && v && ![v isDescendantOfView:arrow_])
        return nil;
    return v;
}
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent *)event {
    if ([super pointInside:p withEvent:event])
        return YES;
    return [arrow_ pointInside:[self convertPoint:p toView:arrow_] withEvent:event];
}

// A command holding (a held key, the next tap's right click): its button
// pushed and ringed in gold, so the panel says so at a glance.
static void ring(UIView *v, CGPoint center, CGFloat radius, BOOL on) {
    CAShapeLayer *layer = nil;
    for (CALayer *l in v.layer.sublayers)
        if ([l.name isEqualToString:@"recomp-ring"])
            layer = (CAShapeLayer *)l;
    if (!on) {
        [layer removeFromSuperlayer];
        return;
    }
    if (layer)
        return;
    layer = [CAShapeLayer layer];
    layer.name = @"recomp-ring";
    layer.path = [UIBezierPath bezierPathWithArcCenter:center
                                                radius:radius
                                            startAngle:0
                                              endAngle:2 * M_PI
                                             clockwise:YES]
                     .CGPath;
    layer.fillColor = UIColor.clearColor.CGColor;
    layer.strokeColor = [UIColor colorWithRed:1 green:0.82 blue:0.35 alpha:1].CGColor;
    layer.lineWidth = 3;
    layer.shadowColor = layer.strokeColor;
    layer.shadowRadius = 4;
    layer.shadowOpacity = 0.9;
    layer.shadowOffset = CGSizeZero;
    [v.layer addSublayer:layer];
}

- (void)setActive:(NSSet<NSString *> *)active {
    for (NSString *ident in commandButtons_) {
        RecompRoundButton *b = commandButtons_[ident];
        const BOOL on = [active containsObject:ident];
        b.selected = on;
        ring(b, b.hitCenter, d_ / 2 - 1, on);
    }
}

// Folded: the frame and the cells shrink into the arrow, which turns round to
// point back out.
- (void)showCollapsed:(BOOL)collapsed animated:(BOOL)animated {
    auto apply = ^{
      for (UIView *v in folding_) {
          const CGPoint c = v.center;
          v.transform = collapsed ? CGAffineTransformConcat(
                                        CGAffineTransformMakeTranslation((fold_at_.x - c.x) * 0.85,
                                                                         (fold_at_.y - c.y) * 0.85),
                                        CGAffineTransformMakeScale(0.15, 0.15))
                                  : CGAffineTransformIdentity;
          v.alpha = collapsed ? 0 : 1;
      }
      arrow_.transform =
          collapsed ? CGAffineTransformMakeRotation((CGFloat)M_PI) : CGAffineTransformIdentity;
    };
    if (animated)
        [UIView animateWithDuration:0.22
                              delay:0
                            options:UIViewAnimationOptionCurveEaseInOut
                         animations:apply
                         completion:nil];
    else
        apply();
}

// Held: the command's name and keys show, and its cell lifts to be dragged
// onto another command's; let go elsewhere, it goes back.
- (void)held:(UILongPressGestureRecognizer *)g {
    const NSInteger i = g.view.tag - 100;
    if (i < 0 || i >= (NSInteger)commands_.count)
        return;
    UIView *cell = cells_[(NSUInteger)i];
    const CGPoint at = [g locationInView:self];
    const UIGestureRecognizerState state = g.state;
    if (state == UIGestureRecognizerStateBegan) {
        platform_ui_haptic_tap();
        show_command_tip(cell, commands_[(NSUInteger)i]);
        drag_from_ = cell.center;
        drag_grab_ = CGPointMake(at.x - cell.center.x, at.y - cell.center.y);
        [self bringSubviewToFront:cell];
        [UIView animateWithDuration:0.12
                         animations:^{
                           cell.transform = CGAffineTransformMakeScale(1.15, 1.15);
                           cell.alpha = 0.9;
                         }];
        return;
    }
    if (state == UIGestureRecognizerStateChanged) {
        cell.center = CGPointMake(at.x - drag_grab_.x, at.y - drag_grab_.y);
        return;
    }
    if (state == UIGestureRecognizerStateEnded) {
        // The command whose cell it was let go over, if any.
        for (NSInteger k = 0; k < (NSInteger)cells_.count; ++k) {
            if (k == i)
                continue;
            if (CGRectContainsPoint(cells_[(NSUInteger)k].frame, cell.center) && self.moved) {
                platform_ui_haptic_tap();
                self.moved(commands_[(NSUInteger)i].ident, commands_[(NSUInteger)k].ident);
                return; // the panel is built again in the new order
            }
        }
    }
    if (state == UIGestureRecognizerStateEnded || state == UIGestureRecognizerStateCancelled ||
        state == UIGestureRecognizerStateFailed) {
        const CGPoint home = drag_from_;
        [UIView animateWithDuration:0.18
                         animations:^{
                           cell.center = home;
                           cell.transform = CGAffineTransformIdentity;
                           cell.alpha = 1;
                         }];
    }
}

- (void)tapped:(UIButton *)b {
    platform_ui_haptic_tap();
    if (getenv("RECOMP_TRACE_HOTKEYS"))
        fprintf(stderr, "[panel] button %ld\n", (long)b.tag);
    if (b.tag == 0) {
        collapsed_ = !collapsed_;
        [NSUserDefaults.standardUserDefaults setBool:collapsed_ forKey:kCollapsedKey];
        [self showCollapsed:collapsed_ animated:YES];
    } else if (b.tag >= 100) {
        if (self.pressed)
            self.pressed(commands_[(NSUInteger)(b.tag - 100)]);
    } else if (self.camera) {
        self.camera((int)b.tag);
    }
}
@end

// The panel's state across turns (main thread only).
namespace {
struct PanelState {
    UIView *bar = nil; // the panel, or the plain camera bar without art
    RecompPanelArt *art = nil;
    bool art_looked = false;
    std::string built;      // what the panel was built for (cells, place)
    std::string built_form; // its form: "phone" or "large"
    std::string commands;   // the document last parsed
    uint32_t generation = 0;
    std::string active;
    NSString *screen = @"";                   // the document's screen
    NSArray<RecompPanelCommand *> *all = nil; // its commands, the game's order
    NSArray<NSValue *> *busy = nil;           // the game's interface: fractions of the picture
    bool shown = false;
    void (*command)(const char *) = nullptr;
    // What the cells and their place were last worked out from: the same
    // again (most turns) keeps them, without asking the defaults or placing.
    std::string asked;
    bool visible = false;
    NSArray<RecompPanelCommand *> *shown_commands = nil;
    NSInteger cols = 1;
    CGRect place = CGRectZero;
    std::string key;
};
PanelState g_panel;
} // namespace

static NSSet<NSString *> *active_set() {
    NSString *s = [NSString stringWithUTF8String:g_panel.active.c_str()];
    return [NSSet setWithArray:[s componentsSeparatedByString:@" "]];
}

// The player's order, per screen (the game's name for it): the commands' ids.
static NSString *order_key(NSString *screen) {
    return [@"RecompPanelOrder." stringByAppendingString:screen];
}

// The screen's commands in the player's order (one never moved stays where
// the game's order has it after those before it), else the game's.
static NSArray<RecompPanelCommand *> *ordered_commands() {
    NSArray<RecompPanelCommand *> *all = g_panel.all;
    NSArray *saved = [NSUserDefaults.standardUserDefaults arrayForKey:order_key(g_panel.screen)];
    if (!saved.count)
        return all;
    NSMutableDictionary<NSString *, NSNumber *> *rank = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < saved.count; ++i)
        if ([saved[i] isKindOfClass:NSString.class])
            rank[saved[i]] = @(i);
    NSMutableArray<RecompPanelCommand *> *out = [NSMutableArray array];
    NSMutableArray<RecompPanelCommand *> *unranked = [NSMutableArray array];
    for (RecompPanelCommand *c in all)
        [(rank[c.ident] ? out : unranked) addObject:c];
    [out sortUsingComparator:^NSComparisonResult(RecompPanelCommand *a, RecompPanelCommand *b) {
      return [rank[a.ident] compare:rank[b.ident]];
    }];
    [out addObjectsFromArray:unranked];
    return out;
}

// A command dragged onto another's cell: it takes that place, kept.
static void move_command(NSString *ident, NSString *before, NSArray<RecompPanelCommand *> *shown) {
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (RecompPanelCommand *c in shown)
        [ids addObject:c.ident];
    const NSUInteger from = [ids indexOfObject:ident], to = [ids indexOfObject:before];
    if (from == NSNotFound || to == NSNotFound)
        return;
    [ids removeObjectAtIndex:from];
    [ids insertObject:ident atIndex:to];
    [NSUserDefaults.standardUserDefaults setObject:ids forKey:order_key(g_panel.screen)];
    if (getenv("RECOMP_TRACE_HOTKEYS"))
        fprintf(stderr, "[panel] %s moved to %lu\n", ident.UTF8String, (unsigned long)to);
}

static void press_command(RecompPanelCommand *c) {
    if (g_panel.command)
        g_panel.command(c.ident.UTF8String);
}

static NSArray<RecompPanelCommand *> *parse_commands(const std::string &doc, NSString **screen,
                                                     NSArray<NSValue *> **busy) {
    NSMutableArray<RecompPanelCommand *> *out = [NSMutableArray array];
    NSMutableArray<NSValue *> *rects = [NSMutableArray array];
    *screen = @"";
    *busy = rects;
    if (doc.empty())
        return out;
    NSData *data = [NSData dataWithBytes:doc.data() length:doc.size()];
    NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class])
        return out;
    if ([json[@"screen"] isKindOfClass:NSString.class])
        *screen = json[@"screen"];
    NSArray *list = json[@"busy"];
    if ([list isKindOfClass:NSArray.class])
        for (NSArray *r in list)
            if ([r isKindOfClass:NSArray.class] && r.count == 4 &&
                [r[0] isKindOfClass:NSNumber.class] && [r[1] isKindOfClass:NSNumber.class] &&
                [r[2] isKindOfClass:NSNumber.class] && [r[3] isKindOfClass:NSNumber.class])
                [rects addObject:[NSValue
                                     valueWithCGRect:CGRectMake(
                                                         [r[0] doubleValue], [r[1] doubleValue],
                                                         [r[2] doubleValue] - [r[0] doubleValue],
                                                         [r[3] doubleValue] - [r[1] doubleValue])]];
    NSArray *commands = json[@"commands"];
    if (![commands isKindOfClass:NSArray.class])
        return out;
    for (NSDictionary *d in commands) {
        if (![d isKindOfClass:NSDictionary.class] || ![d[@"id"] isKindOfClass:NSString.class])
            continue;
        RecompPanelCommand *c = [[RecompPanelCommand alloc] init];
        c.ident = d[@"id"];
        c.label = [d[@"label"] isKindOfClass:NSString.class] ? d[@"label"] : c.ident;
        c.brief = [d[@"short"] isKindOfClass:NSString.class] ? d[@"short"] : @"";
        c.keys = [d[@"keys"] isKindOfClass:NSString.class] ? d[@"keys"] : @"";
        c.kind = [d[@"kind"] isKindOfClass:NSString.class] ? d[@"kind"] : @"tap";
        c.symbol = [d[@"symbol"] isKindOfClass:NSString.class] ? d[@"symbol"] : @"";
        c.quick = [d[@"quick"] isKindOfClass:NSNumber.class] ? [d[@"quick"] intValue] : 0;
        [out addObject:c];
    }
    return out;
}

// Where the panel goes: a grid of `count` cells and a place for it inside the
// safe area that keeps clear of the game's interface (`busy`, window points),
// as near as can be to the panel's corner - the top left on a phone (the
// camera housing's side), the top right on a larger screen. None clear: the
// one over the least of it.
static CGRect place_panel(RecompPanelArt *art, bool phone, NSInteger count, CGRect safe,
                          NSArray<NSValue *> *busy, NSInteger *cols_out) {
    CGRect best = CGRectZero;
    CGFloat best_score = CGFLOAT_MAX;
    *cols_out = MAX(count, 1);
    const CGFloat step = 6, gap = 4;
    for (NSInteger rows = 1; rows <= MAX(count, 1); ++rows) {
        const NSInteger cols = (MAX(count, 1) + rows - 1) / rows;
        if ((cols - 1) * rows >= count && rows > 1)
            continue; // the same columns as fewer rows give
        const CGSize size = [RecompControlPanel sizeForArt:art phone:phone cols:cols rows:rows];
        if (size.width > safe.size.width || size.height > safe.size.height)
            continue;
        for (CGFloat y = safe.origin.y; y + size.height <= CGRectGetMaxY(safe); y += step) {
            if (best_score < 1000 && y - safe.origin.y > best_score)
                break; // a clear place nearer the corner is known
            for (CGFloat i = 0; i + size.width <= safe.size.width; i += step) {
                const CGFloat x = phone ? safe.origin.x + i : CGRectGetMaxX(safe) - size.width - i;
                const CGRect r = CGRectMake(x, y, size.width, size.height);
                CGFloat over = 0;
                for (NSValue *v in busy) {
                    const CGRect hit =
                        CGRectIntersection(CGRectInset(v.CGRectValue, -gap, -gap), r);
                    if (!CGRectIsNull(hit))
                        over += hit.size.width * hit.size.height;
                }
                // Clear beats any overlap; then nearest the corner, then fewer rows.
                const CGFloat score = over * 1000 + i + (y - safe.origin.y) + rows * 0.5;
                if (score < best_score) {
                    best_score = score;
                    best = r;
                    *cols_out = cols;
                }
                if (best_score < 1000 && i + (y - safe.origin.y) > best_score)
                    break; // further along this row only moves away
            }
        }
    }
    return best;
}

static void control_panel(SDL_Window *window, const ControlPanel &want, void (*camera)(int),
                          void (*command)(const char *));

// Every turn of the host (up to each millisecond): its temporaries go at once.
void platform_ui_control_panel(SDL_Window *window, const ControlPanel &want, void (*camera)(int),
                               void (*command)(const char *)) {
    @autoreleasepool {
        control_panel(window, want, camera, command);
    }
}

static void control_panel(SDL_Window *window, const ControlPanel &want, void (*camera)(int),
                          void (*command)(const char *)) {
    PanelState &st = g_panel;
    st.command = command;
    if (!st.art_looked) {
        st.art_looked = true;
        st.art = [RecompPanelArt
            artInFolder:[NSBundle.mainBundle.resourcePath
                            stringByAppendingPathComponent:@"touch-ui/camera-panel"]];
    }
    const char *commands_now = want.commands ? want.commands : "";
    if (want.generation != st.generation || strcmp(commands_now, st.commands.c_str()) != 0) {
        const std::string commands = commands_now;
        st.commands = commands;
        st.generation = want.generation;
        st.asked.clear();
        NSString *screen = nil;
        NSArray<NSValue *> *busy = nil;
        st.all = parse_commands(commands, &screen, &busy);
        st.screen = screen;
        st.busy = busy;
    }
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIView *root = uiwindow.rootViewController.view;
    if (!root)
        return;
    // The safe area, a margin in; the game's picture in window points.
    const UIEdgeInsets in = root.safeAreaInsets;
    const CGRect safe = CGRectInset(UIEdgeInsetsInsetRect(root.bounds, in), 6, 6);
    const CGRect pic =
        want.pic_w > 0 ? CGRectMake(want.pic_x, want.pic_y, want.pic_w, want.pic_h) : root.bounds;
    char asked[160];
    snprintf(asked, sizeof asked, "%d%d %.1f,%.1f,%.1f,%.1f %.1f,%.1f,%.1f,%.1f",
             want.phone ? 1 : 0, want.camera ? 1 : 0, safe.origin.x, safe.origin.y, safe.size.width,
             safe.size.height, pic.origin.x, pic.origin.y, pic.size.width, pic.size.height);
    if (st.asked != asked) {
        st.asked = asked;
        NSArray<RecompPanelCommand *> *shown = ordered_commands();
        st.shown_commands = shown;
        st.visible = want.camera || shown.count > 0;
        // The game's interface in window points.
        NSMutableArray<NSValue *> *busy = [NSMutableArray array];
        for (NSValue *v in st.busy) {
            const CGRect f = v.CGRectValue;
            [busy
                addObject:[NSValue valueWithCGRect:CGRectMake(
                                                       pic.origin.x + f.origin.x * pic.size.width,
                                                       pic.origin.y + f.origin.y * pic.size.height,
                                                       f.size.width * pic.size.width,
                                                       f.size.height * pic.size.height)]];
        }
        const NSInteger cells = (want.camera ? 4 : 0) + (NSInteger)shown.count;
        st.cols = 1;
        st.place = CGRectZero;
        if (st.art && st.visible)
            st.place = place_panel(st.art, want.phone, cells, safe, busy, &st.cols);
        // What the panel is built for: its cells and their place.
        st.key = want.phone ? "phone" : "large";
        st.key += want.camera ? " camera" : "";
        for (RecompPanelCommand *c in shown)
            st.key += " " + std::string(c.ident.UTF8String);
        char where[64];
        snprintf(where, sizeof where, " %ld@%.0f,%.0f", (long)st.cols, st.place.origin.x,
                 st.place.origin.y);
        st.key += where;
        static const bool trace = getenv("RECOMP_TRACE_HOTKEYS") != nullptr;
        if (trace)
            fprintf(stderr, "[panel] placed: %ld cells, %ld columns at %.0f,%.0f, %lu busy, %s\n",
                    (long)cells, (long)st.cols, st.place.origin.x, st.place.origin.y,
                    (unsigned long)busy.count, st.visible ? "shown" : "hidden");
    }
    NSArray<RecompPanelCommand *> *shown = st.shown_commands;
    const bool visible = st.visible;
    const NSInteger cols = st.cols;
    const CGRect place = st.place;
    const std::string &key = st.key;
    // Other cells or another place (a screen, a command moved, the game's
    // interface moved, a foldable folded): the panel comes again.
    const std::string form = want.phone ? "phone" : "large";
    // Going away, the panel as it was fades out; it is built again when it
    // comes back.
    if (st.bar && key != st.built && visible) {
        [st.bar removeFromSuperview];
        st.bar = nil;
        if (form != st.built_form)
            st.shown = false; // another size: the new one fades in
    }
    if (!st.bar && visible) {
        st.built = key;
        st.built_form = form;
        RecompControlPanel *panel = st.art ? [[RecompControlPanel alloc] initWithArt:st.art
                                                                               phone:want.phone
                                                                              camera:want.camera
                                                                            commands:shown
                                                                                cols:cols]
                                           : nil;
        if (panel) {
            panel.pressed = ^(RecompPanelCommand *c) {
              press_command(c);
            };
            panel.moved = ^(NSString *ident, NSString *before) {
              move_command(ident, before, shown);
              g_panel.built.clear(); // built again next turn
              g_panel.asked.clear();
            };
            panel.translatesAutoresizingMaskIntoConstraints = YES;
            panel.frame =
                CGRectMake(place.origin.x, place.origin.y, panel.size.width, panel.size.height);
            st.bar = panel;
        } else if (want.camera) {
            st.bar = [[RecompCameraBar alloc] init]; // no art: plain camera buttons
        }
        if (st.bar) {
            st.bar.alpha = st.shown ? 1 : 0;
            [root addSubview:st.bar];
            if (!panel) {
                UILayoutGuide *g = root.safeAreaLayoutGuide;
                [st.bar.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-12]
                    .active = YES;
                [st.bar.topAnchor constraintEqualToAnchor:g.topAnchor constant:72].active = YES;
            }
            if (panel)
                [panel setActive:active_set()];
        }
    }
    if (!st.bar)
        return;
    if ([st.bar isKindOfClass:RecompControlPanel.class])
        ((RecompControlPanel *)st.bar).camera = camera;
    else
        ((RecompCameraBar *)st.bar).press = camera;
    const std::string active = want.active ? want.active : "";
    if (active != st.active) {
        st.active = active;
        if ([st.bar isKindOfClass:RecompControlPanel.class])
            [(RecompControlPanel *)st.bar setActive:active_set()];
    }
    if (visible == st.shown)
        return;
    st.shown = visible;
    st.bar.userInteractionEnabled = visible;
    UIView *bar = st.bar;
    [UIView animateWithDuration:0.18
                     animations:^{
                       bar.alpha = visible ? 1 : 0;
                     }];
}

// ------------------------------------------------------------ split ring --
// A gold ring around a finger that rests while dragging a stack: it fills as
// the rest counts down to a split, then holds full (Shift is down) with a
// divide sign. Drawn over the game; it takes no touches.
@interface RecompSplitRing : UIView
@property(nonatomic) BOOL held;
- (void)setProgress:(double)progress;
@end

@implementation RecompSplitRing {
    CAShapeLayer *track_, *fill_;
    UIImageView *sign_;
}
- (instancetype)init {
    const CGFloat d = 96;
    self = [super initWithFrame:CGRectMake(0, 0, d, d)];
    self.userInteractionEnabled = NO;
    UIBezierPath *circle = [UIBezierPath bezierPathWithArcCenter:CGPointMake(d / 2, d / 2)
                                                          radius:d / 2 - 6
                                                      startAngle:-M_PI_2
                                                        endAngle:3 * M_PI_2
                                                       clockwise:YES];
    track_ = [CAShapeLayer layer];
    track_.path = circle.CGPath;
    track_.fillColor = UIColor.clearColor.CGColor;
    track_.strokeColor = [UIColor colorWithRed:0.08 green:0.06 blue:0.04 alpha:0.45].CGColor;
    track_.lineWidth = 7;
    [self.layer addSublayer:track_];
    fill_ = [CAShapeLayer layer];
    fill_.path = circle.CGPath;
    fill_.fillColor = UIColor.clearColor.CGColor;
    fill_.strokeColor = [UIColor colorWithRed:0.98 green:0.84 blue:0.45 alpha:0.95].CGColor;
    fill_.lineWidth = 5;
    fill_.lineCap = kCALineCapRound;
    fill_.strokeEnd = 0;
    [self.layer addSublayer:fill_];
    UIImageSymbolConfiguration *weight =
        [UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightBold];
    sign_ = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"divide.circle.fill"
                                                       withConfiguration:weight]];
    sign_.tintColor = [UIColor colorWithRed:0.98 green:0.84 blue:0.45 alpha:1];
    sign_.backgroundColor = [UIColor colorWithRed:0.08 green:0.06 blue:0.04 alpha:0.8];
    sign_.layer.cornerRadius = 11;
    sign_.frame = CGRectMake(d - 24, 2, 22, 22);
    sign_.contentMode = UIViewContentModeCenter;
    sign_.alpha = 0;
    [self addSubview:sign_];
    return self;
}
- (void)setProgress:(double)progress {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    fill_.strokeEnd = progress;
    [CATransaction commit];
    const BOOL held = progress >= 1;
    if (held == _held)
        return;
    _held = held;
    sign_.alpha = held ? 1 : 0;
    if (held) {
        self.transform = CGAffineTransformMakeScale(1.12, 1.12);
        [UIView animateWithDuration:0.18
                         animations:^{
                           self.transform = CGAffineTransformIdentity;
                         }];
    }
}
@end

void platform_ui_split_ring(SDL_Window *window, bool visible, double x, double y, double progress) {
    static RecompSplitRing *ring = nil;
    if (!visible) {
        if (ring && !ring.hidden) {
            ring.hidden = YES;
            [ring setProgress:0];
        }
        return;
    }
    if (!ring) {
        UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        UIView *root = uiwindow.rootViewController.view;
        if (!root)
            return;
        ring = [[RecompSplitRing alloc] init];
        [root addSubview:ring];
    }
    [ring.superview bringSubviewToFront:ring];
    ring.hidden = NO;
    ring.center = CGPointMake(x, y);
    const BOOL was_held = ring.held;
    [ring setProgress:progress];
    if (ring.held && !was_held)
        platform_ui_haptic_tap();
}

#if defined(RECOMP_NATIVE_SCREENS)
// The game's SwiftUI screens (native-screens/ in the game repository, built
// into the RecompScreens module): NativeScreens.swift's C entry points.
using ScreensShow = void (*)(void *, const char *, void (*)(const char *));
using ScreensHide = void (*)(void);
#if defined(RECOMP_GAME_MODULE)
// A game module (an expansion in the multi-game app) carries no copy of the
// module: the app's own serves it. Two copies in one process register the
// same Objective-C classes twice (the runtime warns, and one copy then works
// with the other's objects: a crash in Hammers of Fate's ubi-gs login). The
// screens read the game's paks themselves (RECOMP_GAME_ROOT), never the host.
static ScreensShow screens_show() {
    static ScreensShow f = (ScreensShow)dlsym(RTLD_MAIN_ONLY, "recomp_native_screens_show");
    return f;
}
static ScreensHide screens_hide() {
    static ScreensHide f = (ScreensHide)dlsym(RTLD_MAIN_ONLY, "recomp_native_screens_hide");
    return f;
}
#else
extern "C" void recomp_native_screens_show(void *root_view, const char *state,
                                           void (*command)(const char *));
extern "C" void recomp_native_screens_hide(void);
static ScreensShow screens_show() {
    return recomp_native_screens_show;
}
static ScreensHide screens_hide() {
    return recomp_native_screens_hide;
}
#endif
#endif

void platform_ui_native_screen(SDL_Window *window, const char *state, void (*command)(const char *)) {
#if defined(RECOMP_NATIVE_SCREENS)
    if (!state) {
        if (ScreensHide hide = screens_hide())
            hide();
        return;
    }
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIView *root = uiwindow.rootViewController.view;
    ScreensShow show = screens_show();
    if (root && show)
        show((__bridge void *)root, state, command);
#else
    (void)window;
    (void)state;
    (void)command;
#endif
}

bool platform_ui_phone_interface() {
    // host.ui/phone_interface: 0 off, 1 by the screen (an iPhone's), 2 always
    // (an iPad too).
    int64_t on = 1;
    mods_settings_file_get("host.ui/phone_interface", &on);
    return on == 2 || (on != 0 && UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone);
}

void platform_ui_seed_profile(const std::string &profile_dir) {
    const std::string seed = host_resource("profile-seed");
    std::error_code ec;
    if (seed.empty() || profile_dir.empty() || !fs::is_directory(seed, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(seed, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        fs::path rel = fs::relative(it->path(), seed, ec);
        // phone/...: on an iPhone with the launcher's phone interface not off
        // (host.ui/phone_interface: 0 off, 1 by the screen, 2 always - then on
        // an iPad too), at the path without the prefix; taken out of the
        // profile again when it is off. On a foldable iPhone they stay for
        // both screens: the game's side picks the layout by the window's size.
        if (auto first = rel.begin(); first != rel.end() && *first == "phone") {
            fs::path rest;
            for (auto part = std::next(first); part != rel.end(); ++part)
                rest /= *part;
            rel = rest;
            if (!platform_ui_phone_interface()) {
                std::error_code e2;
                if (fs::remove(fs::path(profile_dir) / rel, e2))
                    fprintf(stderr, "[ios] profile file %s: taken out (phone interface off)\n",
                            rel.c_str());
                continue;
            }
        }
        const fs::path dest = fs::path(profile_dir) / rel;
        std::error_code e2;
        if (fs::exists(dest, e2) && fs::file_size(dest, e2) == it->file_size(e2))
            continue;
        fs::create_directories(dest.parent_path(), e2);
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, e2);
        fprintf(stderr, "[ios] profile file %s: %s\n", rel.c_str(),
                e2 ? e2.message().c_str() : "put in place");
    }
}
