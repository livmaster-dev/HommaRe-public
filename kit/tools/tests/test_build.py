"""Build-tool platform gates and translator arguments, without invoking the translator."""

import importlib.util
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import pytest

spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def test_mods_targets_match_the_game_fixture(tmp_path):
    spec = importlib.util.spec_from_file_location("test_runner", build.ROOT / "tools/test.py")
    test_runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(test_runner)

    assert test_runner.mods_targets(tmp_path) == ["pop_fixture", "mods_tests"]
    fixture = tmp_path / "mods/examples/luawalk/main.lua"
    fixture.parent.mkdir(parents=True)
    fixture.write_text("-- fixture\n")
    assert test_runner.mods_targets(tmp_path) == ["pop_fixture", "mods_tests", "present_events_tests"]


def test_android_templates_render(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    manifest = (out / "app/src/main/AndroidManifest.xml").read_text()
    assert 'package="dev.recompkit.stub"' in manifest
    gradle = (out / "app/build.gradle.kts").read_text()
    assert "StubRecomp" in gradle
    assert 'applicationId = "dev.recompkit.stub"' in gradle
    assert 'resValue("string", "game_id", "stub")' in gradle
    sdl_java = tmp_path / "gen/_deps/sdl3-src/android-project/app/src/main/java"
    assert sdl_java.resolve().as_posix() in gradle
    assert "externalNativeBuild" not in gradle
    android = "{http://schemas.android.com/apk/res/android}"
    root = ET.fromstring(manifest)
    feature = root.find("uses-feature")
    assert feature.get(android + "name") == "android.hardware.vulkan.version"
    assert int(feature.get(android + "version"), 16) == (1 << 22) | (1 << 12)
    # Installable without Vulkan: the launcher then says why the game cannot start.
    assert feature.get(android + "required") == "false"
    touch = [f for f in root.findall("uses-feature") if f.get(android + "name") == "android.hardware.touchscreen"]
    assert touch and touch[0].get(android + "required") == "false"
    permissions = {p.get(android + "name") for p in root.findall("uses-permission")}
    assert {"android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE",
            "android.permission.CHANGE_WIFI_MULTICAST_STATE"} <= permissions
    app = root.find("application")
    assert app.get(android + "label") == "@string/app_name"
    assert app.get(android + "requestLegacyExternalStorage") == "false"
    activity = app.find("activity")
    assert activity.get(android + "screenOrientation") == "fullUser"
    assert activity.get(android + "resizeableActivity") == "true"
    assert activity.find("layout").get(android + "defaultWidth") == "1280dp"
    # One ABI by default; the libraries go in stripped and are unpacked at install.
    assert 'abiFilters += listOf("arm64-v8a")' in gradle
    assert "isEnable = false" in gradle
    assert "useLegacyPackaging = true" in gradle


def test_android_project_carries_each_abi_and_drops_others(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    stale = tmp_path / "android/app/src/main/jniLibs/armeabi-v7a/libmain.so"
    stale.parent.mkdir(parents=True)
    stale.write_text("old")
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen", abis=("x86_64", "arm64-v8a"))
    gradle = (out / "app/build.gradle.kts").read_text()
    assert 'abiFilters += listOf("x86_64", "arm64-v8a")' in gradle
    assert 'include("x86_64", "arm64-v8a")' in gradle
    assert "isEnable = true" in gradle
    assert not stale.parent.exists()


def test_android_stage_puts_each_abis_libraries_in_its_folder(tmp_path, monkeypatch):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    gen_dirs = {}
    for abi, modules in (("x86_64", "ON"), ("arm64-v8a", "ON")):
        gen = tmp_path / ("cmake-" + abi)
        (gen / "host").mkdir(parents=True)
        (gen / "host/libmain.so").write_text("main " + abi)
        (gen / "_deps/sdl3-build").mkdir(parents=True)
        (gen / "_deps/sdl3-build/libSDL3.so").write_text("sdl " + abi)
        java = gen / "_deps/sdl3-src/android-project/app/src/main/java/org/libsdl/app/SDLActivity.java"
        java.parent.mkdir(parents=True)
        java.write_text("class SDLActivity {}")
        (gen / "CMakeCache.txt").write_text("RECOMP_VIDEO:BOOL=OFF\nRECOMP_GAME_MODULES:BOOL=%s\n" % modules)
        gen_dirs[abi] = gen
    # A module from an earlier multi-game build is not this build's.
    left = tmp_path / "android/app/src/main/jniLibs/x86_64/libgame.other.so"
    left.parent.mkdir(parents=True)
    left.write_text("old")
    monkeypatch.setattr(build, "android_strip_tool", lambda: None)
    monkeypatch.delenv("ANDROID_NDK_HOME", raising=False)
    out = build.android_stage(tmp_path, cfg, gen_dirs=gen_dirs, game_dir=tmp_path / "game")
    jni = out / "app/src/main/jniLibs"
    for abi in gen_dirs:
        assert sorted(p.name for p in (jni / abi).iterdir()) == ["libSDL3.so", "libmain.so"]
        assert (jni / abi / "libmain.so").read_text() == "main " + abi
    assert not (out / "app/src/main/assets/ffmpeg-NOTICE.md").exists()
    assert (out / "app/src/main/assets/resources/classic-modes.json").is_file()


def test_android_resources_split_desktop_and_touch_mods(tmp_path):
    game = tmp_path / "game"
    (game / "ui-mods").mkdir(parents=True)
    for name in ("Wide", "Touch"):
        (game / "ui-mods" / (name + ".recipe")).write_text(name)
    (game / "launcher-manifests").mkdir()
    (game / "launcher-manifests/m.json").write_text("{}")
    (game / "launcher-games.json").write_text("{}")
    cfg = {"dir": str(game), "launcher_config_dir": game,
           "android": {"ui_mods": ["Wide"], "touch_ui_mods": ["Wide", "Touch"], "profile_files": {}}}
    out = build.android_write_resources(tmp_path / "res", cfg)
    files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    assert files == ["classic-modes.json", "launcher-games.json", "launcher-manifests/m.json",
                     "ui-mods-touch/Touch.recipe", "ui-mods-touch/Wide.recipe", "ui-mods/Wide.recipe"]
    # A module's resources: the launcher's files are the app's alone.
    module = build.android_write_resources(tmp_path / "module", cfg, launcher=False)
    assert not (module / "launcher-games.json").exists()
    assert not (module / "launcher-manifests").exists()


def test_android_core_assets_replace_stale_manifests_without_shipping_binaries(tmp_path):
    game = tmp_path / "game"
    core = game / "mods/core/display"
    core.mkdir(parents=True)
    for name in ("mod.toml", "palette.json", "display.c", "display.dylib", "display.so"):
        (core / name).write_text(name)
    out = tmp_path / "apk"
    stale = out / "app/src/main/assets/mods/core/stale/mod.toml"
    stale.parent.mkdir(parents=True)
    stale.write_text("stale")
    profile = out / "profile/save.dat"
    profile.parent.mkdir(parents=True)
    profile.write_text("keep")
    build.android_stage_core_assets(out, game)
    staged = out / "app/src/main/assets/mods/core"
    assert sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()) == [
        "display/mod.toml", "display/palette.json"]
    assert profile.read_text() == "keep"
    (core / "mod.toml").write_text("new")
    build.android_stage_core_assets(out, game)
    assert (staged / "display/mod.toml").read_text() == "new"


def test_android_assets_carry_the_games_control_layouts(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    game_dir = tmp_path / "game"
    (game_dir / "layouts").mkdir(parents=True)
    (game_dir / "layouts/pad.json").write_text("{}\n")
    (game_dir / "layouts/pad.phone-portrait.json").write_text("{}\n")
    (game_dir / "layouts/readme.txt").write_text("not a layout\n")
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    controls = out / "app/src/main/assets/controls"
    stale = controls / "gone.json"
    stale.parent.mkdir(parents=True, exist_ok=True)
    stale.write_text("{}\n")
    build.android_stage_layout_assets(out, game_dir)
    assert sorted(p.name for p in controls.iterdir()) == ["pad.json", "pad.phone-portrait.json"]


def test_android_assets_drop_controls_when_the_game_ships_none(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    controls = out / "app/src/main/assets/controls"
    controls.mkdir(parents=True)
    (controls / "pad.json").write_text("{}\n")
    build.android_stage_layout_assets(out, tmp_path / "game")
    assert not controls.exists()


@pytest.mark.parametrize("system", ["Darwin", "Linux", "Windows"])
def test_android_target_selects_the_ndk_preset(system):
    for extra, preset in [([], "android"), (["--stub"], "android-stub")]:
        args, _ = build.parse_args(["--target", "android"] + extra, system=system)
        assert build.preset_name(args.preset, args.config, stub=args.stub, target=args.target) == preset
    assert build.TARGETS["android"] == ["recomp_app"]


def test_android_builds_a_tree_per_abi():
    args, _ = build.parse_args(["--target", "android"], system="Linux")
    assert args.abi == ("x86_64", "arm64-v8a")
    args, _ = build.parse_args(["--target", "android", "--abi", "arm64-v8a", "--abi", "arm64-v8a"], system="Linux")
    assert args.abi == ("arm64-v8a",)
    assert build.build_dir_for("/b", "android", "x86_64") == Path("/b/cmake/android-x86_64")
    assert build.build_dir_for("/b", "android-stub", "arm64-v8a") == Path("/b/cmake/android-stub-arm64-v8a")
    assert build.build_dir_for("/b", "linux") == Path("/b/cmake/linux")


@pytest.mark.parametrize("extra, message", [
    (["--target", "app", "--abi", "x86_64"], "are for --target android"),
    (["--target", "app", "--stage-only"], "are for --target android"),
    (["--target", "android", "--module-only"], "--module-only needs --game-modules"),
    (["--target", "android", "--game-modules", "--module-only", "--push-game"], "--push-game installs an APK"),
])
def test_android_options_are_checked(capsys, extra, message):
    with pytest.raises(SystemExit):
        build.parse_args(extra, system="Linux")
    assert message in capsys.readouterr().err


@pytest.mark.parametrize("devices, console, actions", [
    ("", True, []),
    ("phone\toffline\nlocked\tunauthorized\n", True, []),
    ("phone\tdevice\n", False, ["install", "shell"]),
    ("phone\tdevice\n", True, ["install", "shell", "logcat"]),
])
def test_android_install_requires_a_ready_device(tmp_path, monkeypatch, devices, console, actions):
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\n" + devices)

    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    monkeypatch.setattr(build.subprocess, "run", run)
    apk = tmp_path / "app-debug.apk"
    build.android_install_and_launch(apk, "dev.recompkit.stub", console=console)
    assert calls[0] == ["/sdk/adb", "devices"]
    assert [command[3] for command in calls[1:]] == actions
    if actions:
        assert calls[1] == ["/sdk/adb", "-s", "phone", "install", "-r", str(apk)]
        assert calls[2] == ["/sdk/adb", "-s", "phone", "shell", "am", "start", "-n",
                            "dev.recompkit.stub/dev.recompkit.RecompActivity"]


def test_android_install_does_not_guess_between_devices(tmp_path, monkeypatch):
    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\nfirst\tdevice\nsecond\tdevice\n")

    monkeypatch.setattr(build.subprocess, "run", run)
    with pytest.raises(ValueError, match="Pass --device"):
        build.android_install_and_launch(tmp_path / "app-debug.apk", "dev.recompkit.stub")
    assert calls == [["/sdk/adb", "devices"]]


def test_android_push_stages_only_included_game_files(tmp_path, monkeypatch):
    source = tmp_path / "original/gog/app"
    files = {
        "Stub.exe": b"test executable", "Data/sprites.bin": b"sprites",
        "AUDIO/Music/track.mp3": b"music", "model.txt": b"model",
        "BINKS/intro.bik": b"video", "__support/helper.exe": b"installer",
        "sound.dll": b"dll", "Data/unused.dll": b"dll",
    }
    for name, data in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    cfg = {"game": {"executable": "Stub.exe", "bundle_id": "dev.recompkit.stub"},
           "developer_exe_path": source / "Stub.exe",
           "bundle": {"exclude": ["BINKS", "__support", "*.dll"]}}
    output = tmp_path / "build"
    staged = output / "android/game"
    staged.mkdir(parents=True)
    (staged / "stale.dll").write_bytes(b"old staging must not bypass exclusions")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        if "push" in command:
            assert command == ["/sdk/adb", "-s", "tablet", "push", str(staged),
                               "/sdcard/Android/data/dev.recompkit.stub/files/"]
            assert sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()) == [
                ".stamp", "AUDIO/Music/track.mp3", "Data/sprites.bin", "Stub.exe", "model.txt"]
            for name in ("Stub.exe", "Data/sprites.bin", "AUDIO/Music/track.mp3", "model.txt"):
                assert (staged / name).read_bytes() == files[name]
            import hashlib
            assert (staged / ".stamp").read_text().strip() == hashlib.sha256(files["Stub.exe"]).hexdigest()
        return subprocess.CompletedProcess(command, 0, "List of devices attached\ntablet\tdevice\n")

    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    monkeypatch.setattr(build.subprocess, "run", run)
    build.android_install_and_launch(output / "app-debug.apk", "dev.recompkit.stub",
                                     game_cfg=cfg, build_root=output)
    assert [command[3] for command in calls[1:]] == ["install", "shell", "push", "shell"]
    assert calls[2][4:] == ["mkdir", "-p", "/sdcard/Android/data/dev.recompkit.stub/files"]
    assert calls[-1][4:7] == ["am", "start", "-n"]


def test_android_push_without_device_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\n")

    monkeypatch.setattr(build.subprocess, "run", run)
    with pytest.raises(ValueError, match="No Android device attached.*--push-game"):
        build.android_install_and_launch(tmp_path / "app-debug.apk", "dev.recompkit.stub",
                                         game_cfg={}, build_root=tmp_path)
    assert calls == [["/sdk/adb", "devices"]]


@pytest.mark.parametrize("extra", [["--target", "app"], ["--target", "android", "--no-install"]])
def test_push_game_rejects_options_that_cannot_push(extra, capsys):
    with pytest.raises(SystemExit) as error:
        build.parse_args(extra + ["--push-game"])
    assert error.value.code == 2
    assert "--push-game requires --target android without --no-install" in capsys.readouterr().err


def test_app_target_allowed_on_linux():
    args, _ = build.parse_args(
        ["--target", "app", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert args.preset == "linux"


def test_ios_target_still_needs_macos(capsys):
    with pytest.raises(SystemExit) as error:
        build.parse_args(
            ["--target", "ios", "--stub", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert error.value.code == 2
    assert "The iOS packager runs on macOS" in capsys.readouterr().err


def test_unsigned_ios_needs_no_team(monkeypatch):
    monkeypatch.delenv("RECOMP_IOS_TEAM", raising=False)
    args, _ = build.parse_args(
        ["--target", "ios", "--unsigned", "--game-dir", str(build.ROOT / "games/stub")], system="Darwin")
    assert args.unsigned and args.team == ""


@pytest.mark.parametrize("extra", [["--target", "ios"], ["--target", "app", "--unsigned"]])
def test_unsigned_is_only_for_ios_and_signing_needs_a_team(extra, monkeypatch, capsys):
    monkeypatch.delenv("RECOMP_IOS_TEAM", raising=False)
    with pytest.raises(SystemExit) as error:
        build.parse_args(extra + ["--game-dir", str(build.ROOT / "games/stub")], system="Darwin")
    assert error.value.code == 2
    assert ("--unsigned is for --target ios" if "--unsigned" in extra else "needs --team") \
        in capsys.readouterr().err


def test_allow_table_gaps_reaches_the_translator(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build", allow_table_gaps="switch 004ab2af")
    assert "--allow-table-gaps" in calls[0]
    assert calls[0][calls[0].index("--allow-table-gaps") + 1] == "switch 004ab2af"


def test_acceptances_reach_each_auxiliary_module(tmp_path, monkeypatch):
    """A module's listing has the same gaps a game's has, so the build's
    acceptances cover it too."""
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    (tmp_path / "stage").mkdir()
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build",
                         allow_table_gaps="switch 1003c81c", aux_modules=("blit",),
                         allow_unmodelled="SSE in the math library")
    assert len(calls) == 2
    assert calls[1][calls[1].index("--allow-table-gaps") + 1] == "switch 1003c81c"
    assert calls[1][calls[1].index("--allow-unmodelled") + 1] == "SSE in the math library"


def test_discovered_reaches_the_translator(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build",
                         discovered=tmp_path / "discovery.txt")
    assert calls[0][calls[0].index("--discovered") + 1] == str(tmp_path / "discovery.txt")


def test_no_table_gap_flag_by_default(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(build.subprocess, "run", lambda cmd, **kw: calls.append(cmd))
    build.run_translator(tmp_path / "stage", tmp_path, tmp_path / "build")
    assert "--allow-table-gaps" not in calls[0]


def test_incremental_build_refreshes_runtime_header_without_translation(tmp_path, monkeypatch):
    args, parser = build.parse_args(["--target", "gen"], system="Darwin")
    args.build_root = tmp_path / "build"
    gen = args.build_root / "recomp/gen"
    gen.mkdir(parents=True)
    header = gen / "x86.h"
    header.write_text("stale runtime header\n")
    table = gen / "table.c"
    table.write_text("existing translation\n")
    original_table = table.stat().st_mtime_ns
    monkeypatch.setattr(build, "parse_args", lambda argv: (args, parser))
    monkeypatch.setattr(build, "run_translator", lambda *a: pytest.fail("unexpected regeneration"))
    timestamps = []

    def configure(*a, **kw):
        assert header.read_bytes() == (build.ROOT / "runtime/x86.h").read_bytes()
        assert table.read_text() == "existing translation\n"
        assert table.stat().st_mtime_ns == original_table
        timestamps.append(header.stat().st_mtime_ns)

    monkeypatch.setattr(build, "configure", configure)
    monkeypatch.setattr(build, "build", lambda *a, **kw: None)
    build.main()
    build.main()
    assert timestamps[0] == timestamps[1]  # unchanged headers do not rebuild every chunk
