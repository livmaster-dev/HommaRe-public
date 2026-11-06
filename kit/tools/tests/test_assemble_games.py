"""The other games' modules reach the Linux package's tarball and the Windows
package's zip, not only their folders."""

import importlib.util
from pathlib import Path
import sys
import tarfile
import zipfile

import pytest

spec = importlib.util.spec_from_file_location("assemble_games", Path(__file__).parents[1] / "assemble_games.py")
assemble_games = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assemble_games)


def make_package(tmp_path):
    """A package folder and the tarball tools/package_desktop.py packs before assembly."""
    app = tmp_path / "out/StubRecomp"
    (app / "resources").mkdir(parents=True)
    (app / "StubRecomp").write_bytes(b"\x7fELF")
    (app / "resources/symbols.json").write_text("{}")
    archive = tmp_path / "out/StubRecomp-linux-x86_64.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(app, arcname="StubRecomp", recursive=False)
        tar.add(app / "StubRecomp", arcname="StubRecomp/StubRecomp")
        tar.add(app / "resources", arcname="StubRecomp/resources", recursive=False)
        tar.add(app / "resources/symbols.json", arcname="StubRecomp/resources/symbols.json")
    return app, archive


def add_game(app, game_id, app_name, payload):
    module = app / "modules" / ("libgame.%s.so" % game_id)
    module.parent.mkdir(exist_ok=True)
    module.write_bytes(payload)
    resources = app / "resources/games" / app_name
    resources.mkdir(parents=True, exist_ok=True)
    (resources / "symbols.json").write_text(game_id)
    return [module, resources]


def names(archive):
    with tarfile.open(archive) as tar:
        return sorted(m.name for m in tar.getmembers())


def test_tarball_gets_the_modules_and_their_resources(tmp_path):
    app, archive = make_package(tmp_path)
    archive.chmod(0o644)
    added = add_game(app, "hof", "StubHoF", b"hof") + add_game(app, "toe", "StubToE", b"toe")
    (app / "player-notes.txt").write_text("not part of the package")
    assemble_games.add_to_tarballs(app, added)
    got = names(archive)
    for name in ("StubRecomp/StubRecomp", "StubRecomp/resources/symbols.json",
                 "StubRecomp/modules/libgame.hof.so", "StubRecomp/modules/libgame.toe.so",
                 "StubRecomp/resources/games/StubHoF/symbols.json",
                 "StubRecomp/resources/games/StubToE/symbols.json"):
        assert name in got
    assert "StubRecomp/player-notes.txt" not in got
    assert archive.stat().st_mode & 0o777 == 0o644
    assert not list(archive.parent.glob("*.tar.gz.*")) and len(list(archive.parent.glob("tmp*"))) == 0


def test_assembling_again_replaces_rather_than_duplicates(tmp_path):
    app, archive = make_package(tmp_path)
    assemble_games.add_to_tarballs(app, add_game(app, "hof", "StubHoF", b"old"))
    assemble_games.add_to_tarballs(app, add_game(app, "hof", "StubHoF", b"new"))
    got = names(archive)
    assert len(got) == len(set(got))
    with tarfile.open(archive) as tar:
        assert tar.extractfile("StubRecomp/modules/libgame.hof.so").read() == b"new"


def test_no_tarball_is_fine(tmp_path):
    app = tmp_path / "StubRecomp"
    app.mkdir()
    assemble_games.add_to_tarballs(app, add_game(app, "hof", "StubHoF", b"hof"))
    assert not list(tmp_path.glob("*.tar.gz"))


def make_windows_package(tmp_path):
    """The Windows package folder and zip tools/package_desktop.py makes for the base game."""
    app = tmp_path / "build/windows/package/StubRecomp"
    (app / "resources").mkdir(parents=True)
    (app / "StubRecomp.exe").write_bytes(b"MZ app")
    (app / "SDL3.dll").write_bytes(b"MZ sdl")
    (app / "resources/symbols.json").write_text("{}")
    archive = app.parent / "StubRecomp-windows-x86_64.zip"
    with zipfile.ZipFile(archive, "w") as z:
        for path in (app / "StubRecomp.exe", app / "SDL3.dll", app / "resources/symbols.json"):
            z.write(path, "StubRecomp/" + path.relative_to(app).as_posix())
    return app, archive


def make_windows_game(tmp_path, key, game_id, app_name):
    """An expansion built with --preset windows-cross --game-modules: its DLL and its own package."""
    game = tmp_path / "games" / key
    game.mkdir(parents=True)
    (game / "game.toml").write_text('[game]\nid = "%s"\napp_name = "%s"\n' % (game_id, app_name))
    module = game / "build/windows/modules" / ("libgame.%s.dll" % game_id)
    module.parent.mkdir(parents=True)
    module.write_bytes(b"MZ " + game_id.encode())
    own = game / "build/windows/package" / app_name
    (own / "resources/ui-mods").mkdir(parents=True)
    (own / (app_name + ".exe")).write_bytes(b"MZ its own app")
    (own / "resources/symbols.json").write_text(game_id)
    (own / "resources/ui-mods/Widescreen.recipe").write_text("recipe")
    return game


def test_windows_paths():
    root = Path("/b")
    assert assemble_games.built_app(root, "App", "windows") == root / "windows/package/App"


def test_windows_modules_and_resources_reach_the_folder_and_the_zip(tmp_path, monkeypatch):
    app, archive = make_windows_package(tmp_path)
    hof = make_windows_game(tmp_path, "hof", "second", "StubHoF")
    toe = make_windows_game(tmp_path, "toe", "third", "StubToE")
    (app / "player-notes.txt").write_text("not part of the package")
    monkeypatch.setattr(sys, "argv", ["assemble_games.py", "--app", str(app), "--platform", "windows",
                                      "--game", str(hof), "--game", str(toe)])
    assemble_games.main()
    assert (app / "modules/libgame.second.dll").read_bytes() == b"MZ second"
    assert (app / "resources/games/StubToE/symbols.json").read_text() == "third"
    assert not (app / "resources/games/StubHoF/StubHoF.exe").exists()
    with zipfile.ZipFile(archive) as z:
        got = z.namelist()
        assert z.read("StubRecomp/modules/libgame.third.dll") == b"MZ third"
    for name in ("StubRecomp/StubRecomp.exe", "StubRecomp/SDL3.dll",
                 "StubRecomp/modules/libgame.second.dll",
                 "StubRecomp/resources/games/StubHoF/symbols.json",
                 "StubRecomp/resources/games/StubHoF/ui-mods/Widescreen.recipe"):
        assert name in got
    assert "StubRecomp/player-notes.txt" not in got
    # Again: replaced, not duplicated.
    assemble_games.main()
    with zipfile.ZipFile(archive) as z:
        got = z.namelist()
    assert len(got) == len(set(got))
    assert not list(archive.parent.glob("tmp*"))


def test_windows_app_without_shared_sdl_is_refused(tmp_path, monkeypatch):
    app, _ = make_windows_package(tmp_path)
    (app / "SDL3.dll").unlink()
    monkeypatch.setattr(sys, "argv", ["assemble_games.py", "--app", str(app), "--platform", "windows"])
    with pytest.raises(SystemExit):
        assemble_games.main()
