#!/usr/bin/env python3
"""Write every interface mod's recipe, and check the recipes against the mods.

    tools/ui_recipes.py --game ~/Heroes\\ V/game --toe ~/Heroes\\ V/game-toe \\
        [--out build/ui-mods] [--check-only] [--reference build]

The app carries no game file and no mod made from one: it makes the interface
mods from the player's own install with the recipes in ui-mods/ of each game
(tools/ui_recipe.py, kit/host/ui_mods.cpp; game.toml [ios] / [macos] ui_mods
say which go into the app). This runs the four generators for the three games
on the developer's install (--game: the base game with Hammers of Fate,
--toe: Tribes of the East), the mods into --out, the recipes into the
repository, then applies each recipe to the same install as the app does
(ui_recipe.apply, the reference for ui_mods.cpp) and checks that it makes the
mod the generator made: the same files, byte for byte, and the same entry
dates. --check-only leaves the recipes as they are and checks them.
--reference compares the mods with those another build put in its build/
(widescreen/, phone/, touch/, mobile/): file contents only, as those zips
carry other dates.
"""

import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ui_recipe  # noqa: E402

# Hammers of Fate's loading screen paints its sides with its own backdrop.
HOF_BORDER = ("UI/PWLScreen.(WindowScreenShared).xdb="
              "/UI/pwlscreen/Back.(BackgroundSimpleScallingTexture).xdb#xpointer(/BackgroundSimpleScallingTexture)")


def mods(game, toe):
    """(recipe, mod file under --out, generator and its arguments) for each mod."""
    wide = ["widescreen_mod.py", "--toe-data", toe / "data", "--wide-dialogues"]
    out = []
    for gid, install, recipes, suffix in (("base", game, ROOT / "ui-mods", ""),
                                          ("hof", game, ROOT / "games/hof/ui-mods", "-hof"),
                                          ("toe", toe, ROOT / "games/toe/ui-mods", "-toe")):
        ext = ".pak" if gid == "hof" else ".h5u"
        sel = ["--game", gid, "--base-game", install]
        out += [
            (recipes / "Widescreen.recipe", "widescreen/Widescreen%s%s" % (suffix, ext),
             wide + sel + (["--border", HOF_BORDER] if gid == "hof" else [])),
            (recipes / "PhoneUI.recipe", "phone/PhoneUI%s%s" % (suffix, ext), ["phone_ui_mod.py"] + sel),
            (recipes / "TouchUI.recipe", "touch/TouchUI%s%s" % (suffix, ext), ["touch_ui_mod.py"] + sel),
            (recipes / "NativeTouchUI.recipe", "touch/NativeTouchUI%s%s" % (suffix, ext),
             ["native_touch_ui_mod.py"] + sel),
            (recipes / "MobileOptions.recipe", "mobile/MobileOptions%s%s" % (suffix, ext),
             ["mobile_options_mod.py"] + sel),
            (recipes / "SkillWheelUI.recipe", "skillwheel/SkillWheelUI%s%s" % (suffix, ext),
             ["skill_wheel_ui_mod.py"] + sel),
        ]
    return out


def zip_files(path):
    """{name: (bytes, date_time)} of a zip."""
    with zipfile.ZipFile(path) as z:
        return {i.filename: (z.read(i), i.date_time) for i in z.infolist() if not i.is_dir()}


def read_layers(root, layers, names):
    """{name: bytes}: the named files from the paks, a later one's over an earlier one's."""
    found = {}
    want = {n.lower(): n for n in names}
    for layer in layers:
        pak = root / layer
        if not pak.is_file():
            continue
        with zipfile.ZipFile(pak) as z:
            for info in z.infolist():
                name = want.get(info.filename.lower())
                if name:
                    found[name] = z.read(info)
    return found


def apply(recipe_path, root, made):
    """The mod the recipe makes from the install at root ({name: bytes}),
    the names it left out, and its zip date."""
    rec = ui_recipe.read(recipe_path)
    over = {}
    if rec["over"]:
        over = made.get(rec["over"], {})
    names = [f["source"] for f in rec["files"] if "source" in f and f["source"] not in over]
    names += [n for f in rec["files"] for n, _ in f.get("srcs", ()) if n not in over]
    files, left = ui_recipe.apply(rec, read_layers(root, rec["layers"], names), over)
    date = tuple(int(x) for x in rec["date"].replace("-", " ").replace(":", " ").split())
    made[rec["out"]] = files
    return files, left, date


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", type=Path, required=True,
                   help="the base game's install (with Hammers of Fate: bin/, bina1/, data/, dataa1/)")
    p.add_argument("--toe", type=Path, required=True, help="Tribes of the East's install")
    p.add_argument("--out", type=Path, default=ROOT / "build/ui-mods", help="where the generators' mods go")
    p.add_argument("--check-only", action="store_true", help="keep the recipes, only check them")
    p.add_argument("--reference", type=Path, help="a build/ whose mods to compare contents with")
    args = p.parse_args()

    problems = []
    made = {}
    for recipe, mod, command in mods(args.game.expanduser(), args.toe.expanduser()):
        target = args.out / mod
        cmd = [sys.executable, str(ROOT / "tools" / command[0])] + [str(a) for a in command[1:]] + \
            ["--out", str(target)]
        if not args.check_only:
            cmd += ["--recipe", str(recipe)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode not in (0, 1) or not target.is_file():
            sys.exit("%s failed:\n%s%s" % (" ".join(cmd), run.stdout, run.stderr))
        expected = zip_files(target)
        install = args.toe.expanduser() if "/toe/" in str(recipe) else args.game.expanduser()
        files, left, date = apply(recipe, install, made)
        rel = recipe.relative_to(ROOT)
        bad = []
        if left:
            bad.append("left out: " + ", ".join(left))
        for name in sorted(set(expected) | set(files)):
            if name not in files:
                bad.append("%s: not made" % name)
            elif name not in expected:
                bad.append("%s: made, not in the mod" % name)
            elif files[name] != expected[name][0]:
                bad.append("%s: differs" % name)
            elif expected[name][1] != date:
                bad.append("%s: dated %s, the recipe %s" % (name, expected[name][1], date))
        if args.reference:
            ref = args.reference / mod
            if not ref.is_file():
                bad.append("no %s" % ref)
            else:
                theirs = zip_files(ref)
                for name in sorted(set(theirs) | set(files)):
                    if theirs.get(name, (None,))[0] != files.get(name):
                        bad.append("%s: differs from %s" % (name, ref))
        print("%-38s %4d files  %s" % (rel, len(files), "ok" if not bad else "%d problems" % len(bad)))
        problems += ["%s: %s" % (rel, b) for b in bad]
    for line in problems[:60]:
        print("  " + line)
    if len(problems) > 60:
        print("  ... %d more" % (len(problems) - 60))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
