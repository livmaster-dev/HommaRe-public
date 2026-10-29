#!/usr/bin/env python3
"""Build the app the player installs the games into: HommaRe with the three
games' modules, no game file and no mod made from one, and check it.

    tools/build_app.py --target app          # build/HommaRe.app (macOS), build/package/HommaRe (Linux)
    tools/build_app.py --target ios-sim      # build/ios-sim/.../HommaRe.app
    RECOMP_IOS_TEAM=<team> tools/build_app.py --target ios
    tools/build_app.py --target ios --unsigned   # build/HommaRe-iOS.ipa
    tools/build_app.py --target android      # build/android/app/build/outputs/apk/debug/*.apk

Each game (the base game, games/hof, games/toe) is built with
kit/tools/build.py --game-modules --no-game-files --no-install --release
(a player's build: no performance overlay; --overlay keeps it), the
expansions' modules go into the base game's app (kit/tools/assemble_games.py)
and tools/check_bundle.py checks the result (--game compares it with your
installs too). The launcher in it installs the games from the GOG offline
installers (innoextract: a helper on the Mac, a library on iOS, the system's
on Linux, a program in the APK on Android) and the interface mods are made
from the recipes when a game starts. Install the iOS
app with `xcrun devicectl device install app --device <id> <app>` or
`xcrun simctl install <udid> <app>`.

--unsigned builds the iOS app without a team and packs it for sideloading:
everything inside is signed ad hoc, the app with ios.entitlements (the memory
the game needs), and no provisioning profile names a developer. Sideloadly,
AltStore or SideStore sign it again with the player's Apple ID.

--target android needs ANDROID_HOME (the SDK), ANDROID_NDK_HOME (NDK r29 or
later: native/gpu_skin.cpp uses #embed) and JAVA_HOME (JDK 17 or later). The
expansions are built as modules only (--module-only), the base game's Gradle
project is staged (--stage-only), the modules go in beside libmain.so for
each ABI (--abi, repeatable: x86_64 for Intel Chromebooks and the emulator,
arm64-v8a for ARM Chromebooks, phones and tablets; both by default) and
Gradle assembles a debug-signed APK per ABI and a universal one, each
checked. Install one with `adb install -r <apk>`.
"""

import argparse
import importlib.util
import os
import platform as host_platform
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMES = [ROOT, ROOT / "games/hof", ROOT / "games/toe"]
sys.path.insert(0, str(ROOT / "kit/tools"))
from assemble_games import built_app, load_game  # noqa: E402


def sign_ad_hoc(app, entitlements):
    """Sign the app ad hoc from the inside out; its entitlements ride in the signature."""
    (app / "embedded.mobileprovision").unlink(missing_ok=True)
    frameworks = app / "Frameworks"
    nested = sorted(p for p in frameworks.iterdir() if p.suffix in (".dylib", ".framework")) \
        if frameworks.is_dir() else []
    for path in nested:
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(path)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", "--generate-entitlement-der",
                    "--entitlements", str(entitlements), str(app)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


def pack_ipa(app, ipa):
    """Payload/<app> zipped: the IPA a sideloading tool takes."""
    with tempfile.TemporaryDirectory() as staging:
        payload = Path(staging) / "Payload"
        payload.mkdir()
        subprocess.run(["ditto", str(app), str(payload / app.name)], check=True)
        ipa.unlink(missing_ok=True)
        subprocess.run(["ditto", "-c", "-k", "--norsrc", "--keepParent", str(payload), str(ipa)], check=True)
    print("packed %s" % ipa)


def build_android(args):
    """The expansions' modules, the base game's staged Gradle project, the
    modules put in it, Gradle's APKs, each one checked."""
    for name in ("ANDROID_HOME", "ANDROID_NDK_HOME", "JAVA_HOME"):
        if not os.environ.get(name):
            sys.exit("--target android needs %s (see --help)" % name)
    abis = []
    for abi in args.abi or ():
        abis += ["--abi", abi]
    if not args.skip_build:
        # The expansions first: their modules are what the staged project gets.
        for game, mode in [(g, "--module-only") for g in GAMES[1:]] + [(ROOT, "--stage-only")]:
            cmd = [sys.executable, str(ROOT / "kit/tools/build.py"), "--game-dir", str(game), "--target", "android",
                   "--game-modules", mode] + abis
            if not args.overlay:
                cmd.append("--release")
            if args.jobs:
                cmd += ["--jobs", str(args.jobs)]
            print("+ " + " ".join(cmd), flush=True)
            subprocess.run(cmd, cwd=ROOT, check=True)
    project = built_app(ROOT / "build", load_game(ROOT)[1], "android")
    if not (project / "app/src/main/jniLibs").is_dir():
        sys.exit("No staged Gradle project in %s after the build" % project)
    cmd = [sys.executable, str(ROOT / "kit/tools/assemble_games.py"), "--app", str(project), "--platform", "android"]
    for game in GAMES[1:]:
        cmd += ["--game", str(game)]
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)
    spec = importlib.util.spec_from_file_location("kit_build", ROOT / "kit/tools/build.py")
    kit_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kit_build)
    universal = kit_build.android_gradle(project)
    code = 0
    for apk in sorted(universal.parent.glob("*.apk")):
        check = [sys.executable, str(ROOT / "tools/check_bundle.py"), str(apk)]
        for g in args.game:
            check += ["--game", str(g.expanduser())]
        print("+ " + " ".join(check), flush=True)
        code = code or subprocess.run(check, cwd=ROOT).returncode
    return code


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", choices=("app", "ios-sim", "ios", "android"), default="app")
    p.add_argument("--jobs", type=int, help="passed to kit/tools/build.py")
    p.add_argument("--game", type=Path, action="append", default=[],
                   help="an install to compare the app with (tools/check_bundle.py --game)")
    p.add_argument("--overlay", action="store_true",
                   help="keep the kit's performance overlay (F11): build without --release")
    p.add_argument("--skip-build", action="store_true", help="assemble and check what is built already")
    p.add_argument("--unsigned", action="store_true",
                   help="iOS: build without a team and pack build/<app>-iOS.ipa for sideloading")
    p.add_argument("--abi", action="append", choices=("x86_64", "arm64-v8a"),
                   help="Android: an ABI the APK carries, repeatable (default both)")
    args = p.parse_args()
    if args.unsigned and args.target != "ios":
        p.error("--unsigned is for --target ios")
    if args.abi and args.target != "android":
        p.error("--abi is for --target android")
    desktop = "linux" if host_platform.system() == "Linux" else "macos"
    platform = {"app": desktop}.get(args.target, args.target)
    if args.target == "android":
        return build_android(args)

    if not args.skip_build:
        for game in GAMES:
            cmd = [sys.executable, str(ROOT / "kit/tools/build.py"), "--game-dir", str(game), "--target", args.target,
                   "--game-modules", "--no-game-files", "--no-install"]
            if args.unsigned:
                cmd.append("--unsigned")
            if not args.overlay:
                cmd.append("--release")
            if args.jobs:
                cmd += ["--jobs", str(args.jobs)]
            print("+ " + " ".join(cmd), flush=True)
            subprocess.run(cmd, cwd=ROOT, check=True)
    _, app_name = load_game(ROOT)
    app = built_app(ROOT / "build", app_name, platform)
    if not app.is_dir():
        sys.exit("No %s after the build" % app)
    cmd = [sys.executable, str(ROOT / "kit/tools/assemble_games.py"), "--app", str(app), "--platform", platform]
    for game in GAMES[1:]:
        cmd += ["--game", str(game)]
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)
    if args.unsigned:
        sign_ad_hoc(app, ROOT / "ios.entitlements")
    check = [sys.executable, str(ROOT / "tools/check_bundle.py"), str(app)]
    for g in args.game:
        check += ["--game", str(g.expanduser())]
    print("+ " + " ".join(check), flush=True)
    code = subprocess.run(check, cwd=ROOT).returncode
    if args.unsigned and code == 0:
        pack_ipa(app, ROOT / "build" / (app_name + "-iOS.ipa"))
    return code


if __name__ == "__main__":
    sys.exit(main())
