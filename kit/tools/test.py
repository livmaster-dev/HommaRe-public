#!/usr/bin/env python3
"""Run explicit contributor suites, with game-backed checks isolated from player saves.

The game is a directory holding game.toml (--game-dir, default the kit's stub
game); its outputs live under the build root tools/build.py chooses for it."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
import buildlock  # noqa: E402

spec = importlib.util.spec_from_file_location("build_py", ROOT / "tools/build.py")
build_py = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_py)

PORTABLE_TESTS = [
    "tests/test_setup.py", "tests/test_build_py.py", "tests/test_game_config.py", "tests/test_game_literals.py",
    "tests/test_gen_stub_translation.py", "tests/test_stage_game_files.py",
    "tests/test_extract_icon.py", "tests/test_web_launcher.py",
    "tools/tests/test_package_desktop.py", "tools/tests/test_assemble_games.py", "tools/tests/test_build.py",
    "tools/tests/test_discovery_tools.py",
    "tools/recomp/tests/test_mode_probe.py", "tools/recomp/tests/test_texture_pack.py",
    "tools/recomp/tests/test_terrain_detail.py", "tools/recomp/tests/test_buildlock.py",
    "tools/recomp/tests/test_shaders.py", "tools/recomp/tests/test_translate_config.py",
    "tools/recomp/tests/test_translate_insns.py", "tools/recomp/tests/test_jumptables.py",
    "tools/recomp/tests/test_translate_driver.py",
    "tools/recomp/tests/test_translate_seh.py",
    "tools/recomp/tests/test_translate_imports.py",
    "tools/recomp/tests/test_translate_vector.py",
    "tools/recomp/tests/test_translate_noreturn.py",
]


def run(args, env=None):
    """Propagate a test command's failure instead of treating missing coverage as success."""
    subprocess.run([str(arg) for arg in args], cwd=ROOT, env=env, check=True)


def ctest(build_dir, labels, env):
    """Run the CTest entries whose label matches `labels` (a regex)."""
    run([build_py.cmake_tool("ctest"), "--test-dir", str(build_dir), "-L", labels, "--output-on-failure"], env)


def configure(preset, game_dir, build_root):
    build_dir = build_py.build_dir_for(build_root, preset)
    defines = build_py.game_defines(game_dir, build_root)
    # A cross preset (linux-arm64) runs its tests through qemu-user, against the
    # sysroot tools/build.py --arch would use.
    arch = build_py.cross_preset_arch(preset)
    if arch:
        defines.append("-DRECOMP_LINUX_SYSROOT=" + build_py.linux_sysroot(arch).as_posix())
    build_py.configure(preset, defines, build_dir=build_dir)
    return build_dir


def native(preset, env, jobs, run_tests, game_dir, build_root):
    """Build every test binary this platform has; run the suites that need no snapshot."""
    build_dir = configure(preset, game_dir, build_root)
    build_py.build(preset, ["check_binaries"], jobs, build_dir=build_dir)
    if run_tests:
        ctest(build_dir, "nogame|game|gpu|device", env)


def mods_targets(game_dir):
    """Match the mod targets CMake defines for the selected game."""
    targets = ["pop_fixture", "mods_tests"]
    if (game_dir / "mods/examples/luawalk/main.lua").is_file():
        targets.append("present_events_tests")
    return targets


def mods(preset, env, jobs, game_dir, build_root):
    """Generate the real entity fixture locally, then run the mod suites under one build lock."""
    with buildlock.BuildLock(build_root.parent, "mod tests"):
        build_dir = configure(preset, game_dir, build_root)
        build_py.build(preset, mods_targets(game_dir), jobs, build_dir=build_dir)
        output = build_root / "tests"
        output.mkdir(parents=True, exist_ok=True)
        case = Path(tempfile.mkdtemp(prefix="mod-fixture-", dir=output))
        fixture_env = dict(env, RECOMP_NO_MODS="1", RECOMP_FIXTURE="frames:32",
                           RECOMP_OUT=str(case / "snapshots"),
                           RECOMP_PROFILE_DIR=str(case / "profile"),
                           RECOMP_REGISTRY=str(case / "registry.json"),
                           RECOMP_RUN_RECORD=str(case / "run.json"))
        print("Mod fixture diagnostics: %s" % case, flush=True)
        with (case / "fixture.log").open("w") as log:
            result = subprocess.run([str(build_root / "recomp/pop_fixture")], cwd=ROOT, env=fixture_env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
        result.check_returncode()
        snapshot = case / "snapshots/frame32._data_00598000.bin"
        if not snapshot.is_file():
            raise RuntimeError("The entity fixture was not captured; inspect %s" % case)
        ctest(build_dir, "mods", dict(env, RECOMP_TEST_GAME_VIEW_SNAPSHOT=str(snapshot)))


def probe_module():
    spec = importlib.util.spec_from_file_location("mode_probe", ROOT / "tools/recomp/mode_probe.py")
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    return probe


def gameplay(jobs, game_dir, build_root):
    """Replay native Options and movement in a unique profile; retain diagnostics under the build root."""
    run([sys.executable, "tools/build.py", "--game-dir", game_dir, "--target", "smoke", "--jobs", jobs])
    probe = probe_module()
    output = build_root / "gameplay"
    output.mkdir(parents=True, exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="options-", dir=output))
    script_path = game_dir / "smoke/native-options.script"
    if not script_path.is_file():
        raise RuntimeError("The game has no smoke/native-options.script: %s" % script_path)
    script = script_path.read_text()
    pack = build_root / "texture-pack"
    manifest = json.loads((pack / "manifest.json").read_text()) if (pack / "manifest.json").is_file() else {}
    # Material detail ships from project artwork. Original-game replacement
    # textures are optional, so only assert HD replacements when some exist.
    absent = []
    if not manifest.get("textures"):
        absent.append("expect hd_draws")
    if not manifest.get("terrain_detail"):
        absent.append("expect terrain_detail_draws")
    script = "\n".join(line for line in script.splitlines()
                       if not line.startswith(tuple(absent))) + "\n"
    path = case / "input.script"
    path.write_text(script)
    env = probe.probe_environment((640, 480, 16), path, case)
    env.pop("RECOMP_SMOKE_CLASSIC_PROBE", None)
    env.update(RECOMP_SMOKE_DRAWABLE="1280x960",
               RECOMP_DDRAW_MODES="640x480x8,640x480x16,800x600x16,3840x2160x16",
               RECOMP_CORE_MODS_DIR=str(build_root / "recomp/mods/core"),
               RECOMP_TEXTURE_PACK_DIR=str(pack))
    print("Gameplay diagnostics: %s" % case, flush=True)
    with (case / "smoke.log").open("w") as log:
        result = subprocess.run([str(build_root / "recomp/pop_smoke")], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT, timeout=300)
    result.check_returncode()
    text = (case / "smoke.log").read_text()
    for mode in ("800x600", "3840x2160", "640x480"):
        if "display mode %s 16bpp" % mode not in text:
            raise RuntimeError("The gameplay run did not reach %s; inspect %s" % (mode, case))
    if "all expectations met" not in text:
        raise RuntimeError("Gameplay assertions did not complete; inspect %s" % case)


def integration(env, game_dir, build_root):
    """The host integration script: roots, plugins, headless, smoke and fixture runs against the game."""
    run([str(ROOT / "host/tests/integration_tests.sh")],
        dict(env, RECOMP_GAME_DIR=str(game_dir), RECOMP_BUILD_ROOT=str(build_root)))


def main():
    """Choose portable, compile-only or game-backed suites and check their prerequisites."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--native", action="store_true", help="Build and run the native suites")
    group.add_argument("--mods", action="store_true", help="Real game-backed mod tests")
    group.add_argument("--gameplay", action="store_true", help="Scripted native Options and gameplay run")
    group.add_argument("--integration", action="store_true", help="The host integration script against the game")
    group.add_argument("--compile-only", action="store_true", help="Build the native test binaries only")
    parser.add_argument("--preset", default=build_py.default_preset())
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--game-dir", type=Path, default=ROOT / "games/stub",
                        help="Absolute directory holding the game.toml (default: the kit's stub game)")
    args = parser.parse_args()
    if not args.game_dir.is_absolute() or not (args.game_dir / "game.toml").is_file():
        parser.error("--game-dir must be an absolute directory holding game.toml: %s" % args.game_dir)
    cfg = build_py.game_config.load(args.game_dir)
    build_root = build_py.build_root_for(args.game_dir)
    game_backed = args.mods or args.gameplay or args.integration
    if game_backed and platform.system() != "Darwin":
        parser.error("Game-backed suites require macOS")
    if game_backed and not cfg["developer_exe_path"].is_file():
        parser.error("This suite needs your game installation; run tools/setup.py first, "
                     "or run `ctest --test-dir <build dir> -L nogame` for the portable suites")
    if game_backed and not build_py.archive_path(build_root, preset=args.preset).is_file():
        parser.error("Build the game with tools/build.py before running this suite")
    env = probe_module().without_switches(os.environ)
    env["PY"] = sys.executable
    try:
        if args.gameplay:
            gameplay(args.jobs, args.game_dir, build_root)
        elif args.mods:
            mods(args.preset, env, args.jobs, args.game_dir, build_root)
        elif args.integration:
            integration(env, args.game_dir, build_root)
        elif args.native or args.compile_only:
            native(args.preset, env, args.jobs, args.native, args.game_dir, build_root)
        else:
            run([sys.executable, "-m", "pytest", "-q"] + PORTABLE_TESTS, env)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError, TimeoutError) as error:
        parser.exit(1, "Tests failed: %s\n" % error)


if __name__ == "__main__":
    main()
