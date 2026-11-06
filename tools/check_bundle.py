#!/usr/bin/env python3
"""Check that an app carries nothing of the game: no game file, no mod made from one.

    tools/check_bundle.py build/HommaRe.app [--game ~/Heroes\\ V/game --game ~/Heroes\\ V/game-toe]
    tools/check_bundle.py build/windows/package/HommaRe

The app is to be distributable: the player's own GOG copy brings the game,
the launcher installs it, and the interface mods are made on the device from
recipes (ui-mods/*.recipe, tools/ui_recipe.py). This walks the bundle (a
macOS .app or a flat iOS one, the games' modules' resources under games/ too,
and the entries of any zip inside it) and fails on:

- the game's own kinds of files (archives .pak/.h5u/.h5m/.h5c/.h5s, the UI's
  .xdb, textures .dds/.tga, videos .bik/.wmv, the executables and DLLs, ...),
  a game/ or profile-seed/ folder, any zip (a mod is one); the Windows app's
  own executable and DLLs (own_binary) excepted;
- a picture that is not one of the repository's own files or the icon made
  from its icon.png (the launcher shows the game's textures from the install
  on the device, never from the app);
- a recipe line that is not interface layout (ui_recipe.check_lines), so no
  text of the game rides in one;
- with --game (repeatable): any file, or zip entry, whose bytes are those of
  a file of that install or of an entry of its paks (matched by size and
  CRC-32 from the paks' directories, then compared byte for byte).

What it lists without failing: the repository's own artwork the app shows
(icon, launcher splash), for the reader to judge.
"""

import argparse
import hashlib
import io
import re
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ui_mod_parts  # noqa: E402
import ui_recipe  # noqa: E402

# The game's own kinds of files.
GAME_SUFFIXES = {".pak", ".h5u", ".h5m", ".h5c", ".h5s", ".xdb", ".dds", ".tga", ".bik", ".wmv", ".exe",
                 ".dll", ".bin", ".mp3", ".ogg", ".wav", ".gr2", ".bmp", ".ico", ".cfg", ".lnk"}
# Pictures the app may carry: the repository's own files are allowed by content,
# these generated ones (the icon made from icon.png) by name.
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".tif", ".tiff", ".webp", ".heic", ".icns", ".car"}
GENERATED_IMAGES = ("AppIcon", "Icon-", "Assets.car")
ART_NOTES = {"launcher-splash.jpg", "AppIcon.icns"}
# Smaller files are compared with the install by kind only: a few bytes are
# the same in any two folders of files (an empty line, "1").
MIN_MATCH = 64


def own_binary(rel, app_name):
    """A Windows app's own executable or DLL: <app>.exe, SDL3.dll and FFmpeg's
    beside it, the innoextract.exe its launcher runs, and the games' modules
    (modules/libgame.<id>.dll). Anything else of those kinds is the game's."""
    if "!" in rel:
        return False
    if "/" not in rel:
        return rel in (app_name + ".exe", "SDL3.dll", "innoextract.exe") or \
            re.fullmatch(r"av(codec|format|util)-\d+\.dll", rel) is not None
    return re.fullmatch(r"modules/libgame\.[A-Za-z0-9_.-]+\.dll", rel) is not None


def walk(app):
    """(relative path, bytes) of every file in the bundle; zip entries as path!entry."""
    for path in sorted(p for p in app.rglob("*") if p.is_file() and not p.is_symlink()):
        rel = path.relative_to(app).as_posix()
        data = path.read_bytes()
        yield rel, data
        if data[:4] == b"PK\x03\x04":
            try:
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    for info in z.infolist():
                        if not info.is_dir():
                            yield rel + "!" + info.filename, z.read(info)
            except zipfile.BadZipFile:
                pass


def repo_hashes():
    """SHA-256 of every file the repository tracks (its own artwork among them)."""
    names = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    out = set()
    for name in names.split(b"\0"):
        path = ROOT / name.decode()
        if name and path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES | {".jpg"}:
            out.add(hashlib.sha256(path.read_bytes()).hexdigest())
    return out


class Install:
    """A game install's files and its paks' entries, by (size, CRC-32)."""

    def __init__(self, root):
        self.root = root
        self.loose = {}   # (size, crc) -> path
        self.entries = {}  # (size, crc) -> (pak, entry name)
        for path in root.rglob("*"):
            if not path.is_file() or path.is_symlink():
                continue
            size = path.stat().st_size
            if path.suffix.lower() in (".pak", ".h5u", ".zip"):
                try:
                    with zipfile.ZipFile(path) as z:
                        for info in z.infolist():
                            if not info.is_dir() and info.file_size:
                                self.entries.setdefault((info.file_size, info.CRC), (path, info.filename))
                except zipfile.BadZipFile:
                    pass
            if size <= 64 << 20:
                self.loose.setdefault(size, []).append(path)

    def match(self, data):
        """Where these bytes are in the install, or None."""
        if not data:
            return None
        crc = zlib.crc32(data)
        hit = self.entries.get((len(data), crc))
        if hit:
            with zipfile.ZipFile(hit[0]) as z:
                if z.read(hit[1]) == data:
                    return "%s!%s" % (hit[0].relative_to(self.root), hit[1])
        for path in self.loose.get(len(data), []):
            if path.read_bytes() == data:
                return str(path.relative_to(self.root))
        return None


def types_lines():
    """The lines widescreen_mod.py's recipe may put in types.xml besides
    layout: ui_mod_parts' declarations of HorAlign/VerAlign and the
    <NestedTypes> naming them (at WindowScreen's indent, LF or CRLF)."""
    parts = ui_mod_parts.types_fields() + ui_mod_parts.types_enum()
    out = set()
    for nl in (b"\n", b"\r\n"):
        out |= set(ui_recipe.split_lines(parts.replace(b"\n", nl)))
        for indent in (b"\t" * n for n in range(8)):
            out |= {indent + b"<NestedTypes>" + nl, indent + b"</NestedTypes>" + nl,
                    indent + b"\t<Item>" + ui_mod_parts.ALIGN_TYPE + b"</Item>" + nl}
    return out


TYPES_LINES = types_lines()


def check_recipe(rel, data):
    """The lines a recipe's edits put in that are not interface layout. The
    files it writes whole (the port's own curtains, a note) are not checked."""
    added, edit = [], False
    for raw in data.split(b"\n"):
        if raw and raw[:1] in b"+*=":
            if edit:
                added.append(ui_recipe._unline(raw))
        elif raw[:1] != b"#" and raw:
            edit = raw.startswith(b"@\t")
    return ui_recipe.check_lines(rel, added, TYPES_LINES)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("app", type=Path, help="the .app")
    p.add_argument("--game", type=Path, action="append", default=[],
                   help="a game install to compare every file with (repeatable)")
    args = p.parse_args()
    app = args.app.resolve()
    if not app.is_dir():
        sys.exit("%s: no such app" % app)

    installs = []
    for g in args.game:
        print("reading %s ..." % g, flush=True)
        installs.append(Install(g.expanduser().resolve()))
    repo_images = repo_hashes()

    problems, notes = [], []
    files = recipes = 0
    for rel, data in walk(app):
        files += 1
        parts = rel.split("!")[0].split("/")
        name = rel.rsplit("/", 1)[-1].split("!")[-1]
        suffix = Path(name).suffix.lower()
        if "game" in parts[:-1]:
            problems.append("%s: in a game/ folder" % rel)
        if "profile-seed" in parts:
            problems.append("%s: in profile-seed/" % rel)
        if suffix in GAME_SUFFIXES and not name == "innoextract" and not own_binary(rel, app.name):
            problems.append("%s: a %s file, the game's kind" % (rel, suffix))
        if "!" not in rel and data[:4] == b"PK\x03\x04":
            problems.append("%s: a zip (a mod or a pak is one)" % rel)
        if suffix in IMAGE_SUFFIXES and "!" not in rel:
            if name in ART_NOTES or name.startswith(GENERATED_IMAGES):
                if name in ART_NOTES:
                    notes.append("%s: the repository's artwork (%d bytes)" % (rel, len(data)))
            elif hashlib.sha256(data).hexdigest() not in repo_images:
                problems.append("%s: a picture that is not one of the repository's" % rel)
        if suffix == ".recipe":
            recipes += 1
            problems += check_recipe(rel, data)
        for inst in installs if len(data) >= MIN_MATCH else ():
            where = inst.match(data)
            if where:
                problems.append("%s: the same bytes as %s in %s" % (rel, where, inst.root))
    print("%s: %d files and zip entries, %d recipes%s" % (app, files, recipes,
                                                          ", compared with %d installs" % len(installs)
                                                          if installs else ""))
    for n in notes:
        print("  note: " + n)
    for line in problems:
        print("  PROBLEM: " + line)
    print("no game data found" if not problems else "%d problems" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
