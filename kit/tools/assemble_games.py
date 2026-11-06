#!/usr/bin/env python3
"""Put other games' modules into the app whose launcher picks between them.

    tools/assemble_games.py --app build/HommaRe.app \\
        --game games/hof --game games/toe [--platform macos|ios-sim|ios|linux|windows]

The app is a RECOMP_GAME_MODULES build (tools/build.py --game-modules): it
runs its own game and, when the launcher picks another, loads that game's
libgame.<id>.dylib from its Frameworks (host/sdl/main.cpp). Each --game is a
game directory built the same way; its module goes into Frameworks and its
resources (symbols.json, controls, ...) into Resources/games/<app_name>/
(games/<app_name>/ in a flat iOS bundle), where the module looks for them.
The app is signed again afterwards with the identity it carries (ad hoc for
macOS and the simulator). On Linux the app is the desktop package's folder
(build/package/<app_name>): libgame.<id>.so goes into its modules/ and the
resources into resources/games/<app_name>/; nothing is signed, and the
package's tarball beside the folder (tools/package_desktop.py packs it before
the other games are here) gets the same files. Windows is laid out the same
way (build/windows/package/<app_name>, libgame.<id>.dll in modules/), and its
zip gets the files.
"""

import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_desktop import strip_debug  # noqa: E402

# The desktop platforms, whose app is tools/package_desktop.py's folder.
DESKTOP = ("linux", "windows")
# Top-level names of a game's app that are not its resources. The launcher's
# files ([launcher] config_dir) are the app's that shows the launcher.
NOT_RESOURCES = {"Info.plist", "PkgInfo", "_CodeSignature", "Frameworks", "PlugIns", "embedded.mobileprovision",
                 "game", "Assets.car", "MacOS", "launcher-games.json", "launcher-manifests",
                 "launcher-splash.jpg", "innoextract-NOTICE.md", "innoextract-license"}


def load_game(game_dir):
    cfg = tomllib.loads((game_dir / "game.toml").read_text())
    return cfg["game"]["id"], cfg["game"]["app_name"]


def built_app(build_root, app_name, platform):
    """Where tools/build.py leaves a game's own app for this platform."""
    if platform == "macos":
        return build_root / (app_name + ".app")
    if platform == "linux":
        return build_root / "package" / app_name  # tools/package_desktop.py
    if platform == "windows":
        return build_root / "windows" / "package" / app_name  # the windows-cross presets' folder
    # The Xcode generator puts its products under the configuration's folder.
    for path in (build_root / platform / "Release" / (app_name + ".app"), build_root / platform / (app_name + ".app")):
        if path.is_dir():
            return path
    return build_root / platform / (app_name + ".app")


def module_path(build_root, game_id, platform):
    """libgame.<id>.dylib (.so, .dll) as CMake builds it (host/CMakeLists.txt recomp_game)."""
    name = "libgame.%s.%s" % (game_id, {"linux": "so", "windows": "dll"}.get(platform, "dylib"))
    candidates = [build_root / "modules" / name, build_root / platform / "modules" / name,
                  build_root / platform / "modules" / "Release" / name]
    if platform not in ("macos", "linux"):
        candidates = candidates[1:]
    candidates += sorted(build_root.glob("cmake/*/modules/%s" % name))
    candidates += sorted(build_root.glob("cmake/**/Release*/%s" % name))
    for path in candidates:
        if path.is_file():
            return path
    raise SystemExit("No %s under %s: build that game with tools/build.py --game-modules" % (name, build_root))


def copy_resources(source_app, dest, platform):
    """A game's resources as its own app carries them."""
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    if platform in DESKTOP:
        root, executable = source_app / "resources", ""
    else:
        root = source_app / "Contents/Resources" if platform == "macos" else source_app
        executable = plistlib.loads((root.parent / "Info.plist" if platform == "macos" else root / "Info.plist")
                                    .read_bytes()).get("CFBundleExecutable", "")
    for entry in root.iterdir():
        if entry.name in NOT_RESOURCES or entry.name == executable or entry.name.endswith(".lproj") \
                or entry.name.startswith("AppIcon") or entry.name.startswith("Icon-"):
            continue
        target = dest / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, symlinks=True)
        else:
            shutil.copy2(entry, target)


def signing_identity(app):
    """The identity the app is signed with: "-" for ad hoc."""
    info = subprocess.run(["codesign", "-dvv", str(app)], capture_output=True, text=True)
    for line in info.stderr.splitlines():
        match = re.match(r"Authority=(.+)", line)
        if match:
            return match.group(1)
    return "-"


def sign(app, platform, dylibs):
    identity = signing_identity(app)
    entitlements = None
    if identity != "-":
        out = subprocess.run(["codesign", "-d", "--entitlements", ":-", str(app)], capture_output=True)
        if out.stdout.strip():
            entitlements = Path(tempfile.mkstemp(suffix=".plist")[1])
            entitlements.write_bytes(out.stdout)
    for dylib in dylibs:
        subprocess.run(["codesign", "--force", "--sign", identity, "--timestamp=none", str(dylib)], check=True)
    command = ["codesign", "--force", "--sign", identity, "--timestamp=none"]
    if entitlements:
        command += ["--entitlements", str(entitlements)]
    elif platform == "macos":
        command += ["--preserve-metadata=entitlements,identifier"]
    subprocess.run(command + [str(app)], check=True)
    print("signed %s (%s)" % (app, "ad hoc" if identity == "-" else identity))


def add_to_tarballs(app, added):
    """Add `added` (paths inside the Linux package folder `app`) to its tarballs.

    tools/package_desktop.py writes <app_name>-linux-<arch>.tar.gz beside the
    folder before this script puts the other games in, so a release archive
    held the launcher's own game only. Each archive is rewritten with its own
    members, less any earlier copy of the added paths, plus the added files;
    nothing else in the folder is swept in, as it may hold the player's files.
    """
    top = app.name
    names = {top + "/" + p.relative_to(app).as_posix() for p in added}
    for archive in sorted(app.parent.glob(top + "-linux-*.tar.gz")):
        fd, tmp = tempfile.mkstemp(dir=archive.parent, suffix=".tar.gz")
        os.close(fd)
        try:
            with tarfile.open(archive) as old, tarfile.open(tmp, "w:gz") as new:
                for member in old.getmembers():
                    if member.name in names or any(member.name.startswith(n + "/") for n in names):
                        continue
                    new.addfile(member, old.extractfile(member) if member.isfile() else None)
                for path in added:
                    new.add(path, arcname=top + "/" + path.relative_to(app).as_posix())
            os.chmod(tmp, archive.stat().st_mode)  # mkstemp's file is the owner's alone
            os.replace(tmp, archive)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        print("updated %s" % archive)


def add_to_zips(app, added):
    """Add `added` (paths inside the Windows package folder `app`) to its zips,
    as add_to_tarballs does for Linux: each zip is rewritten with its own
    entries, less any earlier copy of the added paths, plus the added files."""
    top = app.name
    names = {top + "/" + p.relative_to(app).as_posix() for p in added}
    for archive in sorted(app.parent.glob(top + "-windows-*.zip")):
        fd, tmp = tempfile.mkstemp(dir=archive.parent, suffix=".zip")
        os.close(fd)
        try:
            with zipfile.ZipFile(archive) as old, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED,
                                                                 compresslevel=9) as new:
                for info in old.infolist():
                    if info.filename in names or any(info.filename.startswith(n + "/") for n in names):
                        continue
                    new.writestr(info, old.read(info))
                for path in added:
                    files = sorted(f for f in path.rglob("*") if f.is_file()) if path.is_dir() else [path]
                    for f in files:
                        new.write(f, top + "/" + f.relative_to(app).as_posix())
            os.chmod(tmp, archive.stat().st_mode)
            os.replace(tmp, archive)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        print("updated %s" % archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, required=True, help="The launcher's app (a --game-modules build)")
    parser.add_argument("--game", type=Path, action="append", default=[], help="Another game directory")
    parser.add_argument("--build-root", type=Path, action="append", default=[],
                        help="That game's build root, in --game order (default <game>/build)")
    parser.add_argument("--platform", choices=("macos", "ios-sim", "ios", "linux", "windows"), default="macos")
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_dir():
        parser.error("No app at %s" % app)
    desktop = args.platform in DESKTOP
    flat = args.platform not in ("macos",) + DESKTOP
    if desktop:
        sdl = app / ("SDL3.dll" if args.platform == "windows" else "libSDL3.so.0")
        frameworks, games_dir = app / "modules", app / "resources/games"
    else:
        frameworks = app / ("Frameworks" if flat else "Contents/Frameworks")
        games_dir = app / ("games" if flat else "Contents/Resources/games")
        sdl = next(frameworks.glob("libSDL3*.dylib"), None)
    if not sdl or not sdl.is_file():
        parser.error("%s has no shared SDL: build it with tools/build.py --game-modules" % app)
    frameworks.mkdir(parents=True, exist_ok=True)
    dylibs = []
    added = []  # Linux and Windows: what the package's archive needs too
    for i, game_dir in enumerate(args.game):
        game_dir = game_dir.resolve()
        game_id, app_name = load_game(game_dir)
        build_root = (args.build_root[i] if i < len(args.build_root) else game_dir / "build").resolve()
        module = module_path(build_root, game_id, args.platform)
        dest = frameworks / module.name
        shutil.copy2(module, dest)
        if args.platform == "windows":
            strip_debug(dest)  # as tools/package_desktop.py strips the app
        if not desktop:
            subprocess.run(["install_name_tool", "-id", "@rpath/" + dest.name, str(dest)], check=True)
        dylibs.append(dest)
        source_app = built_app(build_root, app_name, args.platform)
        added.append(dest)
        if source_app.is_dir():
            copy_resources(source_app, games_dir / app_name, args.platform)
            added.append(games_dir / app_name)
        else:
            print("warning: no %s; %s gets no resources of its own" % (source_app, game_id), file=sys.stderr)
        print("added %s (%s) from %s" % (game_id, app_name, build_root))
    if args.platform == "linux":
        add_to_tarballs(app, added)
    elif args.platform == "windows":
        add_to_zips(app, added)
    else:
        sign(app, args.platform, dylibs)


if __name__ == "__main__":
    main()
