"""Load a game directory: games/<id>/game.toml plus its curated globals file.

This module is the only place that knows the schema. Python 3.9 has no
tomllib, so the pinned tomli is the fallback."""

from pathlib import Path
import re

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

REQUIRED_GAME_KEYS = ("id", "name", "app_name", "bundle_id", "executable", "sha256",
                      "image_base", "entry_point", "guest_root", "developer_exe")

HEAP_BASE_DEFAULT = 0x01000000

# The settings page's rows, in mods/display_settings.h DisplayRow order; the
# controls' seven rows are one entry. [settings] rows names the ones a game
# shows. Without the key a game shows every row. "keypad" is the pre-touch-
# controls spelling of "controls"; SETTINGS_ROW_ALIASES keeps it loading.
SETTINGS_ROWS = ("rendering", "ui_scale", "wide_view", "window", "resolution", "frame_limit",
                 "performance_overlay", "textures", "filtering", "controls")
SETTINGS_ROW_ALIASES = {"keypad": "controls"}

# [controls] default_layout and the on-screen layout picker's choices.
CONTROLS_LAYOUTS = ("pad", "keys", "pad+keys", "hidden")
# [controls.mapped] left_stick/right_stick/dpad modes.
STICK_MODES = ("cursor", "arrows", "horizontal_arrows", "wasd", "scroll", "wheel", "none")
# [controls.native] buttons: the pad button names a physical button maps to.
PAD_BUTTONS = ("cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
               "select", "start", "ps")
MAPPED_DEFAULTS = {"left_stick": "arrows", "right_stick": "cursor", "dpad": "arrows",
                   "cursor_speed": 900, "cross": "mouse_left", "circle": "mouse_right",
                   "square": "key:Space", "triangle": "key:Tab", "l1": "key:PageUp",
                   "r1": "key:PageDown", "l2": "mouse_middle", "r2": "key:LShift",
                   "start": "key:Escape", "select": "key:F10", "l3": "none", "r3": "none",
                   "ps": "action:settings"}
# [controls.native] axes: the six DirectInput/XInput axis names a physical
# axis maps to.
NATIVE_AXES = ("x", "y", "z", "rx", "ry", "rz")
# Mirrors the name column of host/controls/layout.cpp's kScancodes table (its
# scancode_from_name); keep both lists in sync.
KEY_NAMES = (
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Return",
    "Escape", "Backspace", "Tab", "Space", "Minus", "Equals", "LeftBracket", "RightBracket",
    "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma", "Period", "Slash", "F1", "F2", "F3",
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Insert", "Home", "PageUp", "Delete",
    "End", "PageDown", "Right", "Left", "Down", "Up", "LCtrl", "LShift", "LAlt",
)
# key:<name>, mouse_left/right/middle, wheel_up/down, action:<name> or none.
BUTTON_TARGET_RE = re.compile(
    r"^(key:[A-Za-z0-9]+|mouse_(left|right|middle)|wheel_(up|down)|"
    r"action:(settings|system_keyboard|edit_layout)|none)$")
HEAP_END = 0x0e000000        # runtime/x86.h GUEST_HEAP_END; the mods' heap starts there
GUEST_SIZE_DEFAULT = 0x10000000   # runtime/x86.h GUEST_SIZE: the arena, 256 MB unless a module needs more
AUX_REQUIRED_KEYS = ("name", "path", "sha256", "base", "size")


def windows_version(value):
    """Decode major.minor[.build]; keep the historical 9x default and 6.1 SP1."""
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", value):
        raise ValueError("[game] windows_version must be major.minor[.build]")
    parts = [int(part) for part in value.split(".")]
    major, minor = parts[:2]
    build = parts[2] if len(parts) == 3 else {(4, 10): 2222, (6, 1): 7601}.get((major, minor), 0)
    if major > 255 or minor > 255 or build > 32767:
        raise ValueError("[game] windows_version requires byte-sized major/minor and a 15-bit build")
    return major, minor, build, 2 if major >= 5 else 1


def validate_heap_base(value):
    """The heap arena start: page aligned, above the image base, below the arena end."""
    if value % 0x1000 or not (0x00400000 < value < HEAP_END):
        raise ValueError("[game] heap_base %#x must be page aligned and between 0x00400000 and %#x" % (value, HEAP_END))
    return value


def load_controls(controls, touch, source):
    """Validate [controls], merging [controls.mapped]/[controls.native] over their
    defaults. `touch` is the already-defaulted [touch] table: its `keypad` knob
    only sets `default_layout` when the game has not named one itself."""
    controls = dict(controls)
    if "default_layout" not in controls:
        controls["default_layout"] = {"auto": "keys", "hidden": "hidden"}[touch["keypad"]]
    if controls["default_layout"] not in CONTROLS_LAYOUTS:
        raise ValueError("%s: [controls] default_layout must be one of %s, not %r"
                         % (source, ", ".join(CONTROLS_LAYOUTS), controls["default_layout"]))
    pad = controls.setdefault("pad", "mapped")
    if pad not in ("native", "mapped", "off"):
        raise ValueError('%s: [controls] pad must be "native", "mapped" or "off", not %r' % (source, pad))

    mapped = dict(MAPPED_DEFAULTS)
    mapped.update(controls.get("mapped", {}))
    unknown = sorted(k for k in mapped if k not in MAPPED_DEFAULTS)
    if unknown:
        raise ValueError("%s: [controls.mapped] may name only %s, not %s"
                         % (source, ", ".join(sorted(MAPPED_DEFAULTS)), ", ".join(unknown)))
    for key, value in mapped.items():
        if key == "cursor_speed":
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                raise ValueError("%s: [controls.mapped] cursor_speed must be a positive integer, not %r"
                                 % (source, value))
        elif key in ("left_stick", "right_stick"):
            if value not in STICK_MODES:
                raise ValueError("%s: [controls.mapped] %s must be one of %s, not %r"
                                 % (source, key, ", ".join(STICK_MODES), value))
        elif key == "dpad":
            if value not in ("arrows", "wasd", "none"):
                raise ValueError('%s: [controls.mapped] dpad must be "arrows", "wasd" or "none", not %r'
                                 % (source, value))
        elif not isinstance(value, str) or not BUTTON_TARGET_RE.match(value):
            raise ValueError("%s: [controls.mapped] %s is not a valid target: %r" % (source, key, value))
        elif value.startswith("key:") and value[len("key:"):] not in KEY_NAMES:
            raise ValueError("%s: [controls.mapped] %s names no key: %r" % (source, key, value))
    controls["mapped"] = mapped

    native = dict(controls.get("native", {}))
    unknown_native = sorted(k for k in native if k not in ("xinput", "dinput", "axes", "buttons"))
    if unknown_native:
        raise ValueError("%s: [controls.native] may name only xinput, dinput, axes, buttons, not %s"
                         % (source, ", ".join(unknown_native)))
    native.setdefault("xinput", True)
    native.setdefault("dinput", True)
    if not isinstance(native["xinput"], bool) or not isinstance(native["dinput"], bool):
        raise ValueError("%s: [controls.native] xinput and dinput must be booleans" % source)
    axes = native.setdefault("axes", ["x", "y", "z", "rz", "rx", "ry"])
    if not isinstance(axes, list) or sorted(axes) != sorted(NATIVE_AXES):
        raise ValueError("%s: [controls.native] axes must list all six of %s exactly once, not %r"
                         % (source, ", ".join(NATIVE_AXES), axes))
    buttons = native.setdefault(
        "buttons", ["square", "cross", "circle", "triangle", "l1", "r1", "l2", "r2", "select",
                   "start", "l3", "r3", "ps"])
    if not isinstance(buttons, list) or sorted(buttons) != sorted(PAD_BUTTONS):
        raise ValueError("%s: [controls.native] buttons must list all thirteen of %s exactly once, not %r"
                         % (source, ", ".join(PAD_BUTTONS), buttons))
    controls["native"] = native
    return controls


def load(game_dir):
    """Return the parsed config with `globals` merged in and `dir`/`source` recorded."""
    game_dir = Path(game_dir)
    source = game_dir / "game.toml"
    with source.open("rb") as fh:
        cfg = tomllib.load(fh)
    game = cfg.get("game", {})
    missing = [key for key in REQUIRED_GAME_KEYS if key not in game]
    if missing:
        raise ValueError("%s: missing [game] keys: %s" % (source, ", ".join(missing)))
    game["heap_base"] = validate_heap_base(int(game.get("heap_base", HEAP_BASE_DEFAULT)))
    windows_version(game.setdefault("windows_version", "4.10"))
    # The ANSI code page GetACP reports and CP_ACP conversions use: 1252 (Western) unless the
    # game was localised for another (a Russian release names its files and profiles in 1251).
    if not isinstance(game.setdefault("cpuid_sse", False), bool):
        raise ValueError("%s: [game] cpuid_sse must be a boolean" % source)
    # sockets: WinSock on the host's sockets (runtime/winsock.cpp) for a game
    # that plays over a network; off, every socket call fails offline.
    if not isinstance(game.setdefault("sockets", False), bool):
        raise ValueError("%s: [game] sockets must be a boolean" % source)
    acp = game.setdefault("ansi_codepage", 1252)
    if acp not in (1251, 1252):
        raise ValueError("%s: [game] ansi_codepage must be 1251 or 1252" % source)
    # What a renamed app was called before: the host moves the user-data folder
    # of the first former app_name that exists to the new name, once, and the
    # macOS launcher takes its remembered folders and choices from the defaults
    # of the first former bundle_id that has them.
    for key, current, pattern in (("previous_app_names", "app_name", r"[A-Za-z0-9_-]+"),
                                  ("previous_bundle_ids", "bundle_id", r"[A-Za-z0-9.-]+")):
        names = game.setdefault(key, [])
        if not isinstance(names, list) or any(
                not isinstance(n, str) or not re.fullmatch(pattern, n) or n == game[current] for n in names):
            raise ValueError("%s: [game] %s must list former names, not the current %s: %r"
                             % (source, key, current, names))
    translate = cfg.setdefault("translate", {})
    resumable = translate.setdefault("resumable_stacks", False)
    if not isinstance(resumable, bool):
        raise ValueError("%s: [translate] resumable_stacks must be a boolean" % source)
    if not isinstance(translate.setdefault("store_hooks", True), bool):
        raise ValueError("%s: [translate] store_hooks must be a boolean" % source)
    if not isinstance(translate.setdefault("fixed_guest_base", False), bool):
        raise ValueError("%s: [translate] fixed_guest_base must be a boolean" % source)
    if translate.setdefault("write_track", "stores") not in ("stores", "protect"):
        raise ValueError('%s: [translate] write_track must be "stores" or "protect"' % source)
    if translate.setdefault("flag_liveness", "block") not in ("block", "cfg"):
        raise ValueError('%s: [translate] flag_liveness must be "block" or "cfg"' % source)
    if not isinstance(translate.setdefault("lean_calls", False), bool):
        raise ValueError("%s: [translate] lean_calls must be a boolean" % source)
    if not isinstance(translate.setdefault("ret_hint", False), bool):
        raise ValueError("%s: [translate] ret_hint must be a boolean" % source)
    if not isinstance(translate.setdefault("reg_cache", False), bool):
        raise ValueError("%s: [translate] reg_cache must be a boolean" % source)
    alignment = translate.setdefault("function_alignment", 16)
    if type(alignment) is not int or alignment <= 0:
        raise ValueError("%s: [translate] function_alignment must be a positive integer" % source)
    tracks = cfg.setdefault("media", {}).setdefault("cd_tracks", [])
    if not isinstance(tracks, list) or not all(isinstance(v, str) for v in tracks):
        raise ValueError("%s: [media] cd_tracks must be a list of strings" % source)
    cfg.setdefault("hooks", {})
    cfg.setdefault("bundle", {}).setdefault("exclude", [])
    # [net] movable_udp_ports: UDP ports the game binds exclusively that a
    # second copy on the same machine moves off instead of failing to bind
    # (runtime/winsock.cpp). Empty: every bind fails as it would on Windows.
    net = cfg.setdefault("net", {})
    net.setdefault("movable_udp_ports", [])
    if not isinstance(net["movable_udp_ports"], list) or any(
            not isinstance(p, int) or isinstance(p, bool) or not 0 < p < 65536
            for p in net["movable_udp_ports"]):
        raise ValueError("%s: [net] movable_udp_ports must be a list of ports, not %r"
                         % (source, net["movable_udp_ports"]))
    touch = cfg.setdefault("touch", {})
    touch.setdefault("keypad", "auto")
    if touch["keypad"] not in ("auto", "hidden"):
        raise ValueError('%s: [touch] keypad must be "auto" or "hidden", not %r' % (source, touch["keypad"]))
    # "classic": a long press right-clicks and a drag after it is a wheel-button
    # drag. "hover": a finger that rests only moves the cursor (games that show
    # information on hover), a second finger's tap right-clicks.
    # pan_gain: how much a one-finger pan's wheel-button drag is scaled.
    touch.setdefault("pan_gain", 1.0)
    if not isinstance(touch["pan_gain"], (int, float)) or isinstance(touch["pan_gain"], bool) \
            or touch["pan_gain"] <= 0:
        raise ValueError("%s: [touch] pan_gain must be a positive number, not %r" % (source, touch["pan_gain"]))
    # invert: the camera gestures that answer the other way round.
    touch.setdefault("invert", [])
    if not isinstance(touch["invert"], list) or \
            any(v not in ("panx", "pany", "zoom", "rotate") for v in touch["invert"]):
        raise ValueError('%s: [touch] invert may list "panx", "pany", "zoom", "rotate", not %r'
                         % (source, touch["invert"]))
    # [ios] cfg_defaults: settings a touch device starts with, written into the
    # game's own config files ("setvar key = value" lines) before it runs:
    # { "profiles/default_profile/user.cfg" = { gfx_resolution = "1600x1200" } }.
    ios = cfg.setdefault("ios", {})
    # display_name: the name under the icon on the home screen, where a long
    # one is cut short (default: the game's name).
    ios.setdefault("display_name", cfg["game"]["name"])
    # portrait: whether an iPhone may hold the game upright (the kit then
    # composes it below the top strip); an iPad always plays in landscape.
    # The native launcher stands upright on an iPhone either way.
    ios.setdefault("portrait", True)
    if not isinstance(ios["portrait"], bool):
        raise ValueError("%s: [ios] portrait must be true or false" % source)
    if not isinstance(ios["display_name"], str) or not ios["display_name"].strip():
        raise ValueError("%s: [ios] display_name must be a non-empty string" % source)
    # profile_files: files a touch build puts in the player's profile, which the
    # game sees over its own folder (the asset overlay): { "UserMODs/X.h5u" =
    # "build/x.h5u" }, the source relative to the game directory. A source that
    # is not there is skipped (a file derived from the developer's own data).
    # A profile path under phone/ goes in only on an iPhone, without the prefix.
    files = ios.setdefault("profile_files", {})
    if not isinstance(files, dict) or not all(
            isinstance(k, str) and isinstance(v, str) and k and ".." not in k.split("/")
            for k, v in files.items()):
        raise ValueError('%s: [ios] profile_files maps profile paths to source files' % source)
    # [macos], [linux] and [android] profile_files: the same for the desktop
    # apps and Android (no phone/ files there).
    for section in ("macos", "linux", "android"):
        desktop_files = cfg.setdefault(section, {}).setdefault("profile_files", {})
        if not isinstance(desktop_files, dict) or not all(
                isinstance(k, str) and isinstance(v, str) and k and ".." not in k.split("/")
                for k, v in desktop_files.items()):
            raise ValueError('%s: [%s] profile_files maps profile paths to source files' % (source, section))
    # ui_mods: the game's interface mods made on the player's device from its
    # own install (host/ui_mods.cpp), by name: the game directory's
    # ui-mods/<name>.recipe goes into the app. Per platform, [ios], [macos],
    # [linux] and [android]. One Android app runs on a Chromebook (keyboard
    # and touchpad, a window) and on a phone or tablet: [android] ui_mods are
    # the desktop-class device's, touch_ui_mods a touch device's (the host
    # picks by the device it runs on; host/sdl/main.cpp, tools/build.py).
    for section, key in (("ios", "ui_mods"), ("macos", "ui_mods"), ("linux", "ui_mods"),
                         ("android", "ui_mods"), ("android", "touch_ui_mods")):
        mods = cfg[section].setdefault(key, [])
        if not isinstance(mods, list) or not all(
                isinstance(m, str) and m and "/" not in m and m not in (".", "..") for m in mods):
            raise ValueError('%s: [%s] %s must be a list of recipe names' % (source, section, key))
    edits = ios.setdefault("cfg_defaults", {})
    if not isinstance(edits, dict) or not all(
            isinstance(f, str) and isinstance(kv, dict) and
            all(isinstance(k, str) and isinstance(v, str) for k, v in kv.items())
            for f, kv in edits.items()) or any(
            c in s for f, kv in edits.items() for s in [f, *kv, *kv.values()] for c in "|;\n"):
        raise ValueError('%s: [ios] cfg_defaults must map file paths to {key = "value"} tables' % source)
    # cfg_seeds: a config file an install may lack, made from another before
    # cfg_defaults are set (the English Heroes V has no user_a1.cfg, which
    # Hammers of Fate reads, the same file as its user.cfg in the Russian one):
    # { "profiles/default_profile/user_a1.cfg" = "profiles/default_profile/user.cfg" }.
    seeds = ios.setdefault("cfg_seeds", {})
    if not isinstance(seeds, dict) or not all(
            isinstance(f, str) and isinstance(s, str) and f and s and
            ".." not in f.split("/") and ".." not in s.split("/") for f, s in seeds.items()) or any(
            c in s for f, src in seeds.items() for s in (f, src) for c in "|;\n"):
        raise ValueError('%s: [ios] cfg_seeds must map file paths to the file each is made from' % source)
    # [texts]: words of the game's own texts the port changes ("Exit to Windows"
    # on a Mac, a sound's volume in its .xdb), rebuilt from the player's install
    # into a pak the game loads over its own (host/text_overrides.cpp).
    texts = cfg.setdefault("texts", {})
    texts.setdefault("pak", "")
    texts.setdefault("files", [])
    texts.setdefault("replace", {})
    if not isinstance(texts["pak"], str) or not isinstance(texts["files"], list) or \
            not all(isinstance(f, str) for f in texts["files"]) or \
            not isinstance(texts["replace"], dict) or \
            not all(isinstance(k, str) and isinstance(v, str) and k for k, v in texts["replace"].items()) or \
            any(c in s for s in [*texts["files"], *texts["replace"], *texts["replace"].values()] for c in "|;"):
        raise ValueError('%s: [texts] takes pak = "path", files = ["path", ...] and '
                         'replace = { "old words" = "new words" }' % source)
    # [online]: a Ubisoft Game Service title's server list. gsconnect_host is
    # the host its libcurl asks (runtime/curl_http.cpp sends that request to a
    # community server the player names instead); prompt_title and prompt_text
    # are what the host's question says (host/server_prompt.cpp); default_server
    # ("host" or "host:port") is the one it offers while the player has used
    # none. No host: the game stays offline, as every other request does.
    online = cfg.setdefault("online", {})
    online.setdefault("gsconnect_host", "")
    online.setdefault("prompt_title", "")
    online.setdefault("prompt_text", "")
    online.setdefault("default_server", "")
    keys = ("gsconnect_host", "prompt_title", "prompt_text", "default_server")
    if not all(isinstance(online[k], str) for k in keys) or \
            any(c in online["gsconnect_host"] for c in "/:@ \t\n") or \
            (online["default_server"] and not re.fullmatch(
                r"(\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9_][A-Za-z0-9._-]*)(:[0-9]{1,5})?", online["default_server"])):
        raise ValueError('%s: [online] takes gsconnect_host = "host.name", prompt_title = "...", '
                         'prompt_text = "..." and default_server = "host" or "host:port"' % source)
    touch.setdefault("gestures", "classic")
    if touch["gestures"] not in ("classic", "hover"):
        raise ValueError('%s: [touch] gestures must be "classic" or "hover", not %r' % (source, touch["gestures"]))
    cfg["controls"] = load_controls(cfg.get("controls", {}), touch, source)
    settings = cfg.setdefault("settings", {})
    rows = settings.setdefault("rows", list(SETTINGS_ROWS))
    if not isinstance(rows, list):
        raise ValueError("%s: [settings] rows must be a list, not %r" % (source, rows))
    rows = [SETTINGS_ROW_ALIASES.get(row, row) for row in rows]
    unknown = [row for row in rows if row not in SETTINGS_ROWS]
    if unknown:
        raise ValueError("%s: [settings] rows may name only %s, not %s"
                         % (source, ", ".join(SETTINGS_ROWS), ", ".join(map(repr, unknown))))
    settings["rows"] = rows
    # The World filtering row's value for a profile that never chose one: 3 (8x, the kit's
    # original default) unless the game keeps its own texture filtering (0 = original).
    fd = settings.setdefault("filtering_default", 3)
    if not isinstance(fd, int) or not 0 <= fd <= 4:
        raise ValueError("%s: [settings] filtering_default must be 0-4, not %r" % (source, fd))
    # Its saved key. A game that once stored a value it never used (the row unlisted) names a new
    # key here, so that stale value is not suddenly applied when the row is listed.
    fk = settings.setdefault("filtering_key", "texture_filtering")
    if not isinstance(fk, str) or not fk.replace("_", "").isalnum():
        raise ValueError("%s: [settings] filtering_key must be a plain name, not %r" % (source, fk))
    # The settings page's form: "list", the original single page, or "tabs", the Graphics / Mods /
    # Controls menu with an Apply/Discard draft for renderer changes.
    page = settings.setdefault("page", "list")
    if page not in ("list", "tabs"):
        raise ValueError('%s: [settings] page must be "list" or "tabs", not %r' % (source, page))
    # Presentation: "integer" scales the frame by whole multiples (crisp pixel art, borders when
    # the window is not a multiple); "smooth" fills the window at any size, aspect preserved.
    scaling = settings.setdefault("scaling", "integer")
    if scaling not in ("integer", "smooth"):
        raise ValueError('%s: [settings] scaling must be "integer" or "smooth", not %r' % (source, scaling))
    launcher = cfg.setdefault("launcher", {})
    launcher.setdefault("title", game["name"])
    launcher.setdefault("store", "")
    for key in ("install_names", "gog_ids", "steam_ids"):
        value = launcher.setdefault(key, [])
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise ValueError("%s: [launcher] %s must be a list of strings" % (source, key))
    for key in ("title", "store"):
        if not isinstance(launcher[key], str):
            raise ValueError("%s: [launcher] %s must be a string" % (source, key))
    # Where launcher-games.json, launcher-manifests/ and launcher-splash.jpg
    # are: the game's own directory, or the repository an expansion's
    # games/<id> sits in (a standalone build of it shows the same launcher).
    if not isinstance(launcher.setdefault("config_dir", "."), str):
        raise ValueError("%s: [launcher] config_dir must be a string" % source)
    min_free = launcher.setdefault("min_free_mb", 0)
    if not isinstance(min_free, int) or min_free < 0:
        raise ValueError("%s: [launcher] min_free_mb must be a non-negative integer" % source)
    setup_dirs = cfg.setdefault("setup", {}).setdefault("required_dirs", [])
    if not isinstance(setup_dirs, list):
        raise ValueError("%s: [setup] required_dirs must be a list" % source)
    globals_path = game_dir / translate.get("globals", "globals.toml")
    with globals_path.open("rb") as fh:
        cfg["globals"] = tomllib.load(fh).get("globals", {})
    cfg["dir"] = game_dir
    cfg["source"] = str(source)
    # Developer inputs live beside game.toml: a game repository holds its own
    # ignored original/ and analysis/ directories.
    cfg["developer_exe_path"] = (game_dir / game["developer_exe"]).resolve()
    # The directory holding the executable as the game sees it: when the exe
    # itself is a link into another folder, its own parent would be that folder.
    cfg["developer_game_dir"] = (game_dir / game["developer_exe"]).parent.resolve()
    # What a bundle carries: the install the executable belongs to. A game that
    # keeps it in a subfolder (bin/, bina1/ of a GOG install) needs the whole
    # install, the folder above holding the [setup] required_dirs.
    stage = cfg["developer_game_dir"]
    if setup_dirs and not all((stage / d).is_dir() for d in setup_dirs) \
            and all((stage.parent / d).is_dir() for d in setup_dirs):
        stage = stage.parent
    cfg["developer_stage_dir"] = stage
    cfg["launcher_config_dir"] = (game_dir / launcher["config_dir"]).resolve()
    cfg["listings_path"] = (game_dir / translate.get("listings", "analysis")).resolve()
    # [translate] overrides: a header the generated sources include before they
    # define FN_<addr>, so a game can replace one translated function with a
    # native one (translate.py's RECOMP_OVERRIDE_HEADER). Absent by default,
    # and required to exist when named: a path that silently does not resolve
    # would leave the build looking replaced while running the original.
    overrides = translate.get("overrides")
    cfg["overrides_header"] = None
    if overrides is not None:
        path = (game_dir / overrides).resolve()
        if not path.is_file():
            raise ValueError("%s: [translate] overrides names no file: %s" % (source, path))
        cfg["overrides_header"] = path
    cfg["aux_modules"] = load_aux_modules(cfg, game_dir, source)
    return cfg


def load_aux_modules(cfg, game_dir, source):
    """[modules.aux.<key>]: a DLL the guest loads at run time (LoadLibrary) that
    the kit translates as a second image and maps at its preferred base, so
    its code runs as translated code and its exports answer GetProcAddress.
    Keys: name (the file name the guest asks for), path (developer copy,
    relative to game.toml), sha256, base and size (the PE's preferred base
    and SizeOfImage), listings (Ghidra export directory, relative), and
    function_alignment (default 4). [game] guest_size must reach past every
    module; the default arena is 0x10000000."""
    game = cfg["game"]
    guest_size = int(game.setdefault("guest_size", GUEST_SIZE_DEFAULT))
    if guest_size % 0x1000 or not GUEST_SIZE_DEFAULT <= guest_size <= 0x7ffff000:
        raise ValueError("%s: [game] guest_size %#x must be page aligned and at least %#x"
                         % (source, guest_size, GUEST_SIZE_DEFAULT))
    game["guest_size"] = guest_size
    modules = []
    for key, entry in sorted(cfg.get("modules", {}).get("aux", {}).items()):
        missing = [k for k in AUX_REQUIRED_KEYS if k not in entry]
        if missing:
            raise ValueError("%s: [modules.aux.%s] missing keys: %s" % (source, key, ", ".join(missing)))
        base, size = int(entry["base"]), int(entry["size"])
        if base % 0x1000 or size <= 0 or base + size > guest_size:
            raise ValueError("%s: [modules.aux.%s] base %#x size %#x must fit below guest_size %#x"
                             % (source, key, base, size, guest_size))
        alignment = entry.get("function_alignment", 4)
        if type(alignment) is not int or alignment <= 0:
            raise ValueError("%s: [modules.aux.%s] function_alignment must be a positive integer" % (source, key))
        entries = entry.get("entry_points", [])
        if not isinstance(entries, list) or any(type(a) is not int or not base <= a < base + size
                                               for a in entries):
            raise ValueError("%s: [modules.aux.%s] entry_points must be addresses inside the module"
                             % (source, key))
        ranges = entry.get("data_ranges", [])
        if not isinstance(ranges, list) or any(
                not isinstance(r, list) or len(r) != 2 or any(type(v) is not int for v in r)
                or not base <= r[0] < r[1] <= base + size for r in ranges):
            raise ValueError("%s: [modules.aux.%s] data_ranges must be [start, end] pairs inside the module"
                             % (source, key))
        modules.append({
            "key": key, "name": entry["name"], "sha256": entry["sha256"], "base": base, "size": size,
            "path": (game_dir / entry["path"]).resolve(),
            "listings_path": (game_dir / entry.get("listings", "analysis/" + entry["name"])).resolve(),
            "function_alignment": alignment,
            "entry_points": entries,
            "data_ranges": [tuple(r) for r in ranges],
        })
    extra_base = game.setdefault("heap_extra_base", 0)
    extra_size = game.setdefault("heap_extra_size", 0)
    if (type(extra_base) is not int or type(extra_size) is not int
            or bool(extra_base) != bool(extra_size)
            or extra_base < 0 or extra_size < 0
            or (extra_base and (extra_base < GUEST_SIZE_DEFAULT or extra_base % 0x1000
                                or extra_size % 0x1000 or extra_base + extra_size > guest_size))):
        raise ValueError("%s: [game] heap_extra_base/heap_extra_size must define a page-aligned "
                         "range above the fixed runtime layout and inside guest_size" % source)
    for module in modules:
        if extra_size and extra_base < module["base"] + module["size"] and module["base"] < extra_base + extra_size:
            raise ValueError("%s: extra heap overlaps auxiliary module %s" % (source, module["key"]))
    return modules
