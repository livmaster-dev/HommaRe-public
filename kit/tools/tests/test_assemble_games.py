"""tools/assemble_games.py: the other games' modules reach the Linux package's
tarball, not only its folder, and an Android Gradle project beside libmain.so."""

import importlib.util
from pathlib import Path
import shutil
import sys
import tarfile

import pytest

KIT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("assemble_games", KIT / "tools/assemble_games.py")
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


def stub_game(tmp_path):
    """The kit's stub game as another game of the app, built for both ABIs."""
    game = tmp_path / "other"
    shutil.copytree(KIT / "games/stub", game)
    (game / "layouts").mkdir()
    (game / "layouts/pad.json").write_text("{}\n")
    for abi in ("x86_64", "arm64-v8a"):
        module = game / "build/android-native" / abi / "modules/libgame.stub.so"
        module.parent.mkdir(parents=True)
        module.write_text("module " + abi)
    (game / "build/recomp").mkdir(parents=True)
    (game / "build/recomp/symbols.json").write_text("{}")
    return game


def staged_project(tmp_path, abis):
    project = tmp_path / "android"
    for abi in abis:
        jni = project / "app/src/main/jniLibs" / abi
        jni.mkdir(parents=True)
        (jni / "libmain.so").write_text("main")
        (jni / "libSDL3.so").write_text("sdl")
    return project


def test_android_modules_go_beside_libmain_and_resources_under_modules(tmp_path, monkeypatch):
    game = stub_game(tmp_path)
    project = staged_project(tmp_path, ("x86_64", "arm64-v8a"))
    monkeypatch.delenv("ANDROID_NDK_HOME", raising=False)
    monkeypatch.setattr(sys, "argv", ["assemble_games.py", "--app", str(project), "--platform", "android",
                                      "--game", str(game)])
    assemble_games.main()
    for abi in ("x86_64", "arm64-v8a"):
        assert (project / "app/src/main/jniLibs" / abi / "libgame.stub.so").read_text() == "module " + abi
    resources = project / "app/src/main/assets/resources"
    # Never games/: Android's shared storage ignores case, and the launcher's
    # Games folder (the player's installs) sits beside the unpacked resources.
    assert not (resources / "games").exists()
    module = resources / "modules/StubRecomp"
    assert (module / "symbols.json").is_file()
    assert (module / "controls/pad.json").is_file()
    assert not (module / "launcher-games.json").exists()


def test_android_needs_a_project_with_the_shared_sdl(tmp_path, monkeypatch):
    game = stub_game(tmp_path)
    project = staged_project(tmp_path, ("x86_64",))
    (project / "app/src/main/jniLibs/x86_64/libSDL3.so").unlink()
    monkeypatch.setattr(sys, "argv", ["assemble_games.py", "--app", str(project), "--platform", "android",
                                      "--game", str(game)])
    with pytest.raises(SystemExit):
        assemble_games.main()
