"""Desktop packages use synthetic binaries and never need a game installation."""

import importlib.util
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import zipfile
from unittest.mock import patch

import pytest

spec = importlib.util.spec_from_file_location("package_desktop", Path(__file__).parents[1] / "package_desktop.py")
package_desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_desktop)


def test_stage_layout(tmp_path):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    assert (out / "StubRecomp").is_file()
    assert "STUB.EXE" in (out / "README.txt").read_text()
    assert (out / "LICENSE").is_file()
    assert (tmp_path / "out" / "StubRecomp-linux-x86_64.tar.gz").is_file()


def test_stage_includes_core_manifests_for_builtin_plugins(tmp_path):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    core = tmp_path / "game/mods/core/display"
    core.mkdir(parents=True)
    (core / "mod.toml").write_text('id = "sample.display"')
    (core / "display.c").write_text("source")
    (core / "display.dylib").write_bytes(b"other platform")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux",
                                    game_dir=tmp_path / "game")
    manifest = out / "resources/mods/core/display/mod.toml"
    assert manifest.read_text() == 'id = "sample.display"'
    assert not (manifest.parent / "display.c").exists()
    assert not (manifest.parent / "display.dylib").exists()
    with tarfile.open(tmp_path / "out/StubRecomp-linux-x86_64.tar.gz") as tar:
        assert tar.extractfile("StubRecomp/resources/mods/core/display/mod.toml").read() == manifest.read_bytes()


def test_stage_carries_linux_ui_mods_seed_and_launcher(tmp_path):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    game = tmp_path / "game"
    (game / "ui-mods").mkdir(parents=True)
    for name in ("Widescreen", "PhoneUI"):
        (game / "ui-mods" / (name + ".recipe")).write_text(name)
    (game / "seed.cfg").write_text("seed")
    (game / "launcher-manifests").mkdir()
    (game / "launcher-manifests/base.json").write_text("{}")
    (game / "launcher-games.json").write_text('{"games": []}')
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"},
           "dir": str(game), "launcher_config_dir": game,
           "linux": {"ui_mods": ["Widescreen"], "profile_files": {"profiles/seed.cfg": "seed.cfg"}}}
    stale = tmp_path / "out/StubRecomp/resources/ui-mods/TouchUI.recipe"
    stale.parent.mkdir(parents=True)
    stale.write_text("an earlier build's")
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    resources = out / "resources"
    assert sorted(p.name for p in (resources / "ui-mods").iterdir()) == ["Widescreen.recipe"]
    assert (resources / "profile-seed/profiles/seed.cfg").read_text() == "seed"
    assert (resources / "launcher-games.json").is_file()
    assert (resources / "launcher-manifests/base.json").is_file()
    assert not (out / "libSDL3.so.0").exists()
    with tarfile.open(tmp_path / "out/StubRecomp-linux-x86_64.tar.gz") as tar:
        names = tar.getnames()
    assert "StubRecomp/resources/ui-mods/Widescreen.recipe" in names
    assert "StubRecomp/resources/launcher-manifests/base.json" in names
    assert "StubRecomp/resources/ui-mods/TouchUI.recipe" not in names


@pytest.mark.parametrize("machine,arch", [("x86_64", "x86_64"), ("AMD64", "x86_64"),
                                         ("aarch64", "aarch64"), ("arm64", "aarch64")])
def test_linux_archive_contents(tmp_path, monkeypatch, machine, arch):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    exe.chmod(0o755)
    (tmp_path / "symbols.json").write_text('{"functions": []}')
    (tmp_path / "STUB.EXE").write_bytes(b"private game")
    (tmp_path / "sound.sf2").write_bytes(b"private SoundFont")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    monkeypatch.setattr(package_desktop.platform, "machine", lambda: machine)
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    expected = {"StubRecomp", "LICENSE", "NOTICE", "README.txt", "resources",
                "resources/classic-modes.json", "resources/symbols.json",
                "resources/general-midi.sf2", "resources/general-midi-LICENSE.txt"}
    assert {p.relative_to(out).as_posix() for p in out.rglob("*")} == expected
    assert (out / "StubRecomp").read_bytes() == exe.read_bytes()
    assert stat.S_IMODE((out / "StubRecomp").stat().st_mode) == stat.S_IMODE(exe.stat().st_mode)
    for name in ("LICENSE", "NOTICE"):
        assert (out / name).read_bytes() == (package_desktop.ROOT / name).read_bytes()
    assert (out / "resources/classic-modes.json").read_bytes() == (
        package_desktop.ROOT / "tools/recomp/baseline/classic-modes.json").read_bytes()
    assert (out / "resources/symbols.json").read_bytes() == (tmp_path / "symbols.json").read_bytes()
    # The kit's own General MIDI bank ships; the game's private one does not.
    bank = package_desktop.ROOT / "third_party/soundfonts/generaluser-gs"
    assert (out / "resources/general-midi.sf2").read_bytes() == (bank / "GeneralUser-GS.sf2").read_bytes()
    assert (out / "resources/general-midi-LICENSE.txt").read_bytes() == (bank / "LICENSE").read_bytes()
    readme = (out / "README.txt").read_text()
    assert 'RECOMP_EXE="/path/to/your game/STUB.EXE" ./StubRecomp' in readme
    assert "parent directory as the game data root" in readme
    assert "RECOMP_DATA=" not in readme
    # Repackaging must preserve a player's files without adding them to the archive.
    (out / "player.sav").write_bytes(b"private save")
    package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux")
    assert (out / "player.sav").read_bytes() == b"private save"
    with tarfile.open(tmp_path / "out" / f"StubRecomp-linux-{arch}.tar.gz") as tar:
        assert set(tar.getnames()) == {"StubRecomp"} | {f"StubRecomp/{p}" for p in expected}
        assert tar.extractfile("StubRecomp/StubRecomp").read() == b"\x7fELF"
        assert tar.getmember("StubRecomp/StubRecomp").mode == stat.S_IMODE(exe.stat().st_mode)


def test_stage_bundles_the_games_control_layouts(tmp_path):
    """layouts/*.json (top level only) lands at resources/controls; other files don't."""
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    game_dir = tmp_path / "game"
    layouts = game_dir / "layouts"
    layouts.mkdir(parents=True)
    (layouts / "pad.json").write_text('{"version": 1, "name": "pad"}')
    (layouts / "pad.phone-portrait.json").write_text('{"version": 1, "name": "pad"}')
    (layouts / "readme.txt").write_text("not a layout")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux", game_dir=game_dir)
    controls = out / "resources/controls"
    assert (controls / "pad.json").read_text() == (layouts / "pad.json").read_text()
    assert (controls / "pad.phone-portrait.json").read_text() == (layouts / "pad.phone-portrait.json").read_text()
    assert not (controls / "readme.txt").exists()
    assert {p.name for p in controls.iterdir()} == {"pad.json", "pad.phone-portrait.json"}
    with tarfile.open(tmp_path / "out/StubRecomp-linux-x86_64.tar.gz") as tar:
        names = set(tar.getnames())
        assert "StubRecomp/resources/controls/pad.json" in names
        assert "StubRecomp/resources/controls/pad.phone-portrait.json" in names
        assert not any(n.endswith("readme.txt") for n in names)


def test_stage_bundles_the_native_launchers_config_on_linux(tmp_path):
    """launcher-games.json and launcher-manifests/*.json land in resources/ (Linux only)."""
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    game_dir = tmp_path / "game"
    (game_dir / "launcher-manifests").mkdir(parents=True)
    (game_dir / "launcher-games.json").write_text('{"games": []}')
    (game_dir / "launcher-manifests/gog-1-en.json").write_text('{"files": []}')
    (game_dir / "launcher-manifests/notes.txt").write_text("not a manifest")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"},
           "dir": str(game_dir), "launcher_config_dir": game_dir,
           "linux": {"ui_mods": [], "profile_files": {}}}
    with patch("platform.machine", return_value="x86_64"):
        out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux", game_dir=game_dir)
    assert (out / "resources/launcher-games.json").read_text() == '{"games": []}'
    assert {p.name for p in (out / "resources/launcher-manifests").iterdir()} == {"gog-1-en.json"}
    with tarfile.open(tmp_path / "out/StubRecomp-linux-x86_64.tar.gz") as tar:
        names = set(tar.getnames())
        assert "StubRecomp/resources/launcher-games.json" in names
        assert "StubRecomp/resources/launcher-manifests/gog-1-en.json" in names
    win = package_desktop.stage(tmp_path / "recomp_app", cfg, tmp_path / "win", system="Windows", game_dir=game_dir)
    assert not (win / "resources/launcher-games.json").exists()


def test_stage_without_layouts_creates_no_controls_directory(tmp_path):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"\x7fELF")
    game_dir = tmp_path / "game"
    game_dir.mkdir()
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Linux", game_dir=game_dir)
    assert not (out / "resources/controls").exists()


def test_windows_folder(tmp_path):
    exe = tmp_path / "recomp_app.exe"
    exe.write_bytes(b"MZ")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows")
    assert (out / "StubRecomp.exe").read_bytes() == b"MZ"
    assert (out / "NOTICE").is_file()
    assert (out / "LICENSE").is_file()
    assert (out / "resources/classic-modes.json").is_file()
    assert not (out / "resources/symbols.json").exists()
    assert not list((tmp_path / "out").glob("*.tar.gz"))
    readme = (out / "README.txt").read_text()
    assert '$env:RECOMP_EXE = "C:\\path\\to\\your game\\STUB.EXE"' in readme
    assert ".\\StubRecomp.exe" in readme
    assert "Double-click StubRecomp.exe" in readme


def test_windows_carries_innoextract_and_its_licenses(tmp_path):
    exe = tmp_path / "recomp" / "StubRecomp.exe"
    exe.parent.mkdir()
    exe.write_bytes(b"MZ app")
    (exe.parent / "innoextract.exe").write_bytes(b"MZ innoextract")
    build_dir = tmp_path / "cmake"
    licenses = build_dir / "innoextract-windows/files/license"
    licenses.mkdir(parents=True)
    (licenses / "innoextract.LICENSE.txt").write_text("zlib")
    (licenses / "Boost.LICENSE_1_0.txt").write_text("boost")
    (build_dir / "CMakeCache.txt").write_text("")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows", build_dir=build_dir)
    assert (out / "innoextract.exe").read_bytes() == b"MZ innoextract"
    assert (out / "resources/innoextract-NOTICE.md").read_bytes() == (
        package_desktop.ROOT / "third_party/innoextract/NOTICE.md").read_bytes()
    assert sorted(p.name for p in (out / "resources/innoextract-license").iterdir()) == [
        "Boost.LICENSE_1_0.txt", "innoextract.LICENSE.txt"]
    with zipfile.ZipFile(tmp_path / "out/StubRecomp-windows-x86_64.zip") as z:
        assert "StubRecomp/innoextract.exe" in z.namelist()
        assert "StubRecomp/resources/innoextract-license/innoextract.LICENSE.txt" in z.namelist()
    # A build without the native launcher: no helper, and none left over.
    (exe.parent / "innoextract.exe").unlink()
    package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows", build_dir=build_dir)
    assert not (out / "innoextract.exe").exists()
    assert not (out / "resources/innoextract-license").exists()


def test_windows_binaries_lose_their_dwarf(tmp_path, monkeypatch):
    exe = tmp_path / "recomp" / "StubRecomp.exe"
    exe.parent.mkdir()
    exe.write_bytes(b"MZ" + bytes(126))  # long enough to be taken for a PE
    ran = []
    monkeypatch.setattr(package_desktop, "windows_strip", lambda: "/llvm/bin/llvm-strip")
    monkeypatch.setattr(package_desktop.subprocess, "run", lambda cmd, check: ran.append(cmd))
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows")
    assert ran == [["/llvm/bin/llvm-strip", "--strip-debug", str(out / "StubRecomp.exe")]]
    # The build tree's own copy keeps it, for the RVAs a crash report gives.
    assert exe.read_bytes() == b"MZ" + bytes(126)


def test_windows_zip_carries_the_package_and_nothing_else(tmp_path):
    exe = tmp_path / "recomp" / "StubRecomp.exe"
    exe.parent.mkdir()
    exe.write_bytes(b"MZ app")
    (exe.parent / "SDL3.dll").write_bytes(b"MZ sdl")
    (exe.parent / "symbols.json").write_text("{}")
    game = tmp_path / "game"
    (game / "ui-mods").mkdir(parents=True)
    (game / "ui-mods/Widescreen.recipe").write_text("recipe")
    (game / "launcher-games.json").write_text('{"games": []}')
    build_dir = tmp_path / "cmake"
    build_dir.mkdir()
    (build_dir / "CMakeCache.txt").write_text("RECOMP_GAME_MODULES:BOOL=ON\nRECOMP_VIDEO:BOOL=OFF\n"
                                              "CMAKE_SYSTEM_PROCESSOR:STRING=x86_64\n")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"},
           "dir": str(game), "launcher_config_dir": game,
           "linux": {"ui_mods": [], "profile_files": {}},
           "windows": {"ui_mods": ["Widescreen"], "profile_files": {}}}
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows", build_dir=build_dir)
    (out / "player.sav").write_bytes(b"the player's")
    package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows", build_dir=build_dir)
    assert (out / "SDL3.dll").read_bytes() == b"MZ sdl"
    assert (out / "resources/ui-mods/Widescreen.recipe").read_text() == "recipe"
    archive = tmp_path / "out/StubRecomp-windows-x86_64.zip"
    with zipfile.ZipFile(archive) as z:
        names = set(z.namelist())
        assert z.read("StubRecomp/StubRecomp.exe") == b"MZ app"
    assert {"StubRecomp/StubRecomp.exe", "StubRecomp/SDL3.dll", "StubRecomp/README.txt",
            "StubRecomp/resources/symbols.json", "StubRecomp/resources/ui-mods/Widescreen.recipe",
            "StubRecomp/resources/launcher-games.json"} <= names
    assert "StubRecomp/player.sav" not in names
    assert not list((tmp_path / "out").glob("*.part"))
    # A single-game build has no shared SDL to ship.
    (build_dir / "CMakeCache.txt").write_text("RECOMP_GAME_MODULES:BOOL=OFF\n")
    package_desktop.stage(exe, cfg, tmp_path / "out", system="Windows", build_dir=build_dir)
    assert not (out / "SDL3.dll").exists()
    with zipfile.ZipFile(archive) as z:
        assert "StubRecomp/SDL3.dll" not in z.namelist()


@pytest.mark.parametrize("system", ["Linux", "Windows"])
def test_video_libraries_and_notice(tmp_path, monkeypatch, system):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"fake native binary")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    build_dir = tmp_path / "cmake" / "custom-preset"
    libdir = build_dir / "ffmpeg" / ("bin" if system == "Windows" else "lib")
    libdir.mkdir(parents=True)
    cache = build_dir / "CMakeCache.txt"
    cache.write_text("RECOMP_VIDEO:BOOL=ON\n")
    names = (["avformat-61.dll", "avcodec-61.dll", "avutil-59.dll"] if system == "Windows" else
             ["libavformat.so.61", "libavcodec.so.61", "libavutil.so.59"])
    for name in names:
        if system == "Linux":
            # Installed SONAME symlinks must not point outside the final package.
            real = libdir / (name + ".1.100")
            real.write_bytes(name.encode())
            (libdir / name).symlink_to(real.name)
        else:
            (libdir / name).write_bytes(name.encode())
    (libdir / "unrelated-private-file").write_bytes(b"private")
    monkeypatch.setattr(package_desktop.platform, "machine", lambda: "x86_64")
    out = package_desktop.stage(exe, cfg, tmp_path / "out", system=system, build_dir=build_dir)
    for name in names:
        assert (out / name).read_bytes() == name.encode()
        assert not (out / name).is_symlink()
    notice = "resources/ffmpeg-NOTICE.md"
    assert (out / notice).read_bytes() == (package_desktop.ROOT / "third_party/ffmpeg/NOTICE.md").read_bytes()
    assert not (out / "unrelated-private-file").exists()
    archive = tmp_path / "out/StubRecomp-linux-x86_64.tar.gz"
    if system == "Linux":
        with tarfile.open(archive) as tar:
            for name in names:
                assert tar.getmember(f"StubRecomp/{name}").isfile()
                assert tar.extractfile(f"StubRecomp/{name}").read() == name.encode()
            assert f"StubRecomp/{notice}" in tar.getnames()
            assert not any("unrelated-private-file" in name for name in tar.getnames())

    # An explicit OFF overrides leftover installed libraries in a reused tree.
    (out / "player.sav").write_bytes(b"keep this save")
    cache.write_text("RECOMP_VIDEO:BOOL=OFF\n")
    package_desktop.stage(exe, cfg, tmp_path / "out", system=system, build_dir=build_dir)
    assert all(not (out / name).exists() for name in names)
    assert not (out / notice).exists()
    assert (out / "player.sav").read_bytes() == b"keep this save"
    if system == "Linux":
        with tarfile.open(archive) as tar:
            assert all(f"StubRecomp/{name}" not in tar.getnames() for name in names + [notice, "player.sav"])


@pytest.mark.parametrize("system", ["Linux", "Windows"])
def test_video_enabled_requires_installed_libraries(tmp_path, system):
    exe = tmp_path / "recomp_app"
    exe.write_bytes(b"fake native binary")
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    build_dir = tmp_path / "cmake"
    build_dir.mkdir()
    (build_dir / "CMakeCache.txt").write_text("RECOMP_VIDEO:BOOL=ON\n")
    with pytest.raises(FileNotFoundError):
        package_desktop.stage(exe, cfg, tmp_path / "out", system=system, build_dir=build_dir)
    assert not list((tmp_path / "out").glob("*.tar.gz"))


@pytest.mark.parametrize("system,target,stub,packages", [
    ("Linux", "app", False, True), ("Windows", "app", False, True),
    ("Darwin", "app", False, False), ("Linux", "app", True, False),
    ("Windows", "app", True, False), ("Linux", "smoke", False, False),
    ("Windows", "headless", False, False),
])
def test_build_packages_only_successful_desktop_apps(tmp_path, monkeypatch, system, target, stub, packages):
    spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    (tmp_path / "game.toml").touch()
    cfg = {"game": {"app_name": "StubRecomp", "name": "Stub Game", "executable": "STUB.EXE"}}
    argv = ["build.py", "--game-dir", str(tmp_path), "--target", target] + (["--stub"] if stub else [])
    monkeypatch.setattr(build.sys, "argv", argv)
    monkeypatch.setattr(build.platform, "system", lambda: system)
    monkeypatch.setattr(build.game_config, "load", lambda path: cfg)
    monkeypatch.setattr(build, "texture_pack", lambda *args: None)
    monkeypatch.setattr(build, "configure", lambda *args, **kw: None)
    binary = tmp_path / "build/recomp" / ("StubRecomp.exe" if system == "Windows" else "StubRecomp")
    calls = []

    def fake_build(*args, **kwargs):
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"fake native binary")
        calls.append("built")

    def fake_stage(app_binary, config, out_dir, system=None, build_dir=None, game_dir=None):
        assert calls == ["built"]
        assert app_binary == binary and app_binary.is_file()
        assert config == cfg
        assert out_dir == tmp_path / "build/package"
        assert system in {"Linux", "Windows"}
        assert build_dir == tmp_path / "build/cmake" / system.lower()
        assert game_dir == tmp_path
        calls.append("packaged")
        return out_dir / "StubRecomp"

    monkeypatch.setattr(build, "build", fake_build)
    monkeypatch.setattr(build.package_desktop, "stage", fake_stage)
    build.main()
    assert calls == (["built", "packaged"] if packages else ["built"])

    def failed_build(*args, **kwargs):
        raise subprocess.CalledProcessError(7, "cmake")

    calls.clear()
    monkeypatch.setattr(build, "build", failed_build)
    with pytest.raises(SystemExit) as error:
        build.main()
    assert error.value.code == 7
    assert not calls


@pytest.mark.skipif(not shutil.which("cc") or package_desktop.platform.system() != "Linux",
                    reason="needs a Linux C compiler")
def test_package_runpath_keeps_only_origin(tmp_path):
    source = tmp_path / "main.c"
    source.write_text("int main(void) { return 0; }\n")
    binary = tmp_path / "app"
    subprocess.run(["cc", str(source), "-o", str(binary),
                    "-Wl,--enable-new-dtags,-rpath,$ORIGIN:/build/ffmpeg/lib:/build/_deps/sdl3-build"], check=True)
    package_desktop.package_runpath(binary)
    dynamic = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True, check=True).stdout
    assert "[$ORIGIN]" in dynamic and "/build/" not in dynamic
    assert subprocess.run([str(binary)]).returncode == 0
    package_desktop.package_runpath(binary)  # nothing left to change


@pytest.mark.skipif(not shutil.which("cc") or package_desktop.platform.system() != "Linux",
                    reason="needs a Linux C compiler")
def test_package_runpath_removes_build_only_entries(tmp_path):
    source = tmp_path / "main.c"
    source.write_text("int main(void) { return 0; }\n")
    binary = tmp_path / "app"
    subprocess.run(["cc", str(source), "-o", str(binary), "-Wl,--enable-new-dtags,-rpath,/build/lib"], check=True)
    package_desktop.package_runpath(binary)
    dynamic = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True, check=True).stdout
    assert "RUNPATH" not in dynamic
