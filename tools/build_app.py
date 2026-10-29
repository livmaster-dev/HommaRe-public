#!/usr/bin/env python3
"""Build the app the player installs the games into: HommaRe with the three
games' modules, no game file and no mod made from one, and check it.

    tools/build_app.py --target app          # build/HommaRe.app (macOS), build/package/HommaRe (Linux)
    tools/build_app.py --target ios-sim      # build/ios-sim/.../HommaRe.app
    RECOMP_IOS_TEAM=<team> tools/build_app.py --target ios
    tools/build_app.py --target ios --unsigned   # build/HommaRe-iOS.ipa
    tools/build_app.py --target web          # build/web-site (needs the Emscripten SDK)

Each game (the base game, games/hof, games/toe) is built with
kit/tools/build.py --game-modules --no-game-files --no-install --release
(a player's build: no performance overlay; --overlay keeps it), the
expansions' modules go into the base game's app (kit/tools/assemble_games.py)
and tools/check_bundle.py checks the result (--game compares it with your
installs too). The launcher in it installs the games from the GOG offline
installers (innoextract: a helper on the Mac, a library on iOS, the system's
on Linux) and the interface mods are made from the recipes when a game
starts. Install the iOS
app with `xcrun devicectl device install app --device <id> <app>` or
`xcrun simctl install <udid> <app>`.

--target web builds each game for the browser on its own (a browser cannot
load a game module, so each is a whole app: <game>/build/web/recomp) and puts
them in one site beside the launcher page (kit/tools/web_launcher.py), each
in its folder under the game's id. The player imports their own install into
the browser's storage on that page; the site carries no game file. --serve
[PORT] then serves it cross-origin isolated (COOP/COEP) at localhost:PORT;
with --skip-build it serves what is built.

--unsigned builds the iOS app without a team and packs it for sideloading:
everything inside is signed ad hoc, the app with ios.entitlements (the memory
the game needs), and no provisioning profile names a developer. Sideloadly,
AltStore or SideStore sign it again with the player's Apple ID.
"""

import argparse
import platform as host_platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMES = [ROOT, ROOT / "games/hof", ROOT / "games/toe"]
sys.path.insert(0, str(ROOT / "kit/tools"))
from assemble_games import built_app, load_game  # noqa: E402
import game_config  # noqa: E402
import web_launcher  # noqa: E402


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


def web_site(out):
    """The launcher page with every game's web build in <out>/<game id>/."""
    shutil.rmtree(out, ignore_errors=True)
    web_launcher.build(GAMES, out)
    for game in GAMES:
        game_id = game_config.load(game)["game"]["id"]
        web_launcher.copy_web_build(game_id, game / "build/web/recomp", out)
    print("web site in %s" % out)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", choices=("app", "ios-sim", "ios", "web"), default="app")
    p.add_argument("--jobs", type=int, help="passed to kit/tools/build.py")
    p.add_argument("--game", type=Path, action="append", default=[],
                   help="an install to compare the app with (tools/check_bundle.py --game)")
    p.add_argument("--overlay", action="store_true",
                   help="keep the kit's performance overlay (F11): build without --release")
    p.add_argument("--skip-build", action="store_true", help="assemble and check what is built already")
    p.add_argument("--serve", type=int, nargs="?", const=8000, metavar="PORT",
                   help="web: serve the site at localhost:PORT (default 8000) once it is checked")
    p.add_argument("--unsigned", action="store_true",
                   help="iOS: build without a team and pack build/<app>-iOS.ipa for sideloading")
    args = p.parse_args()
    if args.unsigned and args.target != "ios":
        p.error("--unsigned is for --target ios")
    if args.serve is not None and args.target != "web":
        p.error("--serve is for --target web")
    desktop = "linux" if host_platform.system() == "Linux" else "macos"
    platform = {"app": desktop}.get(args.target, args.target)

    if not args.skip_build:
        for game in GAMES:
            cmd = [sys.executable, str(ROOT / "kit/tools/build.py"), "--game-dir", str(game), "--target", args.target]
            if args.target != "web":
                cmd += ["--game-modules", "--no-game-files", "--no-install"]
            if args.unsigned:
                cmd.append("--unsigned")
            if not args.overlay:
                cmd.append("--release")
            if args.jobs:
                cmd += ["--jobs", str(args.jobs)]
            print("+ " + " ".join(cmd), flush=True)
            subprocess.run(cmd, cwd=ROOT, check=True)
    if args.target == "web":
        site = web_site(ROOT / "build/web-site")
        check = [sys.executable, str(ROOT / "tools/check_bundle.py"), str(site)]
        for g in args.game:
            check += ["--game", str(g.expanduser())]
        print("+ " + " ".join(check), flush=True)
        code = subprocess.run(check, cwd=ROOT).returncode
        if code == 0 and args.serve is not None:
            web_launcher.serve(site, args.serve)
        return code
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
