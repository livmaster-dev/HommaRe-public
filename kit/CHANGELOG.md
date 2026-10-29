# Changelog

## Windows

- The llvm-mingw cross build (`windows-cross`, `LLVM_MINGW_ROOT`) compiles again: zlib
  is fetched and built statically, the UCRT shims map POSIX's `*_l` locale functions
  to the CRT's, and the clocks, sleeps and `setenv` of the native sources come from the
  platform layer. An MSVC-ABI build (CI's Windows runner) builds too.
- `tools/build_app.py --target windows`: the three games in one folder and a zip,
  `HommaRe.exe` with the expansions as game modules (`modules/libgame.<id>.dll`,
  `host/game_modules.cpp`), `SDL3.dll`, FFmpeg's DLLs and `innoextract.exe`; the
  DWARF is stripped from the packaged files. `game.toml` gains `[windows]`.
- Every Windows executable carries `platform/windows/app.manifest`: UTF-8 as the
  process's code page, so the narrow paths the kit passes around open under a
  Cyrillic user name. The app is a GUI program (no console) with the game's icon and
  a VERSIONINFO.
- The native launcher is the Windows one too: installing from GOG's offline installers
  with `innoextract.exe`, finding GOG, Galaxy and Steam installs, the Windows display
  languages; `launcher-games.json` `taglines` gains `windows`.
- libcurl's stand-in has one HTTP GET (`runtime/http_get.cpp`) on the platform
  layer's sockets for every platform, so the ubi-gs prompt fetches on Windows too.
- Paths the platform layer hands out on Windows (the profile, the temporary folder)
  use '/'; a game path with backslashes finds its game root and texts.

## Sockets on every platform

- `runtime/winsock.cpp` runs on the platform layer's sockets: `platform/os.h`
  gains `os_sock_*`/`os_net_*` (non-blocking IPv4 sockets, poll, host name,
  resolution and the interface list) on POSIX and Winsock 2, so WinSock builds
  for Windows, Linux and Android as well as Apple systems. The Bonjour relay
  stays Apple's.
- WinSock's answers where POSIX differs: a failed connect shows in `select`'s
  exceptfds and `SO_ERROR`, not writefds; a blocking stream `send` queues all of
  it; a broadcast without `SO_BROADCAST` is `WSAEACCES`; `select` with no
  sockets, and `getsockname` on an unbound socket, are `WSAEINVAL`; a short
  `SIO_GET_INTERFACE_LIST` buffer is `WSAEFAULT`. Host names resolve on a host
  thread, and an interface scan happens once a second, not per datagram.
- A limited broadcast goes out through every interface (virtual LAN adapters
  included); `RECOMP_NET_PEERS` adds unicast copies for networks without
  broadcast.
- `[net] movable_udp_ports`: a second copy on one machine moves off the game's
  taken UDP ports (+100) instead of sharing them, since a shared port delivers
  a unicast datagram to one of its sockets only.
- Failed socket calls go to the log; `RECOMP_TRACE_SOCKETS=1` also shows each
  datagram's first bytes.
- `GetLastError`/`WSAGetLastError` are per thread, as on Windows: a call into
  the runtime may run another guest thread, whose error then replaced the
  caller's. Heroes V's Ubi.com thread took the main thread's
  `ERROR_FILE_NOT_FOUND` for its lobby connection's error after a `recv` that
  would block, closed the connection, and creating a game failed.

## A GS game's server list from a community server (ubi-gs)

- `[online] gsconnect_host` names the host a Ubisoft Game Service title asks for its
  servers (Heroes V: `gsconnect.ubisoft.com`). libcurl.dll's stand-in
  (`runtime/curl_http.cpp`) sends that one request, over plain HTTP, to a server the
  player names instead; Ubisoft's domain is never contacted, and every other URL still
  fails offline. Without the key nothing changes.
- The host asks while the game waits (`host/server_prompt.cpp`): `[online] prompt_title`
  and `prompt_text`, a field for an IP address or host name with an optional `:port`,
  and the servers that answered before, most recent first, the first one preselected.
  They are kept in the profile's `ubi-gs-servers.txt` (ten at most). A server that does
  not answer brings the question back with the reason; Cancel gives the game the
  failure it had before.
- The question is a page in the settings menu's place (`page_overlay.h`
  `HostPagePrompt`). A game asking from the thread that presents its frames gets it on
  the presenter's repeated frames, so a blocked main thread still shows it; text input
  (a phone's keyboard) is raised while it is open.
- `[online] default_server` ("host" or "host:port") is offered in the field and the list
  while the profile's list is empty; once a server answered, the most recent one is.
- `RECOMP_AUTO_KEYS` takes `text:...`, typed into such a prompt.
- The settings menu's TrueType canvas moved to `runtime/gdi32_canvas.h`, shared with it.

## Linux

- The kit builds and runs on Linux (x86-64): Vulkan for Direct3D 9, GPU skinning
  included, the native launcher, and several games in one app.
- The launcher's tagline can differ per platform (`launcher-games.json` `taglines`).

## Renaming an app

- `[game] previous_app_names` and `previous_bundle_ids` name what an app was called
  before. At start the host renames the first former user-data folder that exists to
  the new name (`host_migrate_own_user_data`; a game module does the same for its own),
  once, and never when the new folder exists. The macOS launcher copies
  `launcher.roots`, `launcher.lastGame` and `launcher.language` from the former bundle
  id's defaults while its own have none, with folders that moved along re-pointed.

## Native address header

- `[translate] native` takes `addresses`, a header passed as `RECOMP_ADDRESS_HEADER`
  to the native sources alone. The translated chunks include only the overrides
  header, so changing an address there rebuilds the native objects, not the translation.

## Direct3D 9: shader objects, FVF and the fixed-function pipeline

- `CreateVertexShader`/`CreatePixelShader` make shader objects from vs/ps 1.x-3.0
  bytecode (`GetFunction`, `GetDevice`); `Set`/`Get*Shader` bind them, referenced as
  Direct3D 9 does. Pixel shaders get the 224 float registers ps_3_0 has.
- `SetFVF`/`GetFVF` bind a declaration built from the FVF code.
- A draw with no vertex or pixel shader runs a vs_2_0/ps_2_0 program generated from the
  fixed-function state (`dx/d3d9_ffp.cpp`): transforms, `SetMaterial`/`SetLight`/
  `LightEnable` lighting, vertex fog, texture coordinate generation and transforms,
  and the `SetTextureStageState` cascade. The CPU renderer runs the same programs.
- `LockRect` honours its rectangle (texture atlases were written at the top-left);
  `AddDirtyRect`, transform/material/light/stage/stream getters and integer
  shader registers are implemented; ps_1_x `texm3x2*`/`texm3x3*` lookups translate.

## Bounded capture reliability (diagnostic only)

- Opt-in D3D9_CAPTURE_SESSION uses explicit F8 arm/trigger, three repeatable
  captures, immutable on-screen frame/capture markers, and background saves.
- Full unfiltered samples at 2 Hz retain recent history and six seconds after
  a trigger; every intervening frame retains attempted/retained/dropped counts.
- Measured diagnostic capacity is 6144 draws and 1024 resource events per sample.
  Overflow or incomplete coverage is reported; no gameplay pool or draw changes.

## Post35 runtime compatibility (checkpoint36)

- Continue the ANSI formatter through `wsprintfA`, with guest cdecl varargs and
  stack checks; add the missing argument counts for 40 existing imports.
- Preserve exact eight-byte PE names and ordinal identity in comparison tests.
  Use a restoring in-memory version fixture when the image lacks RT_VERSION;
  retain missing-resource checks and run rejected-decorations tests after coverage.
- Full runtime passes 1235 checks with three image-dependent skips.

## Continuation36 diagnostic provenance

- Mark external-renderer frame classes as unclassified in timing/acknowledgement
  CSVs; preserve the value through repeats without changing composition or pacing.
- Require affirmative class validity before the throughput helper selects gameplay.
- Clarify thunk fast-path misses separately from interpreter/unknown-call failures;
  add a focused thunk fallback regression mode.

## Milestone35 local diagnostics and bundle integrity

- Add opt-in bounded game audio mix/postmix statistics and an isolated eight-role
  speaker diagnostic; keep mixer gains and routing unchanged.
- Record Metal command/encoder completion failures and presenter commit ages
  without changing pacing or target lifetime.
- Generate the final macOS bundle version before signing, preserving its
  signature across later configure-only and incremental builds.


## Milestone31 local movie-colour validation

- Add an optional explicit timed-movie colour policy, preserving legacy defaults.
- Gate the USA game adapter on original HD/SD shader coefficients; validate pixels,
  reset and muted playback. Still QA-only; no stable player release in this milestone.

## Milestone28 — volume input verification

- Add a bounded ARGB8/XRGB8 volume DDS view and optional Metal fixture probe.
- Verify original USA mip data and static effect inputs without changing the
  production game path or admitting an unverified car/lighting shader.

## Local sampled-volume groundwork (milestone27)

- Add bounded D3D9 sampled-volume resources, mip ownership/locks, 3D shader
  sampling and Metal storage with synthetic ABI/GPU regression coverage.
- Car-effect integration and actual volume/light-context data remain open.

## Local shader compatibility groundwork

- Translate legacy dependent texture reads and fragment derivatives with GPU
  semantic checks; carry vertex/pixel Boolean constants through guest effects
  and immutable host draws, including local-definition precedence.
- Add a private-bytecode Metal compilation diagnostic. Compilation alone does
  not enable custom effects, supply volume textures or establish game parity.

## Unreleased

- Add isolated bounded timed-media playback and a platform audio presentation
  clock. AudioQueue buffer callbacks release storage; they never advance the
  movie clock or call guest/GPU code. Silent probes test pause/reset/EOF.
- Separate audio EOF from video EOF and avoid a synchronous output pause at
  the video-tail transition. Support very short audio and video-only timing.
  Callers may select bounded decoded-anchor preroll with unchanged PCM samples;
  strict PCM gap/overlap editing remains a separate diagnostic policy.
- Bound small native presentation-clock corrections by holding the last native
  high-water mark; reject larger jumps/discontinuities rather than extrapolating.
- Expose validated queue-level gain and optional native media focus/shutdown
  callbacks. Device callbacks never call into the game or renderer.

- Report physical display pixels from the SDL desktop mode and its density,
  with bounded fallbacks, without changing logical pointer coordinates.
- Allow an explicit diagnostic environment flag to capture the next D3D9 frame
  with F8. Ordinary applications retain their original F8 input behavior.

- Report the actual stereo DirectSound speaker configuration (4); value 2
  incorrectly advertised mono. Check output and invalid-pointer behavior.
- Add an opt-in, bounded physical controller diagnostic with separate motor
  pulses and motion-sensor observations. Enumeration alone sends no motor output.

- Replace repeated binary searches for dense translated CALL continuations with
  an immutable, bounded bitmap. Keep exact membership and a binary-search
  fallback for sparse images. Copy import diagnostic labels without printf
  formatting on every graphics call.

- Apply the host frame limit at Direct3D 9 Present using fractional deadlines
  and cooperative waits, without changing simulation clocks. Resetting a
  device or changing the limit clears the old deadline. Explicitly set and
  report Metal display synchronization for both presentation modes.

- Correct D3D9 assembly EXP/EXPP to base-two exponentiation, including the
  component results of vertex shader 1.1 EXPP. GPU pixel regression checks
  cover negative fractional inputs; the CPU fallback uses the same semantics.

- Reopen disconnected SDL controllers even when their removal event is missed,
  and log wired/wireless transport for connection diagnostics.

- Generate text through TranslateMessage only, fixing doubled letters in game
  text fields. DirectInput axes default to 0..65535 with a neutral value of
  32768, while explicit game-selected ranges and signed XInput sticks remain supported.

- On macOS, immediate Metal presentation also disables CAMetalLayer display
  synchronization. The default presentation mode remains unchanged.

- Allow native game adapters to place their own cursors at absolute touch
  positions in logical game pixels. Placement can discard pending DirectInput
  X/Y movement without losing button, wheel or keyboard input.

- Notify exclusive DirectDraw windows when the display mode is set, including
  a return to the same size after a movie, so their menu presentation bounds
  are refreshed instead of leaving the screen black.

- Find hash-pinned auxiliary DLLs in their configured installation subfolders
  after moving a game directory to a mobile device or another computer.

- Initialize settings-page input in the smoke host on its first presented frame,
  matching the app so automated F10 and native Options checks can open the page.

- Update held on-screen stick knobs independently of their bases and touch
  zones. Stick motion now reaches the presenter without rebuilding the cached
  control backgrounds.

- Keep touch and mouse coordinates in the game's logical resolution when
  Direct3D 9 renders at a higher resolution. Supersampling no longer moves
  clicks and cursors beyond the game's window.

- Mapped pads support horizontal-only arrow sticks for steering without
  accidental throttle or braking. Smoke pad scripts exercise the production
  mapped binding as well as the native controller adapters.

- Convert host mouse and touch positions from client to screen coordinates
  for GetCursorPos and MSG.pt, fixing click offsets in games whose window
  starts away from the desktop origin.

- Smoke scripts can drive native virtual-pad buttons, dpad, sticks and
  triggers through the same DirectInput/XInput adapters as the app.

- Switching from a collapsed keyboard to the pad no longer hides its sticks
  and buttons. Saved hidden bits apply only to groups with a reveal tab.

- Keep tablet keyboard halves and their KEYS tabs inside the system safe area,
  including the combined pad-and-keys layout. Controls tracing now records
  touch routing and keyboard press/release events for device diagnostics.

- Preserve GPU frame color formats when staging them for display. BGRA
  backbuffers no longer have red and blue exchanged by an RGBA staging texture,
  including when a pooled frame switches between GPU and CPU pixel uploads.

- Auto-hidden touch controls no longer leave HIDE/KEYS buttons on screen
  when a hardware keyboard or controller suppresses the layout. The layout
  switch stays available, touches through hidden controls reach the game,
  and disconnecting restores the player's saved keyboard visibility.

- Windows cross-builds enable FFmpeg movie and file-backed music decoding,
  use the POSIX host's shell/make with llvm-mingw tools, and bundle the media DLLs.
  CI cross-builds the Windows app and decodes a generated Ogg track under Wine.
- Ports can request an output render size independently of the logical
  display canvas, keeping UI/input coordinates stable at high resolutions.
  A changed request applies to the next frame without resizing leased targets.
- macOS bundles rebuild their core plugins when plugin sources or manifests change.
- Android links core plugins into its native library and refreshes their packaged
  manifests/data before startup, preserving user mods and player profiles.
- Linux and Windows desktop apps also link core plugins and package their
  manifests/data, making game adaptations available in standalone packages.
- Mod hook callbacks can call allocated import trampolines through `guest_call`,
  preserving registers just as calls to translated functions do.
- Static archives live inside each CMake preset's build tree, preventing desktop
  and mobile builds of the same game from overwriting each other's libraries.
- iOS icon extraction accepts a same-named sibling ICO when a valid executable
  has no embedded icon resource.
- Auxiliary DLLs now detach on their final FreeLibrary and initialize again
  from verified, import-patched image bytes on reload. This lets games rebuild
  their renderer when changing resolution. Handle lookups do not acquire a
  load reference, and failed initialization reports an error.
- Colour-keyed RGB textures now apply the legacy implicit alpha test when
  COLORKEYENABLE is set and explicit alpha testing is off, preserving the
  content behind transparent menu overlays even without alpha blending.
- Added read-only AVIFile and Indeo 5 Video for Windows decoding, including
  cdecl ICDecompress, YUV410P conversion and empty-frame image retention.
  WinMM waveform output now plays
  PCM and IMA ADPCM with queued WAVEHDR completion and redirection aliases.
  Abandoned waveform handles are released before host audio shutdown.
- Miles digital driver initialization now returns a driver handle, retains
  preferences and master gain, and decodes named MP3 effects. Optional
  `[media].cd_tracks` maps disc track numbers to file-backed music, with
  Ogg/Vorbis added to the packaged FFmpeg build.

- Renderer target pressure waits for completion before accepting new guest
  writes, preserving incremental HUD updates during ordinary GPU backpressure.
  Scene-slot capacity now covers the presenter's full target pool.
- Short x87 arithmetic listings use the instruction bytes to distinguish
  ST0 and STi destinations, preserving matrix multiplication results.
- D3D3, Device3, Viewport3 and Material3 expose their versioned ABI, FVF
  vertex input, texture binding and single-stage texture states. Unsupported
  vertex-buffer/strided paths return errors with the correct stack cleanup.
- Ports may opt into `translate.resumable_stacks` when their guest scheduler
  switches stacks. CALL continuations become dispatch entries and a changed
  return address unwinds to the entry driver before resuming the guest.
- Auxiliary modules accept reviewed `entry_points`; runtime discovery records
  missing DLL code, and each translation filters discoveries to its image.
  Table recovery also recognizes pre-scaled byte offsets into dword tables.
- Decorated stdcall imports retain their argument cleanup even before their
  implementation exists. Startup shims now cover system locale, bounded user
  name queries and window minimization; unavailable AVI/MCI paths return errors.
- Auxiliary DLL imports can bind directly to the main executable's code and
  data exports. Module lookup includes the executable, and GetProcAddress
  resolves named and ordinal exports with image bounds checks.
- Opening a predefined registry root with a null or empty subkey succeeds
  even when the writable profile contains no values for that root.

- On-screen controls replace the split keypad. Every game now starts with a
  PlayStation-styled gamepad as well as the keyboard: two sticks, a dpad,
  ✕○□△, shoulders and triggers, start and select, drawn over the game. A
  tab cycles between the `pad`, `keys` and `pad+keys` layouts and a Hidden
  slot, and the F10 page carries the layout, its size, its opacity, button
  haptics and whether the pad stays on screen when a controller is
  connected. The keyboard itself is unchanged: same halves, same keys, same
  hold-to-chord, tap-to-latch, double-tap-to-lock, and the old
  `host.keypad/*` settings carry over on first run.

- Layouts are files, and a player can edit them on the device. "Edit
  controls" on the F10 page opens an editor: drag to move, pinch to resize,
  add or delete a control, rebind it, snap to a 10 pt grid and to other
  controls, then save or reset to the game's default. Edits are saved per
  game and per form factor under `<profile>/controls/`, so a phone in
  portrait and a tablet keep separate layouts. A game repo ships its own
  starting layouts in a `layouts/` directory, which the build copies into
  the app.

- Physical controllers work everywhere: a DualSense, Xbox or MFi pad opens
  through SDL with hot-plug, on desktop, iOS and Android, and feeds the
  same virtual pad the on-screen controls do. A pad-only layout hides
  itself while a controller is connected (turn that off with "Pad with
  controller"), and a keyboard layout still hides itself when a hardware
  keyboard is attached. Rumble from the game reaches the controller, or the
  phone or tablet's own motor when there is no controller, and a light
  haptic tap answers each on-screen press.

- Phones are supported, including portrait. Layouts come in `tablet`,
  `phone-landscape` and `phone-portrait` forms, iPhone and Android phones
  may rotate, and in portrait the game is pinned to the top at full width
  with the controls filling the space below it, so nothing covers the game.
  Tablets stay landscape.

- Games that read a controller can be given a real one. With
  `[controls] pad = "native"` the virtual pad appears as a DirectInput
  joystick and through `xinput1_3`, `xinput1_4` and `xinput9_1_0`, so the
  game's own controller support drives it and its `XInputSetState` rumble
  comes back out. Otherwise the pad is mapped to keys and the mouse, with a
  per-game table under `[controls.mapped]`.

- `game.toml` gains a `[controls]` section — `default_layout`, `pad`, and
  the `[controls.mapped]` and `[controls.native]` tables — which replaces
  `[touch] keypad`. The old spelling is still read (`"auto"` → `"keys"`,
  `"hidden"` → `"hidden"`) so no game repo has to be re-pinned at once.
  One behaviour change: `[touch] keypad = "hidden"` used to hide the two
  halves but keep their KEYS tabs on screen; it now selects the Hidden
  layout and draws nothing. No game in the kit sets it.

- Video decoding works in the Windows build. With the presets' MSVC-ABI
  clang, FFmpeg is built by its own MSVC toolchain from an MSYS2 shell and
  make (found beside each other, so Git's or WSL's bash is not used), its
  import libraries are linked from `bin/`, and its DLLs are copied beside
  every built executable. Windows CI installs MSYS2's make and builds with
  video on, so the Media Foundation and Bink tests run there too.

- CI passes on main again after the Siege of Avalon merge: the repository
  check allows the bundled GeneralUser GS SoundFont, and the Media Foundation
  topology test is skipped in builds without video decoding, where no file
  opens as a source.

- Launcher: a long title shrinks to fit (and is cut short only below the body
  size), a path wraps after a slash, Up and Down keep the column when the
  buttons are in two, a screen too short for its text keeps the buttons on
  it, and the Manage screen drops the import instructions. The launcher tests
  build on Windows.

- The launcher offers only folders holding the whole game: a folder with the
  executable but without every `[setup] required_dirs` entry (an unpacked
  patch in Downloads, say) is no longer listed.

- The desktop app accepts `--launcher`; its argument check printed the usage
  and exited before the launcher could read the flag.

- `runtime/native_seam.h` lists what a game's native overrides may call: the
  area a fullscreen window fills (`host_display_screen_size`, reported by the
  SDL host from the fullscreen window, which keeps clear of a camera notch, or
  else the display, and by the smoke host from `RECOMP_SMOKE_DRAWABLE`), the
  profile path a guest file would be written to (`recomp_writable_path`,
  nothing without an overlay), the path a guest file is read from
  (`recomp_readable_path`), and `ddraw_add_mode`. Hosts without a display or
  DirectDraw get defaults that report none.

- CI passes on Linux and Windows too. Handwritten sources are formatted to
  the repository's clang-format again. The SEH fatal paths flush stdio before
  `abort()`, so the line that says why survives a stderr redirected to a
  file, which glibc buffers (`seh_tests` lost it on Linux). `os_wait` reports
  a Windows release runtime's fast-fail abort (0xC0000409) as SIGABRT, as it
  already did the debug runtime's exit status 3. The literal checker, the
  mode probe's diff headers and two tool tests compare paths the same way on
  Windows as elsewhere.

- CI builds again on every platform. The GeneralUser GS SoundFont the hosts
  bundle was ignored by `.gitignore` (`*.sf2`) and never committed, so every
  packaging step failed to copy it and a public checkout had no music; it is
  tracked now. The smoke host includes `<algorithm>` and `<cstdint>` for the
  compilers that do not bring them in transitively (Linux, Windows), and the
  watchpoint's backtrace is compiled out on Android, whose NDK has
  `backtrace` only from API 33. The instruction tests skip on Windows, where
  their POSIX shared-library harness cannot be built.

- The portable suites pass again. The instruction harness defines the
  runtime globals the memory writers read (the watchpoint and the DirectDraw
  write ranges), which it had been missing since the watchpoint arrived; the
  driver and SEH tests expect a popped-return jump to end in `recomp_return`;
  and the adopted-epilogue case no longer also configures the epilogue as an
  entry point, which since configured entries stopped being absorbed made it
  test the opposite of its intent.

- Window creation runs without a thread switch. The import checkpoints could
  hand the baton to another guest thread in the middle of `CreateWindowEx` and
  its creation messages, and Delphi's VCL passes the control being created to
  the first message through a global (`CreationControl`): with a worker and
  the main thread both creating windows, the message took the other thread's
  control, called a freed object instance through Delphi's `StdWndProc`, and
  the access violation reached an untranslated top-level handler. Siege of
  Avalon crashed so in a level, and a close in that state was left unanswered
  until the host unwound it. `CreateWindowExA/W` skip their entry checkpoint,
  and the shim runs in a `sched_atomic_enter` stretch that a guest exception
  unwinding past it ends. `runtime_tests` checks both.

- A windowed DXGI swap chain on a program's own top-level window owns the
  display, as a fullscreen one does: the display mode becomes its back-buffer
  size and the window covers it. The host shows one window, and it is the
  program's, so the program's window is the picture. Before, Siege of Avalon
  with Fullscreen unchecked put a 1920x1080 window on the host's 1024x768
  fallback desktop and showed its top-left corner. Its presents now take the
  Direct3D 11 hardware path too. A chain on a child window is still a picture
  inside its window. `dx_tests` and `headless_tests` check both.

- The presenter finishes the Direct3D 11 hardware path's GPU work before it
  stops (`host_gpu2d_release_device`). A run that ended mid-frame left a
  render pass open, and Metal aborted the process at exit when the encoder was
  released without `endEncoding`.

- DXGI swap chains no longer choose the host window. A fullscreen chain
  posted fullscreen into the same slot as the Display setting, so Siege of
  Avalon's `ForceD3DFullscreen=1` overrode the player's choice after the
  launcher. A chain now sets only the guest's display mode and window bounds,
  and the host window follows the Display setting, which defaults to windowed.

- `game.toml [settings] rows` lists the settings rows a game shows, as
  `RECOMP_SETTINGS_ROWS`; without the key every row shows. The F10 page and the
  native Options tabs show only those rows, and a row a game does not list
  keeps its neutral value. The keypad rows wrap like the display rows, and
  their values are atomics. With Wide view off, a 4:3 scene on a wide drawable
  is boxed at the guest's aspect, with its HUD, pointer and input mapped
  through the same box. The page's scale is capped to what the drawable holds.

- Leaving or entering fullscreen with the window's own button changes the
  Display setting, so the next frame no longer puts the window back.

- `game.toml [mods] builtin = "none"` compiles out the mod runtime's Populous
  hooks, which name Populous routines by address. In any other game the first
  hook was refused and the loader stopped before Lua, so no mods loaded. An
  event a game names no routine for is now skipped instead of failing the load.

- The kit no longer carries Siege of Avalon's HLSL. `dx/d3d11.cpp` describes
  the shader contract in words instead of quoting the LGPL source, and
  `dx_tests` hands the shim the tagged blobs `D3DCompile` would make - entry
  point, target, source digest - from `fixtures/quad_shaders.h`, which replaces
  the source fixture. The scaffold test now checks the digest `D3DCompile`
  computes. NOTICE says what is described and that nothing is included.

- Direct3D 11 draws the GPU can reproduce exactly go to the GPU
  (`host/gpu2d.cpp`). A draw that is two triangles tiling an axis-aligned
  rectangle, a texel to a pixel, inside its texture, is a copy with a blend,
  and the compositor program draws it into a GPU copy of the render target;
  textures are uploaded, converted to RGBA8 and only where they changed, when
  a draw samples them. At `Present` the back buffer is copied into the
  presenter's frame on the device when GDI says the swap chain has the whole
  screen (`gdi_surface_covers_screen`, `gdi_present_external`), and GDI reads
  it back only if it has to compose over it. Everything else - other draws,
  `Map`, `UpdateSubresource` into a target, windowed presents - reads the
  target back and stays on the software rasterizer, which remains the
  reference; a target read back three frames running stays there.
  `RECOMP_D3D11_SOFTWARE=1` turns the path off. Siege of Avalon's frame was a
  1920x1080 rasterized copy and three more full-frame copies on the CPU at
  every present; it is now an upload when the game changes the picture and a
  GPU copy per present.
  `dx_tests` runs the texel-copy scene through a software double of the host
  and checks the fallback; `host_tests` checks the host side on Metal.

- The rasterizer's texel copy converts every 16-bit format, the packed R16
  decode included, through whole-word tables, and swaps 8-bit channel order
  word by word. Siege of Avalon's main layer is R16 drawn with the packed
  decode, which still went a channel at a time and was a quarter of its
  render thread on the Mac. An Unlock's per-pixel diff skips unchanged
  stretches eight bytes at a time and compares the rest inline instead of
  calling `memcmp` for every pixel of a changed row.

- `WaitMessage` waits until there is a message or a paint to retrieve, pumping
  timers and host input meanwhile, instead of returning at once. Delphi's idle
  handler calls it whenever the queue is empty, and a WaitMessage that came
  straight back turned `TApplication`'s loop into a spin that asked for the
  cursor sixty thousand times a second and kept the scheduler baton: Siege of
  Avalon's render and mouse threads, which run only when it is let go,
  presented at 50 fps on a 120 Hz display. With no host to post a message it
  still returns at once. `runtime_tests` checks it waits for a late message.

- A present that covers the whole screen skips the work beneath it.
  `gdi_present_windows` no longer reads a primary, composes every window and
  copies the snapshot over them when the snapshot is the screen's size and
  covers it; it hands the snapshot on as it is. The ARGB to RGBA conversion in
  `host_display_present_window`, the D3D11 present's conversion and the
  rasterizer's 5-6-5 texel copy run through plain pointers and whole-word
  tables. At 1920x1080 a Siege of Avalon frame's present fell to about 3 ms.

- The offscreen presenter's refresh wait uses the rate its synthetic display
  link ticks at, 120 Hz. It used the 60 Hz default, so a smoke run's
  `Present` with a sync interval waited for every other tick of its own link
  and no smoke could show more than 60 fps.

- A DirectDraw Unlock compares only the rows the guest wrote while its lock was
  open. A guest Lock opens a write range over the surface's pixels
  (`recomp_dirty`, up to four at once) that every translated store updates, and
  Unlock narrows its diff to the rows the range saw - provided no import that
  could write the surface ran in between. Lock, Unlock, the surface's AddRef,
  Release, GetSurfaceDesc and IsLost, critical sections and
  `UpdateSubresource` say they write no surface
  (`imports_call_leaves_surfaces`); any other import falls back to the whole
  compare. Siege of Avalon locks its 1920x1080 back buffer and a source
  surface for every sprite it draws, and the full compare of each was the
  largest cost on its main thread. `dx_tests` checks the narrowed diff, the
  fallback after an import, and that with neither nothing else is compared.

- The settings page works for a program that only presents windows - a D3D11
  renderer, the VCL, a film. Such a frame never registered the page's input
  (only `host_present` did), so F10 did nothing, and it sealed without the page
  texture (only `host_frame_seal` attached it), so a page opened any other way
  was never drawn. `host_display_present_window` now registers the input, and
  both seal paths attach the page through one helper; `host_tests` checks a
  window seal carries it.

- The SDL host closes the audio device before `SDL_Quit`
  (`host_audio_shutdown`). The mixer's sink is a static, so the process's exit
  handlers destroyed it after SDL had torn its audio down, and
  `SDL_DestroyAudioStream` locked a freed mutex: every game that exited on its
  own ended in a segfault, which the fault handler reported against the last
  guest address. Found under lldb from Siege of Avalon's Exit; the Mac app now
  ends with status 0, and the iOS host, which ends with `exit`, the same way.

- `MsgWaitForMultipleObjects` and its Ex form answer for their handles: a
  signalled one is WAIT_OBJECT_0 + its index, ahead of a queued message, and
  MWMO_WAITALL waits for all of them. The call only ever reported a message or
  a timeout, and Delphi's `TThread.WaitFor` on the main thread loops on it
  until the thread's handle is signalled - so stopping a thread hung the
  program. Siege of Avalon stops its D3D mouse thread on the way out, and
  choosing Exit left it running until the host unwound it.

- A call into the first 64 KB raises an access violation through the guest's
  own exception handlers, as it faults on Windows, instead of returning 0. A
  call through a nil interface reads a zero vtable and lands there; a Delphi
  program turns the fault into EAccessViolation and a try/except around the
  call carries on, and Siege of Avalon relies on that around draws whose
  surface can be nil. Returning 0 ran on with the garbage and aborted at the
  next jump through it - on the iPad at every start, and on the Mac now and
  then at exit. Only when no handler takes the fault does the call return 0
  as before. `seh_tests` checks the record and that other unknown targets are
  not faults.

- DirectDraw write tracking keeps one baseline copy per surface instead of a
  copy per Lock and a hash of the whole surface at every final Unlock. A
  program that draws text a glyph at a time locks its whole back buffer for
  each glyph, and each paid three full passes over it - a copy, a compare and
  an FNV hash; hovering a conversation's replies in Siege of Avalon at
  1920x1080 spent half a core there. The baseline is the "before" of every
  write lock, the Unlock diff compares with it and brings the changed band up
  to date, a retained pointer's stores are found by comparing with it, and a
  Lock skips even that compare when nothing has written the surface since -
  blits copy their own rectangle into it. What is left is the one compare the
  diff needs; `Surface_Unlock` no longer shows in the profile. Baselines
  belong to their surfaces: a recorder reset keeps them, a surface's release
  and a full DirectDraw reset drop them.

- `IDXGISwapChain::Present`'s refresh wait is a scheduler sleep, as `Sleep`
  is, instead of a host sleep. Guest threads run one at a time, and a game
  that presents from its own thread slept through every refresh holding the
  baton: in Siege of Avalon that was 38% of the time, sampled while the player
  hovered over a conversation's replies, with the thread that reads the mouse
  and redraws the highlight frozen for all of it. The presenter now returns
  the delay (`host_present_refresh_delay`) and the shim waits it out in the
  scheduler, so the other threads run.

- A game that plays MIDI and ships no instrument bank now has music. The
  SoundFont search knew only one game's bank (`Sound\POPFIGHT.SF2`), so a
  game that relied on Windows' own General MIDI synthesizer - whose bank
  cannot be redistributed - was accepted and not heard. The kit carries
  GeneralUser GS 2.0.3 (`third_party/soundfonts/generaluser-gs`, its own
  permissive licence) and offers it after the game's own bank: apps bundle it
  as `general-midi.sf2` with its licence (macOS, iOS, the desktop archives),
  and a developer build reads it from the kit tree. `host_tests` and
  `test_package_desktop` check both.

- A DirectDraw blit whose source is its own destination copies as DirectDraw
  does, as though through a temporary. A game that scrolls its map buffer by
  blitting it onto itself moves it down or right as often as up or left, and
  the rows were copied top down, so a downward scroll read back rows it had
  just written and the first band repeated to the bottom: walls and banners
  smeared into vertical strips. Rows now run bottom up when the destination is
  below the source, and a keyed or scaled overlap reads a copy of the source
  taken first. The host never showed it on a DirectDraw screen - a blit record
  reads the source's leased revision - which is why only a renderer reading
  its own back buffer (a D3D11 present) did. `dx_tests` scrolls a surface in
  every direction, and with a key, against the copy DirectDraw makes.

- The runtime's GDI draws Windows' own sans-serif interface faces with a
  bundled Open Sans (Apache-2.0, `third_party/fonts/opensans`, embedded at
  build time by `cmake/EmbedFiles.cmake`). A program that draws text without
  setting a font gets the VCL's default, which is a Windows face - Tahoma,
  Segoe UI - that no game registers, and the kit drew it in its fixed 8x16
  bitmap cells: a game's speech over its characters came out as blocky
  terminal text. Segoe UI, Tahoma, Microsoft/MS Sans Serif, the MS Shell Dlg
  aliases, Arial, Verdana, Calibri, Trebuchet MS and Helvetica now resolve to
  Open Sans, the semibold at weight 600 or more; a face the program registers
  itself still wins, and fixed-pitch, serif, system raster and unknown faces
  keep the cells. Each substituted name is logged once. `gdi_tests` measures
  the advances both ways.

- `IDXGISwapChain::Present` honours its sync interval: the guest thread sleeps
  to the presenter's next refresh boundary, as a Present returns at the
  vertical blank on Windows. A renderer that asks for one paces its whole loop
  by that return, and returning at once let a game present eight hundred times
  a second - its main thread never left the render loop to pump input, which
  played as lag, and eight megabytes of staging a call outran the GPU by
  hundreds of megabytes a second. `DXGI_PRESENT_TEST` and
  `DXGI_PRESENT_DO_NOT_WAIT` still return at once, and so does a headless
  presenter, so smokes run as fast as they did.

- The Metal device drains an autorelease pool in every entry point. Its
  callers are the presenter's worker and the guest thread, and neither runs a
  run loop; Metal hands out command buffers, encoders and drawables
  autoreleased, so on those threads nothing ever let go of them. A drawable is
  a 23 MB IOSurface at 3024x1898, and they piled up at a frame a refresh:
  vmmap showed 54 GB of `IOAccelerator` regions on a process forty seconds
  old, most of it paged out, which is the "unchecked memory growth" and a
  good part of the lag. With a pool per call the graphics footprint holds at
  about 250 MB.

- The software Direct3D 11 rasterizer copies texel rows where it can. A
  triangle with one 1/w at every vertex has screen-linear texture coordinates,
  and when those put a texel centre under every pixel centre - both formats
  four bytes, or the R16 word through the packed-565 decode, no blending, no
  decode of an 8-bit source - the per-pixel shading reduces to the texel
  itself, so `raster_triangle` copies each covered row instead of sampling,
  blending and writing 2 million pixels one float channel at a time. That is
  the whole-surface present quad a 2D D3D11 pipeline draws every frame, and it
  cost 60 ms at 1920x1080: a 16 fps ceiling on a game whose own painting took
  a fraction of that, felt as a pointer that trailed the hand. The copy takes
  the general loop's own arithmetic to the byte (a 65536-entry table for the
  packed decode, built from the loop's float expressions), is taken only where
  that arithmetic would land on texel centres, and a test draws the same scene
  through both paths and compares every word. `dx11::present` likewise takes
  8-bit channels as the bytes they are, and the GDI present copies a
  same-sized presented surface by rows rather than dividing per pixel. The
  creator went from 17 real frames a second to 30-75, with the rest of the
  time now the game's.

- A fullscreen DXGI swap chain sizes its output window to the mode it puts the
  display in, and gives the bounds back when it leaves fullscreen, through the
  same path as SetWindowPos so a window procedure hears WM_WINDOWPOSCHANGED. A
  window created before any mode exists has the desktop fallback's size, and
  mouse messages are routed by window bounds: a 1024x768 form on a 1920x1080
  mode left everything right of x=1024 reaching no window at all, which is how
  half of a character creator - the training list, its OK area, the Continue
  button - took no clicks while the other half did. Windows does this resize
  itself; the runtime now does too, and the swap-chain test hands the chain a
  real window and checks its rectangle both ways.

- `RECOMP_TRACE_POINTER` also stamps host state transitions - focus, window
  occlusion and exposure, pointer capture and enter/leave, hit-kind changes -
  on the frame-timings clock, so they can be laid beside the presenter's
  acknowledgement trace. Laid that way, every completion-fallback stretch
  matched a focus loss or occlusion exactly: macOS stops presenting a window
  nobody can see, and the frames that time out then are frames nobody misses.
  Without the alignment it read as a stutter.

- A media session raises `MEEndOfPresentation` when the presentation runs out,
  before `MESessionEnded`. A player is entitled to ignore the latter, and this
  one does so by name; what ends playback is the former, from whose handler the
  player stops the session and posts its own "playback ended" message to the
  window that owns the film. Raising only `MESessionEnded` left a film that
  reached its last frame and then nothing: no Stop, no Close, and a game
  waiting on a message that never came, showing the black the film had faded
  to. Only ever reached by letting a film run out - every smoke until now
  pressed Escape - which is how it stayed hidden behind a movie that was not
  visible in the first place.

- `RECOMP_TRACE_GDI` also reports keyed `BltFast` calls with the key range and
  the source rectangle, and counts the ones whose rectangle is empty. A sprite
  sheet indexed through a table that never loaded still blits, still reports
  success and still writes nothing: the rectangle is the only thing that says
  so, and "the text is missing" looks identical whether the glyphs are absent,
  transparent or one pixel wide.

- A media session presents through the window seam, not the DirectDraw one.
  `host_present` stages a guest-sized copy and leaves publishing to the
  DirectDraw recorder's frame sealing - and while a movie plays the game is not
  drawing, so nothing ever seals and every staged frame is dropped: a black
  screen with the soundtrack playing over it. It also takes only 8 and 16bpp,
  so the fit had to be flattened to RGB565 first. The frames now go out as
  32-bit ARGB through `host_display_present_window`, which stages and seals a
  frame itself, the way a renderer painting its own window does. The smoke host
  forwards one call to the other, which is exactly why this only ever appeared
  in the real one.

- `RECOMP_TRACE_FILES=1` names every guest file open and attribute query with
  the host path it resolved to, and `RECOMP_TRACE_GDI` now also reports
  `DrawText` with its DC, that DC's size and the string. "The text is missing"
  and "the text is empty" look identical on screen and are different bugs.

- A JMP through the entry stack slot now ends the way a RET does. The proof
  behind that emission establishes where the jumped-to value came from, never
  what it is, and a block recovered as a function of its own begins at delta
  zero holding whatever its real caller pushed. Delphi's finally idiom - PUSH
  resume; CALL cleanup; POP EAX; JMP EAX - puts a continuation INTO the
  establishing body there, and setting EIP and returning dropped it: the body's
  epilogue never ran, so it never restored EBP, and its caller went on reading
  its own locals through a frame pointer that had moved. That surfaced as a
  window painting nothing, four blits away, with every handle in the call
  reading as rubbish. Routing it through recomp_return leaves a genuine return
  exactly as cheap as it was and dispatches the rest.

- `RECOMP_WATCH=<hex address>[:<length>]` reports every guest write that
  touches those bytes, and `RECOMP_WATCH_FRAME=1` reports a guest call that
  returns with EBP changed. A routine that loses the frame pointer corrupts
  nothing and crashes nowhere: its caller simply reads its own locals from
  somewhere else afterwards, and the damage surfaces as a wrong value in an
  unrelated place, which is the hardest kind of fault to work backwards from.
  The frame report names the call and the EIP the callee left off at, so the
  routine that did it is read off the log rather than deduced. Both are off by
  default; unarmed they cost one compare that is never taken.

- A configured entry point is never withdrawn again. This file's own policy
  says a `[translate] entry_points` address is established code, and `resolve`
  duly adopts it - but `extend_finally_body` then read a PUSH of that address
  inside another function's span as naming a pushed cleanup continuation,
  re-decoded the bytes into the enclosing body, and retired the configured body
  into an alternate, which emits no dispatch entry at all. The address the port
  had verified was simply absent from the translation, and the only symptom was
  a call to it returning zero. For Delphi that PUSH is how a window procedure is
  handed to `MakeObjectInstance`, so a form's every message - WM_PAINT included
  - ran nowhere. An address that a jump table, `__initterm` or config names is
  now neither absorbed as a continuation nor retired, and a declared entry that
  still fails to be adopted says so rather than going missing quietly.

- A media session posts `MESessionTopologyStatus` carrying
  `MF_TOPOSTATUS_READY`. A player does its renderer setup from that event and
  from nowhere else: it is where `MFGetService` is called for
  `IMFVideoDisplayControl`, where `SetVideoWindow` is called, and where the
  interface every later repaint goes through is stored. A session that posted
  only `MESessionTopologySet` left that field nil, and the failure surfaced as
  a call through a nil interface inside the player, nowhere near this layer -
  the video was decoding and presenting the whole time, through this layer's
  own path rather than the renderer the player thought it had. `MFGetService`
  now also names the object and the service asked of it under
  `RECOMP_MF_TRACE`, because a refusal there is invisible until something calls
  through what it did not return.

- A refused blit reports its geometry and the frame that asked for it, and
  `RECOMP_TRACE_GDI=1` reports every blit, the DC every window paint hangs off
  (`BeginPaint`, and whether the window was one this layer knows),
  `CreateCompatibleDC`, and a `BeginBufferedPaint` that this layer declines.
  Between them they say which painting path a program took, which is the
  question a blank window actually poses. A destination with no bitmap behind
  it and a source of zero are indistinguishable from the guest, which is told
  nothing either way; the geometry says which it was, and the caller chain says
  who asked. It is how "the picture was never drawn" is told apart from "the
  picture was drawn and lost".

- A call to an address the translation does not cover now reports the caller's
  registers. For an indirect call they say what the call was made ON: a virtual
  dispatch reached its target through a word in the object, so the registers
  separate "the vtable slot is empty" from "the pointer is not an object at
  all" - a distinction the address alone cannot make, and one that turned an
  unexplained null call into a pointer that was never an object, its first
  word being two characters of text.

- `RECOMP_HEAP_QUARANTINE=1` retires a freed guest block instead of returning
  it to the free list, and lets no neighbour absorb it. A guest that keeps
  using memory it has freed then reads its own dead object rather than
  whatever was allocated over the top of it, so a use-after-free faults where
  it is rather than wherever the reused block happens to be written next. It
  answers the question either way: a fault that survives quarantine unchanged
  was never a reuse at all. Off by default, and a run with it on never
  recycles a byte, so it is a diagnostic and not a way to play.

- Media Foundation plays a file. `dx/mf.cpp` puts the objects a player builds -
  source resolver, media source, presentation and stream descriptors, media
  type handler, topology and its nodes, renderer activates, media session,
  events, async results, presentation clock, `IMFVideoDisplayControl`,
  `IMFAudioStreamVolume` and `MFGetService` - over the FFmpeg reader in
  `mf_media.cpp`. The topology is recorded rather than resolved: the session
  plays the source its nodes name, which it recognises by the objects handed to
  `SetUnknown` rather than by the attribute GUID naming them, so it does not
  depend on knowing `MF_TOPONODE_SOURCE`. What a player actually observes is
  the event order, so `MESessionTopologySet`, `MESessionStarted`,
  `MESessionPaused`, `MESessionStopped`, `MESessionEnded` and
  `MESessionClosed` are posted as they happen and collected through the
  `IMFAsyncCallback` the player armed with `BeginGetEvent`. `Close` delivers
  its event before returning, because a player blocks on an event it sets from
  its own `Invoke` and the frame pump cannot run while that wait holds the
  guest thread.

- A playing session owns the screen. Video is decoded to the wall clock and
  presented at the guest's own display mode - aspect kept, the remainder
  black - never at the file's resolution, because the host maps pointer
  coordinates back through whatever was last presented. While a session is on
  screen the DirectDraw primary, the GDI window present and GDI window
  compositing all stand aside (`mf_owns_the_screen`, weak in `runtime/`, strong
  in `dx/`), and the screen goes back to the game the moment the file ends.
  Without the compositing half of that the movie was presented and then painted
  out by the game's own black window, which is what a real renderer's window
  would have been sitting in front of.

- `ole32!PropVariantClear` and `PropVariantCopy`. Both are delay imports for a
  Delphi Media Foundation player, and an unresolved delay import is not quiet:
  the stub raises 0xC06D007F, which surfaces as an external-exception dialog
  and takes the process with it. Every PROPVARIANT the shims produce is
  VT_EMPTY, so emptying the sixteen bytes is the whole of clearing one.

- The vendored FFmpeg decodes MS-MPEG-4 part 2 (`msmpeg4v1,v2,v3`). A `.wmv`
  from the Windows Media Encoder era usually carries fourcc MP43 rather than a
  WMV-numbered codec, and the demuxer that reads the container is no use
  without the decoder that reads the frames.

- Diagnostics: `RECOMP_MF_TRACE` narrates a session (what opened, what the
  topology named, every event, whether the video keeps up with the clock), and
  `RECOMP_PRESENT_TRACE` names every frame reaching the screen by geometry,
  depth and whether it is blank - which is how one presenter is told from
  another when two of them reach the same screen.

- A fullscreen DXGI swap chain sets the mode USER32 reports. `GetSystemMetrics`
  and everything else reading the virtual screen took the DirectDraw mode or
  the default desktop, so a game that switched to 1920x1080 through Direct3D
  still laid out and hit-tested against the mode before the switch: in Siege's
  character creator the pop-up lists sat beyond the old 1024-pixel width and
  no click on them landed. Releasing fullscreen hands the screen back to the
  DirectDraw mode.

- DirectDraw locks are cheaper when a guest locks a whole surface to change a
  little of it, which is how a DXR text draw works and why hovering a menu's
  text crawled. The retained-pointer hash takes eight bytes a step instead of
  one (and still catches any changed word, tail bytes included), write-lock
  shadows reuse earlier buffers instead of zero-filling a new one, and the
  unlock diff masks only the band of rows that changed. On a creator hover
  script the text routine's share fell from 23.8 s to 4.6 s; the records it
  produces are unchanged.

- A display mode larger than the screen no longer opens a window larger than
  the screen. The desktop host sized its window at a whole multiple of the
  mode in points, never below one, and made the mode its minimum size, so a
  1920x1080 mode opened a 1920x1080-point window on a 1512x982-point Retina
  laptop and could not be shrunk. Such a mode now takes the largest whole
  multiple of its frame in drawable pixels that fits the usable screen -
  960x540 points there, every guest pixel one drawable pixel - or, when none
  fits, the largest size that does, with the minimum never above the window.
  Modes that fit keep the size they had (`host_window_size_for`).

- Fonts a program registers with `AddFontMemResourceEx` are drawn with.
  The data used to be discarded and every string drawn in the fixed 8x16
  cells; now the fonts are kept and rasterized with stb_truetype (vendored,
  MIT or public domain), and a DC whose font names one measures by its
  advances and metrics - `GetTextExtentPoint32W`, `GetTextMetricsW`,
  `DrawTextW` wrapping and alignment - and draws anti-aliased outlines in
  `ExtTextOutW`, `DrawTextW` and the themed text calls. A face the program
  did not register keeps the bitmap cells; the kit ships no fonts. The tests
  use a generated two-glyph font (`runtime/tests/make_test_font.py`).

- The system `STATIC` control class. Windows draws a static control's text
  in the class's own window procedure, and a program that subclasses it -
  the VCL's `TStaticText` does, through `GetClassInfoW` - hands it every
  message it does not handle. With no such class the lookup failed, the
  subclass fell back to `DefWindowProc`, and every caption stayed blank. The
  procedure keeps `WM_SETFONT`, redraws on `WM_SETTEXT`, asks the parent for
  colours with `WM_CTLCOLORSTATIC`, and paints into the DC `WM_PAINT` or
  `WM_PRINTCLIENT` carries when there is one: a double-buffering program
  paints its control into a memory DC and copies that over the window, so
  painting the window directly was covered by the copy.

- `SetWindowPos` shows and hides. `SWP_SHOWWINDOW` and `SWP_HIDEWINDOW` are
  the transitions `ShowWindow` makes, and the VCL shows every child control
  with the first and never calls `ShowWindow` for one, so each stayed hidden
  and was never asked to paint.

- `WS_CLIPCHILDREN`. A child window's DC writes into its top-level window's
  surface, so a parent repainting painted over its visible children; a DC of
  a window with the style now leaves their areas alone, as Windows does.

- Visual styles are on, with no theme data. `IsThemeActive` and
  `IsAppThemed` answer yes, as every Windows since Vista does; `OpenThemeData`
  finds nothing, so controls draw the classic way. Every uxtheme export a
  VCL program binds is present, since it calls through whatever pointer it
  got and a delay-loaded one that is missing raises instead.
  `DrawThemeParentBackground` does the real work - the parent paints its
  background into the child's DC through `WM_ERASEBKGND` and
  `WM_PRINTCLIENT`, which is how a transparent control shows the window
  behind it - and `DrawThemeText`/`DrawThemeTextEx` draw their text in the
  DC's font rather than failing, or a program with styles on loses its
  captions. Buffered painting and animation report no buffer.

- `comctl32.dll` has a version resource: 6.10 when the executable's manifest
  binds `Microsoft.Windows.Common-Controls` 6.0, else 5.82. The VCL decides
  whether to paint with visual styles by that number.

- `CharUpperBuffA`, `CharLowerBuffA`, `GetStringTypeExA`, `GetStringTypeExW`
  and `FlushInstructionCache`. A Delphi runtime built this decade builds its
  ANSI case tables from inside a unit's initialization by running every
  byte value through the first two, and an import the kit does not know is
  called with its arguments left on the stack: two calls a byte, eight bytes
  a call, and the unit-init loop popped its own counter back as garbage and
  stopped with more than half the program's units never initialized. It was
  the PNG reader's chunk registry that made it visible - "unknown but
  necessary chunk" on a perfect file - but every unit after the one building
  the tables was missing.

- A pushed continuation is decoded and dispatchable. Delphi leaves a
  finally block with `PUSH continuation; ...; POP reg; JMP reg`; the jump
  dispatches on a variable, so nothing names the continuation - not the
  dangling-target check, not recursive descent, which cannot follow a push.
  A body that consumes a pushed address that way now grows into it when its
  listing stopped short, and the address becomes a block entry as a
  jump-table target does, so `recomp_jump` reaches it from any body that
  shares the code - a recovered block can overlap a listed function's grown
  tail and execute the same jump without holding the label. A push followed
  by `PUSH FS:[..]` is a try frame's handler and is left to that recovery.

- `DirectInputCreateW` and `DirectInputCreateEx`. A Unicode program asks for
  the W entry and, refused, runs with no DirectInput at all and reads its
  mouse some slower way. The W object is the A object remembering that
  `DIDEVICEINSTANCEW` carries its two names as 260 UTF-16 units each, at 40
  and 560, in an 1100-byte record - which `GetDeviceInfo` used to reject as
  too large. The Ex entry names the interface by IID; the W IIDs are the A
  ones plus one, and an unknown IID is E_NOINTERFACE.

- The guest's main stack is 8 MB, not 1 MB. Siege of Avalon's 1.19 launcher
  exhausted a megabyte on Play even though the executable reserves only a
  megabyte on Windows: the same code runs deeper under the kit than
  natively, for a reason not yet established, so the guest gets headroom
  rather than the program's own reserve. The region below the stack was
  unused; worker stacks come from the heap.

- Every import the Siege of Avalon 1.19 image names now has an argument
  count: `SafeArrayAccessData`, `SafeArrayUnaccessData`, `ValidateRect` and
  `GetUpdateRect` do what they say, `LoadCursorFromFile{A,W}` hand out a
  handle, and sixteen more are logging-only entries with the right stdcall
  count, from `CombineRgn` to `URLDownloadToFileW`. The runtime's coverage
  gate - every named import must have a count - is the check that would have
  caught the case-table bug before it cost anything, and it is green again.

- A raise the guest goes on to handle is logged at the verbose level with the
  frames it climbed out of. A language exception hides exactly that, and the
  run log used to show only the dialog that reported it.

- Three more shapes of the listing defect `--allow-unmodelled` exists for.
  A direct call whose literal target is not in the image at all becomes a
  trap at its own address; the jump-table pass gets the same tolerance the
  emit path has, so a table that cannot be decoded because one of its own
  instructions cannot be modelled no longer fails the build; and a literal
  dispatch target that no instruction boundary agrees with - a jump decoded
  out of padding, landing inside a real instruction - becomes the trap a
  withdrawn block gets. Without the switch each one still fails the build.

- A SAFEARRAY may have more than one dimension. `SafeArrayCreate` refused
  anything else and the header it allocated was a fixed 24 bytes, so a guest
  that wrote `array[x, y]` got a null array back and had to abandon whatever
  it was building. The header is 16 bytes plus one bound per dimension, and
  creation, validation, copying, element addressing and the two bound queries
  all read `cDims` now. `rgsabound[i]` describes the dimension an index list
  names i'th, which is the order guests use.

- DirectDraw clips a blit that runs off its destination instead of refusing
  it. `Blt` and `BltFast` required the whole rectangle to lie inside the
  surface, so a draw that hung off an edge wrote nothing at all; real
  DirectDraw writes the part that lands, which a game relies on whenever it
  draws a scrolled buffer or a tile page at a border.

- Keyboard input goes to the window with the focus, as Windows sends it.
  Hosts posted it to the first window the guest created, which in a VCL
  application is the invisible application window: every keystroke went
  somewhere that does nothing with one, so a text field could not be typed
  into. The runtime knows which window holds the focus and now routes by it,
  falling back to the active window and then to the first.

- Translator: `--allow-unmodelled REASON` turns an instruction the translator
  cannot model into a trap at its own address instead of refusing the image,
  and reports every one. A listing routinely decodes the data past a
  function's last instruction as code - sixteen-bit addressing and port
  instructions in a thirty-two-bit user-mode image are the signature - and an
  image should not be refused over bytes nothing executes. Without the switch
  such an instruction still refuses the image, and reaching one at run time is
  fatal either way, loudly and with its address.

- An analysis pass no longer crashes a build on a body it cannot parse: a
  function whose instructions will not read is simply not a SEH helper, and
  the translation pass reports it the way it reports every other failure.

- Translator: the port string instructions (INS/OUTS, with and without REP)
  translate instead of failing the build. A user-mode guest never reaches one;
  they appear where a listing misdecodes data as code, and one such byte in a
  startup stub was enough to stop a whole image from translating. They read
  and write through the existing port shims and advance the pointer and count
  exactly as the other string forms do.

- Native overrides are reachable from a game: game.toml `[translate]
  overrides` names a header the generated sources include before they define
  FN_<addr>, so a game can replace one translated function with a native one
  and every call site, tail call and jump-table case for that address follows.
  The translator has emitted the hook since Task 8; nothing set it until now.
  A named header that does not exist is an error, because a path that quietly
  failed to resolve would leave a build looking replaced while running the
  original.

- Input: pending mouse moves are coalesced, as Windows does. The routing pump
  delivers one message per call, so a host reporting motion faster than the
  guest pumps built a backlog and the pointer trailed the hand by its length.
  A move between a press and a release is kept, so a drag is unaffected.

- DirectDraw recorder: what a frame retains is bounded. A frame ends when
  something is presented or drawn, so a screen built entirely from blits into
  a back buffer the guest never flips records into one frame indefinitely, and
  every source lease it takes can cost a full copy of those pixels. The oldest
  leases of an unsealed frame are now let go - nothing can have asked for them,
  since only a sealed frame is offered to a presenter - and the recorder keeps
  a bounded window of sealed frames, releasing anything older. This halves an
  observed growth of 15 MB a second on that kind of screen; the remainder is
  still under investigation.

- Diagnostics are bounded. A guest that generates code writes a new routine at
  a new address every time, so a report keyed by that address is a new key on
  every call: the once-only log now caps its key set, the undeliverable-call
  record caps its sample, and a guest thunk the decoder cannot run is reported
  once per SHAPE of code rather than once per address. The last of those was
  also thousands of formatted writes a second on a drawing path.

- Desktop host: mouse messages are routed by position, as the smoke host and
  Windows both do. They were posted to the main window carrying a screen
  position, so every click reached whichever window the guest created first -
  for a Delphi game, the invisible application window - and that window read
  the screen position as its own client one. A full-screen game never noticed;
  a windowed launcher could not be clicked at all.

- Input: a button press and its release are never applied to the guest in the
  same turn. Queued input arrives in batches, so a real click landed as a
  press and a release between two of the guest's polls, and a guest that reads
  its button state rather than the message queue never saw the button down at
  all. The batch is cut before the release and the rest waits a turn.

- Windows: child windows are composited with their parents, clipped to every
  ancestor. Only top-level windows reached the screen before, so a control
  that paints into its own window - which is most of them - was invisible.

- Smoke host: RECOMP_SMOKE_WINDOW_INPUT routes every scripted pointer step
  through the window mapping a real mouse uses, so a host-side input defect
  can be reproduced without a hand on the mouse.

- GDI: blits convert between colour and monochrome instead of matching the
  nearest palette entry. Into a 1-bit bitmap the source's background colour
  becomes white and everything else black; out of one, white takes the
  destination's background colour and black its text colour. That conversion
  builds and uses every transparency mask, so without it a mask came out as a
  luminance map and a transparent draw kept the wrong half of the image.

- user32: SetLayeredWindowAttributes and GetLayeredWindowAttributes. The
  window compositor drops the colour key and applies the constant alpha, so a
  shaped form is drawn as its artwork rather than as a rectangle of the key
  colour.

- Host: the system pointer stays visible until the guest has a display surface
  of its own. While it is still showing plain windows it draws no cursor, so
  hiding the host one left nothing to aim with.

- GDI: MaskBlt honours its mask bitmap instead of refusing every call that
  supplies one. A set mask bit takes the foreground raster operation and a
  clear one the background, which is how the VCL draws a transparent bitmap;
  refusing it lost whole window backgrounds, not single blits.

- Scheduler: `ExitProcess` on the main thread ends every other guest thread.
  No worker runs guest code again, so a host no longer crashes in one running
  on state the guest has already torn down; each is offered the baton once, to
  end, so a host's shutdown drive finishes instead of waiting out its bound.

- Auxiliary guest modules: game.toml `[modules.aux.<key>]` names a DLL the
  runtime maps beside the image at its preferred base (content-hashed, no
  relocation) with `[game] guest_size` growing the arena to hold it.
  `translate.py --module <key>` translates it into `gen/aux-<key>/` with
  prefixed, self-registering tables; the image's dispatch falls back to the
  module registry. LoadLibrary hands out the module's base and runs its
  entry point once, GetProcAddress answers from its export directory, and
  the bundle keeps the DLL regardless of `*.dll` exclusions.

- SEH: adopt registrations left installed by returning compiler helpers
  into a checkpoint in their live caller, including POP/JMP return helpers
  and helpers that fill caller-reserved stack records.

- DirectDraw: expose ANSI and Unicode legacy/extended device enumeration,
  reporting the primary display through callbacks with the correct ABI.

- Translator: recognize POP restores of FS:[0] through a proven zero
  register, including unlink helpers that do not establish their own frame.

- SEH: at verbose logging level, validate the guest chain at import and SEH
  boundaries and report its first invalid link together with the last valid
  observation. The diagnostic is bounded and does not alter guest memory.

- DirectDraw: expose DirectDrawCreateEx with an explicit unsupported result
  for IDirectDraw7, allowing callers to fall back to the legacy factory and
  its supported interfaces without receiving an incompatible vtable.

- Runtime: theme and desktop-composition probes report disabled visual
  styles and composition, allowing callers to use their classic window path.

- Runtime: buffered-paint initialization reports E_NOTIMPL through the
  uxtheme export, with safe cleanup for callers using ordinary GDI painting.

- Runtime: WTS session notification exports report an unavailable session
  service through their normal BOOL/error result instead of a missing DLL.

- Kernel32: GetNativeSystemInfo reports the same 32-bit guest system
  information as GetSystemInfo for delay-loaded platform probes.

- Kernel32: expose en-US thread, user and system preferred UI languages,
  with UTF-16 multi-string size queries and bounded writes for MUI callers.

- Runtime: GetModuleHandleA/W exposes registered DLLs before LoadLibrary,
  sharing stable pseudo-module handles with later loads and GetProcAddress.

- Runtime: configure the reported Windows version with
  `[game] windows_version = "major.minor[.build]"`. GetVersion,
  GetVersionExA/W and VerifyVersionInfoW share that identity. The default
  remains 4.10 (build 2222); 6.1 defaults to build 7601 and Service Pack 1.

- Build: refresh the generated directory's runtime header on incremental
  builds, without regenerating translated sources or touching unchanged files.

- Runtime: RET into a resolved import shim executes the target and its
  normal return, preserving Delphi delay-load calls on their first use.

- USER32: SetWindowPos sends WM_WINDOWPOSCHANGED synchronously, with
  WM_MOVE and WM_SIZE delivered by DefWindowProc, so successive dimension
  changes see the window procedure's updated bounds.

- Smoke mouse input follows visible, enabled windows in stacking order,
  delivers client coordinates and modifier flags, respects capture, and
  asks inactive windows to activate before a button press.

- Smoke: screen metrics, retained GDI captures and pointer bounds share the
  virtual desktop selected by RECOMP_SMOKE_DRAWABLE until DirectDraw sets
  a mode. Script dumps and pointer coordinates assert matching dimensions.

- GDI: DrawTextW and DrawTextExW now paint bitmap-font glyphs with the DC's
  colours, rectangle clipping and basic text layout. Measurement uses the
  selected font, including screen DCs without a backing surface.

- Translator: RET follows pushed interior continuations after cleanup, with
  shared Delphi finally blocks and their epilogues kept in the establishing
  body and exposed through alternate entries for exception dispatch.

- Translator: computed jumps to non-entry CALL continuations return to the
  pending host caller without dispatching again or popping the guest stack.

- Translator: recognize closed shutdown loops as nonreturning so recovery
  stops at their calls before decoding trailing data as instructions.

- Translator: follow adjacent `PUSH imm32; RET` continuations before returning
  to the host caller, while preserving direct entry at a shared `RET`.

- Merge integration: profile tests select the first generated entry and are
  omitted when none exists; ANSI and wide disk-space queries share the same
  virtual disk geometry.

- The web: Direct3D 9 renders on WebGPU (`host/gpu/webgpu/d3d9_webgpu.cpp`,
  WGSL from the shared generator, `dx/d3d9_wgsl.h`) over a `gpu::Device` for
  the browser (`host/gpu/webgpu/webgpu_device.cpp`). The game runs on a
  worker and reads its files from the browser's private storage; the main
  thread drains the render queue as soon as work is handed over, not once
  per animation frame (which held a race to 20 fps). `tools/build.py --target
  web` builds the app and a servable site with the launcher and the player
  page (`web/player/index.html`); `tools/web_launcher.py --web-build` places a
  build and `--serve` serves a site with the COOP/COEP headers it needs. A
  drain is asked for with a timeout, never a proxied call: the main thread runs
  proxied calls wherever it happens to be blocked, including inside the WebGPU
  binding's own lock, which drawing would take again.
- Core mods are compiled into the app where plugins cannot be loaded (iOS,
  the web): `cmake/BuiltinMods.cmake` renames each mod's entry points and
  lists them in `recomp_builtin_mods`, which the loader consults by the
  plugin's stem before opening a file. The iOS bundle carries the manifests.
- Cross builds: `windows-cross` presets over llvm-mingw
  (`cmake/toolchains/llvm-mingw.cmake`), `web` presets over Emscripten, each
  with its own output directory beside the shared translation. A developer
  run finds its mods and state files in its own `recomp/` directory.
  `build_core.py` builds plugins for the compiler's target, not the host.
- `RECOMP_SMOKE_SECONDS` raises the smoke host's 180-second limit for slow
  (software) renderers.
- The launcher offers only folders holding the whole game: a folder with the
  executable but without every `[setup] required_dirs` entry (an unpacked
  patch in Downloads, say) is no longer listed.
- Direct3D 9 renders on Vulkan as well as Metal: `host/gpu/d3d9_host.cpp`
  owns the `host_d9_*` entry points and the render thread and drives a
  `D9Backend` (`host/gpu/d3d9_backend.h`) made from the host's GPU device.
  `host/gpu/vulkan/d3d9_vulkan.cpp` records dynamic-rendering passes, compiles
  the shaders' GLSL (`dx/d3d9_glsl.h`) to SPIR-V with glslang (fetched, 16.6.0)
  and keeps every image in GENERAL layout. The MSL and GLSL come from one
  generator, `dx/d3d9_shadergen.cpp`; its MSL output is unchanged.
- Direct3D 9: the half-pixel correction moves geometry right and down (by
  63/64 of half a pixel, as Wine does). It moved left and up, which put
  post-processing one texel off and left the last column and row of a
  full-target pass undrawn.
- Direct3D 9 on Metal: multisampled surfaces and back buffers, a render thread,
  and fewer copies and state changes per draw. `game.toml [translate] native`
  names C replacements for hot guest functions.
- The desktop app accepts `--launcher`; its argument check printed the usage
  and exited before the launcher could read the flag.
- Translator: MMX, SSE and SSE2 instructions become a `recomp_unmodelled`
  trap rather than a translation failure, since `recomp_cpuid` advertises
  none of those extensions and a guest that checks CPUID never reaches one.
  The check runs before the string instructions, because `MOVSD` and `CMPSD`
  name both a string instruction and an SSE2 scalar-double one and only the
  operands tell them apart.
- Translator: `XADD`, `CMPXCHG`, `LAHF`, the x87 constant loads (`FLDLN2`,
  `FLDL2E`, `FLDLG2`, `FLDL2T`) and the x87 environment ops `FNSTENV` and
  `FLDENV`, each covered by a Unicorn differential case.
- Translator: `INT3` ends a block. MSVC pads between functions with it, and a
  listing whose tail is a call that never returns otherwise ran that padding
  into the next function. An unhandled breakpoint ends the process on
  Windows; `recomp_breakpoint` reports the address and stops.
- Runtime: `GetModuleHandleA` hands out the pseudo handle for a DLL the
  runtime serves instead of reporting it missing, so a guest that asks before
  loading anything gets a handle it can pass to `GetProcAddress`. The MSVC
  CRT's `__mtinit` does exactly that and treats a null handle as "skip the
  whole block", which left it calling a TLS function pointer it never filled.
- Direct3D 9 (`dx/d3d9.cpp`): the factory object, the adapter and format
  queries a game asks before it commits to a device, and CreateDevice. The
  device's vtable is complete and in interface order, because a guest calls
  these by slot index; a handful of methods do something and the rest report
  themselves once and return D3D_OK. Nothing is rasterized yet.
- D3DX 9 effects are real: compiled fx_2_0 effects are parsed from the
  executable's own RCDATA (a null module is the executable), with parameter,
  technique and pass handles, typed parameter storage, real descriptions, and
  passes that bind their shader bytecode, fill constant registers through each
  shader's CTAB and resolve every sampler's texture. Preshaded render states
  are not evaluated yet.
- A CPU renderer for Direct3D 9 (`dx/d3d9_raster.cpp`): SM 1.x/2.0 bytecode
  interpreted without flow control, perspective-correct rasterization into
  32-bit targets, and texture sampling in the usual 8-, 16- and 32-bit formats
  and DXT1/3/5. Textures get a real mip chain, formats are kept per surface,
  and the viewport is honoured.
- DirectInput 8: DirectInput8Create and the version 8 interfaces, on the
  existing mouse and keyboard devices.
- Direct3D 9 reports its first 24 clears, copies and every present: colour,
  target and whether it is the back buffer, and how much of the presented
  frame is lit. A black frame is otherwise indistinguishable from a broken
  presentation path.
- Presentation accepts 32-bit X8R8G8B8 frames (`host_present_expand_xrgb8888`,
  tested beside the 5-6-5 expansion) in the app and smoke hosts. Direct3D 9
  Present hands the device's back buffer to it, Clear fills 32-bit targets,
  and StretchRect copies between them. The back buffer now has its own field:
  it used to share render_target, which SetRenderTarget overwrites. The
  DirectDraw mode menu still offers only 8 and 16 bpp, deliberately.
- Direct3D 9 draws report what they were asked to draw: primitive kind and
  count, vertex stride, and where the vertices are. The two user-pointer forms
  were bare stubs, so the geometry a renderer needs was not merely
  unrasterized, it was unrecorded.
- Direct3D 9: IDirect3DTexture9::LockRect takes five dwords with `this` and
  the cube form six; both were declared one short, so a caller's stack drifted
  between locking a texture and unlocking it and the unlock went through a
  wrong stack slot. With the counts right, the game uploads its textures:
  330 locks matched by 330 unlocks, and no unresolved calls anywhere.
- Direct3D 9: a texture level or cube face references the texture it is a
  view into, as the real interface does, and GetContainer reports it. A game
  may take face zero, release the texture, and go on using it for the other
  five faces; without the container reference that release destroyed it and
  the next face came back through a dead pointer.
- Direct3D 9 textures: GetCubeMapSurface returns a real surface per face,
  kept on the texture so the same face comes back each time; GetLevelDesc
  fills its structure; and GetLevelCount returns 1. That last one returns a
  count rather than an HRESULT, so the stub's D3D_OK told callers a texture
  had no levels at all. A method that reports success without writing what
  the caller asked for is worse than one that fails.
- Direct3D 9 and D3DX 9 pop counts: IDirect3DDevice9::CreateTexture takes
  nine dwords with `this`, CreateVolumeTexture ten, ProcessVertices seven and
  ID3DXEffect::GetParameterBySemantic three. Each was one short, so the
  trampoline left the guest stack four bytes high and the caller returned into
  rubbish; the game died at EIP zero straight after CreateTexture. A vtable
  slot's argument count is part of the interface, not a detail.
- Direct3D 9 resources: textures, cube textures, surfaces, vertex and index
  buffers, vertex declarations and queries. Each is a real object over real
  guest memory, so Lock hands the game storage it can fill and the contents
  survive; the device keeps its own back buffer and depth buffer and answers
  GetBackBuffer and GetDepthStencilSurface with them. Draws are counted, not
  rasterized.
- D3DX 9 (`dx/d3dx9.cpp`): the matrix and vector maths for real (multiply,
  inverse, transpose, the left-handed projections, translation and the vector
  transforms), plus an effect pool and effects loaded from a game's own
  resources. Effects are accepted and not compiled, so what was a crash on a
  null interface becomes a list of the methods a shader-era game really uses.
- User32: RegisterClassExA registers the class the Ex structure describes, so
  a game that uses it gets a window instead of a silent CreateWindowExA
  failure; AdjustWindowRect joins its Ex form. Kernel32: GlobalMemoryStatusEx
  reports the same machine GlobalMemoryStatus does, in 64-bit fields.
- Kernel32: system and file time conversion, process id, `DuplicateHandle`,
  `SleepEx`, waitable timers, priority and affinity, toolhelp snapshots that
  report no processes, and `IsDebuggerPresent`.
- Imports: stdcall pop counts for the unshimmed exports of `d3d9`,
  `d3dx9_26`, `DINPUT8`, `SHFOLDER`, `WINMM`'s wave families, `WS2_32`,
  `NETAPI32` and the remaining `USER32`, `GDI32` and `KERNEL32` gaps. A call
  the runtime answers with zero now leaves the guest stack where the callee
  would have; without a count the stack drifted and a later return landed in
  rubbish.
- runtime_tests: the "no shims for it" case names a module no table mentions.
  `ddraw.dll` gained arity-only entries, so the runtime does serve it now.
- Bink: close any movie left open at guest exit before host audio teardown.
  Release decoder state, audio channels and guest records in the shared
  smoke, headless and SDL host shutdown path, preventing a process-exit abort.

- Bink: start and refill audio from DoFrame, NextFrame and Wait, so movies
  have sound when the game never calls the optional BinkService helper.
  All four entry points share the same audio routine and paused guard.

- Bink: serve the DirectSound token, decoded-frame rectangles and pause
  entry points. Pausing holds the frame clock and stops audio refills;
  resuming shifts frame deadlines by the paused interval. Builds without
  FFmpeg expose the same entry points with finished-video behavior.
  LoadLibrary now accepts any DLL with registered shims, case-insensitively,
  so dynamically loaded video imports resolve through GetProcAddress.

- Bink: open a video from a guest file handle at its current offset using
  FFmpeg custom I/O over the remainder of the host file. The player reopens
  the file read-only and owns its I/O context, leaving the guest's position
  and descriptor intact. Reject memory-resident video with a readable error;
  log and ignore other open flags. Cover decoding from an optional private
  container selected by `RECOMP_TEST_BINK_CONTAINER=<host path>,<offset>`.

- DirectDraw: releasing the object that set the display mode, or
  RestoreDisplayMode, puts the desktop back, so GetSystemMetrics and
  GetDeviceCaps report the desktop fallback until the next SetDisplayMode.
  A game that changes resolution by releasing and re-creating DirectDraw reads
  its screen bounds in between; the stale previous mode had every pointer
  position past the old width or height count as a screen edge, which
  scrolled the map whenever the pointer rested there. GetSystemMetrics logs
  the screen sizes it reports at the verbose level.

- Smoke: add `tap x y`, driving a stationary finger through TouchMapper and
  the window input gates with the smoke host's clock and presented frames.
  Cover parsing and the mapper-driven finger lifecycle.

- Touch: place a tap's pointer first, then press after one presented frame
  (60 ms when presented-frame counts are unavailable), so a game can sample
  the new cursor position before handling the click. Start the existing
  90 ms / two-frame release hold at the actual press. A new finger finishes
  a pending tap's press and release before starting the next gesture; focus
  loss drops an unissued press. Drag and edge-hold behavior is unchanged.
  Mapper and host suites pass; device menu/minimap confirmation is pending.


- Build: default FFmpeg ON on Linux, import its major-version shared objects
  and package them beside the executable with an `$ORIGIN` rpath and notice.
  On Windows, detect MSYS2 bash/make and require a MinGW-compatible compiler;
  keep video OFF with a status message when prerequisites are missing or
  the compiler uses the MSVC ABI. Package enabled builds' DLLs and notice;
  remove staged video files on OFF without touching player files. Windows
  CI stays video OFF and Linux needs no new packages. macOS configure and
  fake-file staging checks pass; Linux/Windows builds and playback are unverified.

- Build: enable shared FFmpeg by default on iOS and Android. Cross-build
  arm64 iOS 17 dylibs with relative install names and use Xcode's Embed
  Frameworks phase to copy and sign them with the app's identity/team.
  Cross-build Android API-29 libraries and package the three unversioned
  `.so` files beside `libmain.so`; remove staged copies when video is OFF.
  Include the FFmpeg notice in mobile bundles and document all configure
  flags. Android stub/real APKs and the standalone iOS FFmpeg build pass;
  embedded iOS signatures and mobile device playback remain unverified.

- Bink: decode video through FFmpeg into the DirectDraw surface supplied by
  the game, converting YUV420P to RGB565, RGB555 or XRGB8888. Stream decoded
  audio through the shared mixer, pace frames with host time and return an
  empty error string on success. Pending host close ends the video loop;
  builds without FFmpeg keep the finished-video stub and Smacker stays refused.

- Build: fetch SHA-256-pinned FFmpeg 7.1.1 for macOS with only Bink/Smacker
  decoders and demuxers and file input. `RECOMP_VIDEO` defaults to ON on
  macOS and OFF elsewhere; OFF retains the build without FFmpeg. Link
  avformat, avcodec and avutil dynamically, bundle the three replaceable
  dylibs with relative install names and ad-hoc signatures, and ship the
  LGPL notice, source identity and build flags.

- Touch: taps press and release at the finger's position, including near a
  window edge, without a cursor nudge after release. Held fingers and drags
  retain edge snapping so holding an edge still scrolls; lifting an edge
  hold moves the cursor back inside to stop scrolling.

- Load saved host settings before symbol-table validation, so window mode and
  other profile settings survive relaunch even without a usable symbol table.
  Hide renderer and native Options rows from the fallback settings page until
  symbols are available; retain window mode, frame limit and performance overlay.

- SDL: confine a captured pointer in a plain window as well as borderless
  and fullscreen modes. Click inside to capture, hold Escape to release,
  and drag into the window's resize margin to release capture for resizing.

- Android reads game data from SDL's external files directory under `game/`,
  reads `switches.txt` beside it, and defaults to a writable `profile/` there.
  Missing data logs the expected executable and `adb push` command, then exits.
  Enable fullscreen touch/keypad behavior and background audio/presenter
  suspension; end the process after SDL teardown when the game exits.
  `--push-game` stages the configured install using `[bundle].exclude`, then
  pushes before launch, and fails clearly without a ready Android device.

- Build `--target android` through the NDK preset, then package its
  `libmain.so` with an SDLActivity subclass and the matching FetchContent
  Java sources. Add a Gradle 9.7.1 wrapper and AGP 9.1.1 templates for
  arm64-v8a, API 29 minimum, compile/target SDK 36 and required Vulkan 1.1.
  Install and launch on a ready adb device, stream logcat with `--console`,
  and skip device actions when none is attached. Stub APK packaging is
  verified on macOS; Android device execution remains unverified.

- Add `android` and `android-stub` NDK presets for arm64-v8a, API 29 and
  static libc++. Build the SDL host as `libmain.so` with static SDL3 and
  NDK Vulkan/log libraries, omit desktop tests and add an Android stub CI
  build. Read arm64 Linux/Android page-fault writes from the kernel's ESR
  signal-frame record. APK packaging and device execution are not implemented here.

- Package successful desktop app builds under `build/package`: a Linux
  folder and architecture-named tarball, or a Windows folder. Include the
  kit notices, display-mode baseline, available translation symbol index
  and launch instructions using `RECOMP_EXE`; exclude original game files.
- Build: allow the app, smoke and headless hosts on Linux and Windows,
  including translation with `--regenerate`. Only the iOS packager requires
  macOS, including stub builds.
- Miles streams decode MP3 through the decoder shared with DirectShow.
  Streams refill from the guest frame pump, support volume and loop counts,
  and remain playing until queued PCM drains.
- USER32: queue `WM_MOVE` and `WM_SIZE` after window creation and the
  corresponding `SetWindowPos` operations, plus `WM_SIZE` on the first show.
  Screen and fullscreen metrics follow the accepted DirectDraw display mode,
  retaining the caption-height deduction and 1024x768 fallback without a mode.
  Games can now size their fullscreen blit rectangles from window messages.
- DirectDraw: writes through a writable pointer retained after `Unlock` reach
  the renderer before `Blt`, `BltFast` and primary presentation. Whole-rectangle
  hashes detect the writes and share the written-lock CPU recording path;
  surfaces never locked writable keep their existing path without hashing.
- Bink: `BinkOpen` returns a 256-byte guest heap record with 640x480
  dimensions and zero frame counters, so a game skips an unavailable
  cinematic instead of treating an open failure as fatal. `BinkClose`
  frees the record; decoding and waiting remain no-ops, and `SmackOpen`
  still returns 0. No video decoder is included.
- GDI: `GetDeviceCaps` reports the accepted DirectDraw mode and depth-dependent
  palette capabilities, falling back to 640x480x8 before a mode is set.
  `GetTextExtentPointA` shares the fixed 7-pixel width and 16-pixel height of
  `GetTextMetricsA`; `SetBkColor` stores each DC's background color and returns
  its previous value, initially white. Text output remains undrawn.
- Build: define `profile_tests` only when the translation's `funcs.h` defines
  its target function `FN_00500040`, so other translations can build all
  native test binaries without that game-specific suite.
- Runtime: optional `[game] heap_base` sets the heap arena start through the
  generated config and build definitions, so images ending above 16 MB can
  load. The default remains `0x01000000`; the value must be page aligned,
  above `0x00400000` and below `0x0e000000`. A rejected image now reports
  both its end and the heap start, with the setting to raise.
- Build: `tools/build.py --regenerate --allow-table-gaps "<reason>"` passes
  the waiver and its reason to the translator. Omitting the flag keeps
  jump-table gap checks unchanged.
- Touch: a tap's synthesized click now stays pressed until the game has
  presented two frames after the press (`TouchMapper::frames_presented`, fed
  by the SDL host from the present count), as well as for the 90 ms it
  already held. A game that samples its buttons with `GetKeyState` once per
  frame and presents at 15 frames a second could miss a clock-timed press and
  release altogether; on a device that made most taps land nowhere. A game
  that stops presenting still gets its release after 400 ms.
- Touch: the edge-snap margin grows by the window's safe-area inset on each
  edge (`TouchMapper::set_edge_insets`, from `SDL_GetWindowSafeArea`). A
  finger on a tablet's top bezel arrives no closer than the status bar's far
  side, some 32 points down, so the 16-point margin never saw it and the
  game's top-edge scroll never started.
- Touch: a synthesized click's release carries the press position again. The
  mapper read the placed point back out of the action vector after pushing the
  press into it, past a reallocation; on a device the release then landed at
  0,0, so a tap pressed one button and released on another (nothing happened)
  and a game that scrolls at its edges flew to its top-left corner.
  `input_touch_tests` now checks the release position across vector capacities.
- Touch: a drag the system cancels (an edge gesture it claims) releases its
  button where the cursor was placed, not at 0,0. The release carried the
  origin as its position, the host moved the game's cursor there, and a game
  that scrolls at its edges flew to its top-left corner.
- A hardware pointer resting against a system strip (a tablet's status bar,
  which the pointer cannot enter) is placed on the edge behind it
  (`pointer_behind_strip`, with 16 points of slack for a hand pushing against
  the strip), and stays there until the pointer has come 64 points away
  (`PointerStripLatch`): iPadOS glides a pointer that touched the strip some
  30 points back down on its own, which would have left the edge after an
  instant. A game's top-edge scroll, which needs the cursor to stay put while
  it ramps up, works from a mouse on a tablet.
- The iPad host ends the process when the game exits (`ExitProcess`,
  `platform_ui_process_exit`); before, the game was gone and the app stayed
  on screen showing its last frame.
- Switches from a file: `recomp_env_apply_file` reads NAME=VALUE lines, and
  the iPad host applies `Documents/switches.txt` at start, so RECOMP_* switches
  (the pointer trace, a pinned clock) reach a device that has no shell. Put the
  file there with `xcrun devicectl device copy to ... --domain-type
  appDataContainer --domain-identifier <bundle id> --destination
  Documents/switches.txt`.
- Smoke scripts: `button <left|right|middle> <down|up>` presses or releases a
  mouse button where the pointer is and leaves it, so the moves in between
  are a drag; `click` remains a press and a scheduled release.
- VERSION.dll serves the executable's own version resource out of the mapped
  image: `GetFileVersionInfoSizeA`/`GetFileVersionInfoA` for the game's module
  name, `VerQueryValueA` for `\`, `\VarFileInfo\Translation` and
  `\StringFileInfo\<lang>\<name>` (strings narrowed in place for the A
  caller). Any other file still has none. A game's "Version" label fills in.
- DirectShow: `IFilterGraph::EnumFilters` returns an enumerator over the
  graph's (empty) filter list instead of E_NOTIMPL, so a game that lists its
  filters for its log walks nothing rather than logging a failure.
- The runtime's log lines are tagged `[recomp]`, not with a game's initials.
- File seam: a handle opened for writing on an existing file goes through the
  read tier and is promoted to the write tier by its first WriteFile or
  SetEndOfFile, continuing at the offset it had reached. Classifying the open
  itself as a write copied every archive a game opens read/write into the
  profile (one game: 590 MB per fresh profile, and a copy the watchdog cut
  short then hung the next run). A create or truncation is still a write from
  the start. `runtime_tests` covers the promotion against the seam fixture.
- Mods loader: the overlay - the profile as the writable tier - is installed
  before the symbol table is loaded and before RECOMP_NO_MODS is honoured, so a
  port with no symbol table or mods still keeps its saves, settings and logs
  out of the game's own installation. The run record creates its directory.
- DirectShow multimedia streaming, the reading side (`dx/dshow.cpp`): a game
  that plays its MP3 music through `CoCreateInstance(CLSID_AMMultiMediaStream)`
  gets IAMMultiMediaStream over the file, decoded with minimp3
  (`third_party/minimp3`, CC0). Both ways a game drives it are served: pulling
  samples (IAudioMediaStream, AMAudioData, IAudioStreamSample::Update filling
  the caller's buffer and signalling its event, MS_S_ENDOFSTREAM, Seek) and
  driving the filter graph (IGraphBuilder from GetFilterGraph, IMediaControl
  Run/Pause/Stop, IMediaEventEx with a real completion event and EC_COMPLETE,
  IMediaSeeking, IMediaPosition, IBasicAudio), where the kit streams the PCM to
  a host audio channel from the frame pump. `dx_tests` covers both against an
  MP3 tone kept as a header (`dx/tests/fixtures/tone_mp3.h`).
- COM classes register: `com_register_class` names a CLSID, a constructor and
  the interface IID_IUnknown gets, and one `CoCreateInstance` in `com.cpp`
  serves every registered class (DirectSound moved onto it); an unregistered
  class is still REGDB_E_CLASSNOTREG. `win32_create_event` and
  `win32_reset_event` let a shim own a kernel event on the guest's behalf.
- Split on-screen keypad for touch: two 8x5 halves in the bottom corners, three
  sizes, Shift/Ctrl/Alt that hold, latch or lock, HIDE/KEYS tabs, settings on the
  F10 page persisted per game; replaces the eight-key strip. `RECOMP_KEYPAD=1` forces
  it on for a desktop check.
- Switches: every environment switch the kit reads is `RECOMP_<NAME>`, read
  through one function, `recomp_env` in `platform/os.h`. The spellings from the
  kit's origin as one game's port - `POPM_<NAME>`, `POP_RECOMP_<NAME>`,
  `POP_HOST_<NAME>`, `POP_SMOKE_<NAME>`, `POP_GPU_<NAME>`, `POP_VULKAN_<NAME>`,
  `POP_REPLACE_<NAME>`, `POP_PLATFORM_TEST`, `POP_TEST_DIR`, `POP_CC`,
  `POP_BUILD_ROOT` and the rest - are gone, not aliased: `POPM_PIN_CLOCK` and
  `POP_RECOMP_PIN_CLOCK` are both `RECOMP_PIN_CLOCK`, `POPM_TEST_DIR` (mods
  tests) is `RECOMP_TEST_DIR`, `POP_TEST_DIR` (platform tests) is
  `RECOMP_PLATFORM_TEST_DIR`, and the tools' `POP_BUILD_ROOT` is
  `RECOMP_BUILD_ROOT`. `tools/test.py` and the mode probe drop a caller's
  `RECOMP_*` switches from a child's environment while keeping the names that
  locate the game and toolchain (`mode_probe.without_switches`). Smoke scripts,
  test fixtures and docs use the new names; a game repository's scripts must
  too. Still to move: the CMake variables (`POP_ROOT`, `POP_BUILD_ROOT`,
  `POP_OUT`, `POP_WARN_STRICT`, ...) and the `POPM_TESTING` compile macro,
  which share the old prefix but are not read from the environment.
- Translator: instruction forms a Visual C++ 6 executable uses that the first
  corpus did not: `LOOP`, `INT3`, `CLC`/`STC`, `PUSHF`/`POPF`, 16-bit `PUSH`/`POP`,
  word- and dword-width `CMPS`/`SCAS` with every REP prefix, `FPTAN`,
  `FSAVE`/`FNSAVE`, `FRSTOR`, `FINIT`/`FNINIT`, and segment registers as a `MOV`
  source (the flat selectors) or destination (dropped; a CS load is `#UD`).
  `x86.h` gains `x87_finit`, `x87_fnsave`, `x87_frstor` and the wider string
  compares. `tools/recomp/tests/test_translate_insns.py` checks each form against
  Unicorn on synthetic listings without a game, and runs in `tools/test.py`.
- Translator: jump tables bounded the way that compiler's hand-written `memcpy`
  bounds them - a low-bit `AND` mask whose unreachable slot holds code, a guard
  that branches to the jump after `CMP idx,N` (0..N-1) or `SUB idx,N` (-N..-1),
  and a `NEG` of a bounded index - decode exactly instead of falling back to a
  forward read that found nothing or the wrong entries
  (`tools/recomp/tests/test_jumptables.py`).
- Translator: a pushed immediate that decodes as a thunk is an entry candidate:
  the CRT's `atexit` is handed ten-byte `MOV ECX,obj / JMP dtor` stubs that no
  function-start signal accepts, and called into nothing at exit without it.
- Runtime: `game_path_resolve` uses the absolute `RECOMP_DEVELOPER_EXE` from
  wherever the app runs, and a guest root of more than one component
  (`C:\GOG Games\<name>`) resolves in `normalise_components`, so a game
  repository's build finds and opens its own game without a dialog.
- Runtime: `runtime/gdi32.cpp`, a fourth shim table: DIB sections in guest
  memory, memory DCs, `GetObjectA`, colour tables, logical palettes, `BitBlt`
  and `PatBlt` between DIBs, `GetDIBits`, text accepted and not drawn.
- Runtime: boot-path shims with their argument counts: the CRT locale probes,
  `GetEnvironmentVariableA`, `GlobalMemoryStatus`, `SetErrorMode`,
  `GetLogicalDrives`, `SHGetSpecialFolderPathA` (per-user folders under the
  guest root), `IsWindowUnicode`, `GetSystemMetrics`, `LoadCursorA`,
  `CreateIconIndirect`/`DestroyIcon`, the mixer API (no driver),
  `mciGetErrorStringA`, `VERSION.dll` (no version resource).
- dx: `CoCreateInstance` for `CLSID_DirectSound` (every other class is
  `REGDB_E_CLASSNOTREG`) and `IDirectSound::Initialize` succeeds.
- Runtime: `POPM_GUEST_ARGS` appends switches to the command line the CRT reads
  through `GetCommandLineA`, so a game's own `-debugout` or `-nointro` can be
  passed. The RaiseException diagnostic also names the class of every object a
  register points at (through MSVC RTTI) and dumps the thrown object's dwords.
- user32: `ScreenToClient`, `GetActiveWindow`, `SetFocus`.
- Translator: a pushed immediate that decodes as a thunk, a call to a callee the
  listings show never returning (recovery stops there; the emitter leaves a trap),
  and a code pointer whose bytes happen to be printable are all handled; see the
  translator tests.
- Translator: a literal transfer to a block the sweep withdrew (padding after a
  call that never returns) becomes `recomp_unknown_call(c, addr); return;` rather
  than a dangling dispatch, so a listing that ends on a `throw` translates. The
  dispatch gate now names the functions that failed to translate before it
  reports what dispatched to them (`tools/recomp/tests/test_translate_driver.py`).
- Builds take `--game-dir`: a game directory anywhere, with outputs under its own
  `build/`; paths in `game.toml` resolve from its directory. The kit ships `games/stub`
  for game-free builds and CI. Populous moves to github.com/veritr1x/populous-recomp,
  which pulls the kit in as a submodule; its smoke scripts, release notes, game docs and
  game-bound tests go with it.
- Imported the Populous recompilation from populous-recomp-checkout at
  b450dfa8bae7fe568192ef61028ed82489cba394 as the base of recomp-kit. The
  translation is no longer tracked; regenerate it with tools/build.py --regenerate.
- Kit layout: runtime/, dx/, host/, platform/, mods/ at the top level; games/populous/
  holds the game config, curated globals, plugins and artwork.
- games/<id>/game.toml drives identity, addresses and translator inputs through a
  generated game_config.h; tools/check_game_literals.py keeps game literals out of kit code.
- tools/build.py --stub and the *-stub CMake presets link the hosts without game code.
- iOS: `tools/build.py --target ios` builds, signs and installs the app on a paired iPad;
  touch mapper, fullscreen Metal window, game files seeded into Documents on first launch.
- iOS: on-screen key bar, taps that place the game's cursor and hold the click, app icon
  from the game executable, lifecycle-driven suspend of audio and presentation.
- iOS acceptance fixes: the key bar hides to a KEYS tab; a long press then lift is a
  right click and a long press then drag holds the wheel button (Populous scrolls and
  rotates with it; the game has no right-drag camera); a finger resting on a screen
  edge scrolls; settings persist (the translation's symbol table ships in the bundle
  so the mod settings store initialises); touch is cancelled cleanly on focus loss.
- Pointer hit test: the first and last three drawable pixels count as the scrolling
  edge, so an iPadOS trackpad pointer (which stops half a point short of the left
  edge) and the window's last point reach the row or column the game scrolls from.
- A GPU surface read refused around a background/foreground transition is retried
  for up to a second instead of aborting the game; the Metal device reports the first
  failed command buffer's error.
- A routine a game builds in its heap and calls can no longer hang the game.
  The interpreter that runs such code gives each call a budget of instructions
  and stops a routine that never reaches its RET - code that jumps to itself, a
  loop over a counter the guest left as garbage - the way it stops one that
  reads outside guest memory: the call returns zero and the log says which
  routine and where.

- Build with CMake presets for macOS, Linux and Windows through the unchanged
  `tools/build.py` and `tools/test.py`; the xcrun shell scripts are gone.
- Add a platform layer (`src/recomp/platform/os.h`) so the runtime, adapters
  and mod foundation compile and pass their portable tests on Linux and Windows.
- Mod plugins resolve their file extension per platform; a manifest written on
  macOS loads its `.so` or `.dll` counterpart unchanged.
- CI compiles and tests the portable layers on macOS, Ubuntu and Windows.

## 2026-09-12 — Initial native source release

- Publish the macOS native runtime, static translator, Metal renderer and C/Lua
  mod API as a standalone contributor project.
- Provide verified local game setup, build/test commands, architecture and code guides.
- Include Enhanced rendering, widescreen projection, native FPS/frame-pacing
  overlay, live Options settings and one Graphics resolution selector through 4K.
- Preserve the fixes for animation timing, audio clock behavior, focus changes,
  pointer confinement, resolution cycling and settings persistence.
- Keep original game files, generated translations and local test artifacts out of Git.

Current validation boundaries and performance limits are recorded in [Testing](docs/testing.md).
