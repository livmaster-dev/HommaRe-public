# Testing

## Optional timed-media device probes

Build through the game's `tools/test.py --compile-only`. Ordinary CTest does
not open an output device. `audio_clock_probe` submits silence to stereo and
5.1 queues and verifies native presentation stamps, paused time and reset epochs.
`timed_player_probe --silent source.wmv new-delivery.csv` pauses, resumes,
resets and plays a full file against that clock. `--silent-once` supports short,
audio-only and video-only generated fixtures without the one-second lifecycle
sequence. CSV destinations must be new. Source decode warnings stay visible.

These probes measure scheduling into a callback, not actual displayed pixels,
audible AV alignment or six physical speakers. The current strict timestamp
PCM gap/overlap policy is a diagnostic, not a fidelity acceptance. Record exact
binary/source hashes and do not replace a binary during a corpus run. macOS
device access may require an unsandboxed tool run; defer while an owner game is
running. Device stalls fail after5s; the ordinary test suite does not inject a
real-device loss, so that error path needs separate validation.

Run checks appropriate to your change. Every suite's output belongs under ignored
`build/`; requested tests must report failure rather than silently skip prerequisites.

| Command | What it checks | Needs game files? |
| --- | --- | --- |
| `tools/test.py` | Setup failures, synthetic texture processing and display-mode tooling | No |
| `tools/format.py` | Consistent formatting of handwritten native code | No |
| `tools/test.py --compile-only` | Every native test binary this platform has compiles (macOS, Linux, Windows) | No |
| `tools/test.py --native` | Portable suites everywhere; runtime, adapters, offscreen Metal and UI tests on macOS | Partly: `game`-labeled suites need the image |
| `tools/test.py --mods` | Real loader/hooks/settings/native Options and replay contracts | Yes, plus translated archive |
| `tools/test.py --gameplay` | Menu navigation, mode cycling, selection, movement and clean exit | Yes, plus translated archive |

Native suites are CTest entries with labels: `nogame` runs everywhere and in CI,
`game` needs your installation, `gpu` needs a Metal device, `device` needs a
real Metal and audio device (the offline audio render is not what a hosted CI
runner produces), `mods` needs the translated archive and the entity snapshot `tools/test.py --mods` captures. Run
one directly with `.venv/bin/ctest --test-dir build/cmake/macos -L nogame` or `-R dx_tests`.
Two suites cover the on-screen controls, both `nogame`: `controls_tests` for the
layout model, geometry, routing, virtual pad, mapped binding, raster primitives
and editor (including the old keypad geometry as a regression oracle), and
`pad_tests` for the DirectInput joystick and the three XInput DLLs, which lives
beside `dx_tests` because it defines the `host_pad_*` callbacks strongly.
Every `tools/test.py` mode takes `--game-dir /abs/path/to/<game>`; without it
the kit's stub game is used and the `game`-labelled suites report a skip.

Invoke these with `.venv/bin/python`. The native tests need a macOS Metal device;
CI compiles them but does not claim GPU or original-game execution. The mod suite
first builds and runs a deterministic 32-frame entity capture from your own game
in an isolated directory; no saved capture from another checkout is needed. The gameplay
runner uses an isolated profile and original textures unless a local pack exists.

For Linux on 64-bit ARM from an x86-64 Linux host, make the sysroot once with
`tools/linux_sysroot.py --arch arm64`, then `tools/test.py --preset linux-arm64-stub
--native` builds the test binaries and runs them through qemu-aarch64. qemu-user
has the host's page size whatever `-p` says, so `d3d9_pages_16384` and
`d3d9_pages_65536` (`RECOMP_D3D9_HOST_PAGE`) stand in for 16 and 64 KiB kernels.
qemu writes a core file into `kit/` for every child `seh_tests` aborts on purpose
unless `ulimit -c 0` is set first. The GPU suites run on lavapipe under qemu when
`VK_DRIVER_FILES` names the sysroot's `usr/share/vulkan/icd.d/lvp_icd.aarch64.json`
(the loader cannot open it through qemu's path remapping), but very slowly.

For a Windows cross-build, set `LLVM_MINGW_ROOT`, then run
`tools/build.py --preset windows-cross --stub --target app` and
`tools/test.py --preset windows-cross-stub --compile-only`. CI verifies the
Indeo 5/AVI and Vorbis/Ogg components and uses Wine to load `dx_tests.exe` with
only its three copied FFmpeg DLLs. `RECOMP_TEST_AUDIO=/path/to/track.ogg` exercises
the CD-music decoder and requires nonzero PCM; CI generates its own sine tone.
`RECOMP_TEST_AVI=/path/to/movie.avi` exercises the guest AVIFile/Indeo imports,
requiring changing nonblack frames. Movie probes use private local inputs,
never uploaded. These headless probes do not open an audio device or game window.

`RECOMP_TEST_TIMED_MOVIE=/path/to/movie.wmv build/recomp/dx_tests` fully drains
both bounded queues, verifies backpressure, pause/reset/loop identity and channel
separation, and reports decode damage rather than hiding it. Optional
`RECOMP_TEST_TIMED_TRACE=/path/to/frames.csv` records every decoded PTS/duration,
audio sample count/rate and timestamp origin. This is not a realtime player or
an audio/video synchronization test.

`build/recomp/host_tests --audio-identification /path/to/speakers.wav` generates
a nine-second 48kHz WAVEFORMATEXTENSIBLE file through the offline mixer, semantic
sink mapping and SDL stream. Tones begin at0,1.5,3,4.5,6,7.5seconds in logical
L/R/C/LFE/Ls/Rs order. No device opens; playing it later on configured speakers
is a separate physical-routing check. `--audio-only` covers reordered/invalid
speaker layouts; `--audio-device` remains a silent half-second stream probe.

For instruction-translation changes, use the original differential harness:

```sh
.venv/bin/python tools/recomp/tests/test_translate.py --help
.venv/bin/python tools/recomp/tests/test_translate.py
.venv/bin/python -m pytest tools/recomp/tests/test_translate_hooks.py
```

Three translator suites need no game and run in `tools/test.py`:
`test_translate_insns.py` runs synthetic listings of individual instruction
forms through the translator, compiles them with the test harness and
compares registers, flags and memory with Unicorn; `test_jumptables.py`
decodes the jump-table shapes on synthetic functions over a fake image;
`test_translate_driver.py` covers driver rules such as what a withdrawn block
leaves behind. Add a case there first when the translator meets an
instruction or table shape it does not handle.

The differential harness compares translated routines with original instructions
under Unicorn. Unicorn is a development tool, not part of the playable app.

## Manual gameplay checks

- Start a level, select a person, issue a move order and observe the destination.
- Compare Classic/Enhanced and Wide on/off in a window wider than an 800×600 canvas.
- Cycle resolution through 640×480, 800×600 and 3840×2160, then resume play.
- Change graphics/host settings, quit normally, relaunch and check persistence.
- Test Command-Q, focus loss/return and mouse motion at all four edges in each window mode.
- Play the intro long enough to expose streaming stalls; check music and effects in a level.
- Compare animation speed at 60 and 120 FPS. Capture frame-pacing data during real play.

Report macOS/device, selected resolution, window mode, rendering mode and frame
limit. State whether FPS counts submitted, completed or displayed frames. A pinned
smoke clock is deterministic test timing and cannot substantiate real-time performance.

## Current local evidence

The source snapshot includes fixes exercised through native Options, live
640×480 → 800×600 → 4K → 640×480 transitions, unit movement and clean exit.
The initial publication additionally runs the source-only CI checks and local
native suites built through CMake from the standalone checkout, and the Linux and
Windows portable-layer suites in CI. Long campaign completion, multiplayer
and sustained 4K120 remain unverified; publish measurements with their conditions.

## Bounded D3D9 metadata history (Metal, opt-in)

`RECOMP_D3D9_TRACE=1` allocates a ten-frame circular history at backend startup.
The default4096draws/1024events per frame uses90,112,000bytes including the current
frame. Recording never allocates or performs I/O. `host_d9_probe_next_frame(tag)`
flushes existing history immediately in this mode; without it the legacy next-
frame probe still applies. Set RECOMP_HOST_DUMP_DIR to a private writable folder.
Only the existing RECOMP_D3D9_PROBE_HOTKEY diagnostic gate enables the F8 trigger.

Build through tools/test.py --compile-only. Run d3d9_trace_tests for bounded ring,
immutable data, overflow and serialization checks. Run host_tests --d3d9-trace
with RECOMP_D3D9_TRACE=1 on Metal for twelve frames containing intentional draw
failures and recovery; output JSONL retains the last ten. The regular --d3d9-gpu
suite must also pass with the trace environment unset. External JSON checking
should confirm labels, frame/result sequence, resource generation changes and
mutation of caller-owned constants/labels after submission.

This is metadata for backend-submitted work, not a GPU capture or visibility
oracle. Object/model IDs and earlier game/shim rejected draws are not represented.
Serials distinguish allocation/CPU upload/potential GPU writes and completion.
Inspect overflow counters before drawing conclusions. Ten frames at60Hz span
about167ms; a delayed human trigger can miss a one-frame disappearance.

## Bounded volume DDS probe (Metal, optional private fixtures)

Build with `tools/test.py --compile-only`. `d3d9_tests` covers the allocation-free
`dx/dds_volume.h` reader with non-cubic mip chains, truncation, malformed headers,
format/pitch rejection and resource limits. This is deliberately a narrow view
of linear ARGB8/XRGB8 volume DDS, not a general DDS importer.

`host_tests --d3d9-volume-dds /absolute/file.dds [more.dds ...]` reads bounded
private fixtures, uploads and reads back every mip/slice, then rasterizes every
voxel through an explicit-LOD volume shader and compares all output bytes.
Use `MTL_DEBUG_LAYER=1` on Metal. Missing arguments exit2; missing/malformed files
fail explicitly. No fixture names or proprietary bytes are compiled into source.
The normal `host_tests --d3d9-gpu` suite must continue to pass independently.

This proves texture storage/sampling for the supplied bytes, not the correctness
of any game's material, sampler configuration, vertex data, constants or passes.

## Timing classifier provenance and thunk fallback

The optional `RECOMP_FRAME_TIMINGS` and `RECOMP_PRESENT_ACK_TRACE` CSVs now append
`screen_class_valid`. Require `1` before interpreting `screen_class` as a phase.
D3D9's external finished-image path provides no phase signal: its compatibility
composition class is still `0`, with validity `0` on original and repeated frames.
Older CSVs lack this provenance and cannot establish phase timing by label alone.
No rendering, transition, pacing or input decisions consume the new flag.
The throughput helper rejects missing/false validity and reports unavailable
phase timing when no classified gameplay samples remain.

After `tools/test.py --compile-only`, `runtime_tests --thunks-only` runs the
fast-probe, interpreter fallback, stack/RET preservation and failure-path tests.
A `guest thunk ... fast-path miss` means only the small prober declined the code;
`recomp_unknown_call` next tries the integer interpreter. An `interp: routine ...
stopped` or `call to unknown target` remains an execution problem. A successful
synthetic bank routine does not establish every game's bank semantics.
