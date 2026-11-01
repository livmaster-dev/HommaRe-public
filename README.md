<!-- Describes the state of branch `main` (2026-10-28). -->

<p align="right"><b>English</b> · <a href="README.ru.md">Русский</a></p>

<h1 align="center">HommaRe</h1>

<p align="center">
An unofficial static recompilation of <b>Heroes of Might and Magic V</b> (2006), with
<b>Hammers of Fate</b> and <b>Tribes of the East</b>: a native port, not an emulator.<br>
Play your own GOG copy on Apple silicon Macs, iPad and iPhone.
</p>

<p align="center">
<a href="#download">Download</a> ·
<a href="#quick-start">Quick start</a> ·
<a href="#controls">Controls</a> ·
<a href="#faq-and-troubleshooting">FAQ</a> ·
<a href="../../issues">Report a bug</a>
</p>

<!-- Screenshot: the adventure map on a 21:9 ultrawide (Hor+ view, HUD at the edges). -->

> [!IMPORTANT]
> HommaRe is an unofficial fan project. It is not affiliated with, endorsed by or
> sponsored by Ubisoft, Nival or GOG.
>
> **No game files are included or distributed.** To play, you need your own legally
> obtained copy of the game from GOG. The launcher downloads it from your GOG.com library
> or installs it from GOG's own offline installers, and verifies every file. Please don't
> ask for, or share, game files.
> See the full [legal notice](#legal-notice).

**Contents:** [What it is](#what-is-hommare) · [What's new](#whats-new) · [Highlights](#highlights) ·
[Status](#status) · [Download](#download) · [Quick start](#quick-start) ·
[Controls](#controls) · [Settings](#settings) · [FAQ](#faq-and-troubleshooting) ·
[What's next](#whats-next) · [For developers](#for-developers) · [Credits](#credits) ·
[License](#license) · [Legal notice](#legal-notice)

## What is HommaRe?

HommaRe is a static recompilation of the Windows version of Heroes of Might and
Magic V: the game's own x86 code, translated ahead of time into C and compiled into an
arm64 app. There is no Wine, no CrossOver, no Rosetta and no emulator at run time: the
game runs as an ordinary Mac, iPad or iPhone app.

All three games are in one app: the original game (1.60), **Hammers of Fate** (2.1) and
**Tribes of the East** (3.1). You install them from your own GOG copy, pick one in the
launcher and play, with things the 2006 game couldn't do: ultrawide screens, touch and
trackpad controls, an interface made for the iPhone, and a frame rate that saves your
battery when nothing moves.

## What's new

**0.1-playable-421** (October 2026) · [all changes](CHANGELOG.md)

- **German, French and Polish** GOG releases: install them from GOG.com or their
  installers, and the native screens speak their language.
- **Hammers of Fate's local network games** no longer crash.
- **The original game's arithmetic on every device:** the expansions create games on
  ubi-gs. Network games need this build on every device.
- **More native screens:** Load and Save in network and hot-seat games, Save in the
  expansions, their LAN lobbies and duels.
- **Touch:** the tap-to-strike battle in Hammers of Fate and Tribes of the East, and a
  stack splits when you drag it and hold still.

## Highlights

- **Native on Apple silicon.** The game's code, and RAD's Granny 2 animation library
  with it, are recompiled for arm64. Direct3D 9 is drawn with Metal, and the models'
  skinning and lighting run on the GPU.
- **Three games, one app.** Heroes V, Hammers of Fate and Tribes of the East, each with
  its own profiles and saves. The launcher shows what is installed and starts the one you
  pick.
- **A launcher that looks like the game.** Its frames, buttons and backdrop are read from
  your installed game's own data at run time, so the app ships no game art. It has the
  ten languages of GOG's installers (English, Russian, Czech, German, Spanish, French,
  Italian, Hungarian, Polish and Simplified Chinese) and stands upright on an iPhone.
- **Downloads from your GOG.com library, on every device.** Sign in to GOG in the
  launcher and it downloads the games you own, in English, Russian, German, French or
  Polish, checks every file's SHA-256 and resumes a stopped download. No installer files and no Files app needed.
- **Installs from your GOG installers, on every device.** Give the launcher GOG's offline
  installer: the `.exe`, the `.bin`, the folder they are in, or several at once; on a Mac
  you can also drop them onto the window. It unpacks only what the game needs, checks
  every file's SHA-256 and can resume an interrupted install. GOG's update installers
  (`patch_*.exe`) are applied too, and a game you already installed is found as well.
- **Ultrawide support.** On 21:9 the adventure map and battles get a wider view instead
  of a stretched one, the interface moves to the screen edges, and dialogue scenes
  span the screen. *Interface bounds* keeps the HUD in a 21:9, 16:9 or 4:3 box on very
  Mac wide screen; screens, backgrounds and the 3D world still fill the whole display. In
  all three games.
- **Touch controls adapted for Heroes.** Heroes V explains everything on *hover*, so a
  finger at rest acts as a hovering mouse: hold to read a tooltip, tap to act. A second
  finger gives the right click; drag to pan, pinch to zoom, twist to turn the camera.
  The system keyboard comes up on a text field, and a drag scrolls a list.
- **Battles with a touch.** Tap an enemy and the cells you can strike it from light
  up; tap one to walk there and strike. The attack cursor shows beside the damage hint,
  as it does under a mouse.
- **An interface for the iPhone.** The round dial menus become rows of buttons along the
  screen edges, panels are larger, the hero list sits beside the minimap, the town's
  build and hire menus move to the corner, and everything stays clear of the Dynamic
  Island. The battle camera comes nearer, and glides back while you place your troops.
  The layout follows the window, so a foldable switches it as it folds.
- **Menus made for the screen.** On iPhone the main menu, profiles, campaigns, Load and
  Save, Options, duels, custom and hot-seat games and the in-game menu are drawn as
  native screens, built from the game's own pictures, words and frames, with text sized
  for a phone. iPad can have them too.
- **Local network games.** *Multiplayer → Local Network* finds the games on your Wi-Fi:
  host one, join one and play, between Macs, iPads and iPhones.
- **Trackpad on a Mac.** Two fingers over the adventure map raise and lower the camera,
  or move it like on an iPad: scroll to pan, pinch to zoom, twist to turn. In battle the
  camera zooms towards the pointer, and lists scroll under your fingers.
- **Smooth with options to preserver battery.** A 40 fps limit by default (60 or 120 if you like;
  ProMotion iPhones run at 120 Hz so the limit holds). After a minute without input the
  game drops to 30 fps until you touch the mouse, keyboard or screen. An optional energy
  mode lowers the frame limit and render scale in Low Power Mode or when the device is
  hot. A hidden window runs at 10 fps.
- **Sharp picture, light load.** *Graphics quality* draws the 3D at native resolution or
  smaller and upscales it with MetalFX, while the interface is always drawn at the
  screen's own resolution. On iPad and iPhone, *Auto* lowers the 3D's share as the device
  heats up. Shaders are compiled ahead of time and kept between runs to avoid stutter.

## Status

This is an early, active project. What works today:

| | Heroes V 1.60 | Hammers of Fate 2.1 | Tribes of the East 3.1 |
|---|---|---|---|
| Mac (Apple silicon) | Playable: campaigns, scenarios, hot-seat, battles, towns, saves | Playable: maps, battles, towns; less tested | Playable: maps, battles, towns; less tested |
| iPad | Playable with touch | Playable with touch; less tested | Playable with touch; less tested |
| iPhone | Playable with the phone interface | Phone interface, tried on an iPhone 15 Pro Max | Phone interface, tried on an iPhone 15 Pro Max |
| Ultrawide (21:9) | Hor+ view, HUD at the edges, interface bounds, wide dialogues | The same | The same |
| Mac trackpad | Map; battle zoom and pan | Map; battle zoom and pan | Map; battle zoom and pan |

Tested on a MacBook Pro (M1 Max, macOS 27) with a 3440x1440 ultrawide, an iPad Pro 13"
(M5) and an iPhone 15 Pro Max (iOS 27), mostly with the **Russian GOG releases** of the
game. The English GOG installers are recognised and installed too; other languages
haven't been tested yet: reports welcome.

## Download

> [!NOTE]
> **Mac:** a ready-made build is in [Releases](../../releases/latest)
> (`HommaRe-<version>-macOS.dmg`). It holds no game data: it downloads the game from
> your GOG.com library or installs it from your own GOG installers. macOS can't verify its developer, so it asks once on the first start (see
> [Quick start](#quick-start)). **iPad and iPhone:** an unsigned IPA is in the same
> release (`HommaRe-<version>-iOS.ipa`); sideload it with Sideloadly, AltStore or
> SideStore, or build the app yourself on a Mac.

| Platform | How to get it |
|---|---|
| Mac (Apple silicon) | [Download the DMG](../../releases/latest), or [build it yourself](#building-from-source). |
| iPad and iPhone | [Sideload the IPA](../../releases/latest) with Sideloadly, AltStore or SideStore, or build it yourself on a Mac with Xcode. |
| Linux (x86-64) | [Build it yourself](#building-from-source); it draws with Vulkan. |
| Linux (64-bit ARM) | [Build it yourself](#building-from-source), on the ARM computer or cross-compiled on an x86-64 PC; Vulkan. For ARM Chromebooks, Snapdragon X laptops, Asahi Linux, Raspberry Pi 5. Builds; not yet tried on the hardware. |
| Windows, Android | Not planned for now: the original game already runs on Windows. |

### System requirements

- **Everyone:** your own copy of Heroes of Might and Magic V from GOG: *Heroes of Might
  and Magic V* (with Hammers of Fate) and/or *Heroes of Might and Magic V: Tribes of the
  East*. About 4 GB of free space for Heroes V with Hammers of Fate, and 2.2 GB more for
  Tribes of the East, plus room for the installers if you install from them.
- **Mac:** Apple silicon, macOS 14 Sonoma or later. There is no Intel Mac build.
- **iPad and iPhone:** an M-series iPad or a recent iPhone (tested on iPhone 15 Pro Max).
  The app asks iOS for more memory than most apps. The game plays in landscape.

## Quick start

### Mac

0. **Get HommaRe.** Download `HommaRe-<version>-macOS.dmg` from
   [Releases](../../releases/latest), open it and drag *HommaRe* to *Applications*.
   macOS can't verify the app's developer, so the first start fails: open *System Settings → Privacy &
   Security*, scroll down and press *Open Anyway* next to HommaRe. Later starts just work.
   (Or [build it yourself](#building-from-source).)
1. **Open HommaRe.** A game that isn't installed says *Not installed*. Press *Install from
   GOG…*.
2. **Sign in to GOG and download.** Sign in on GOG's own page in the panel that opens,
   choose the game's language and press *Download* next to each game you own. The
   launcher downloads it, checks every file and marks it ready. *Heroes of Might and Magic
   V* gives you both the original game and Hammers of Fate. A stopped download resumes
   where it stopped.
3. **Or use your installer files.** If you already have GOG's offline installers (one
   `.exe` and one or more `.bin` files for each game, and `patch_*.exe` if GOG offers
   one), press *Use installer files…* in the same panel and choose the `.exe`, the `.bin`
   or their folder, or drop them onto the window. If the game is already installed
   somewhere, choose *Game folder…* instead.
4. **Play.** Pick a game in the launcher and press *Play*. Your profiles, saves and
   settings are kept apart from the game files, so they survive updates and reinstalls.

### iPad and iPhone

First install the app: sideload `HommaRe-<version>-iOS.ipa` from
[Releases](../../releases/latest) with Sideloadly, AltStore or SideStore (they sign it
with your Apple ID; turn on Developer Mode on the device), or
[build it](#building-from-source) yourself.

1. **Open HommaRe**, press *Install from GOG…* and sign in to GOG. Choose the game's
   language and press *Download* next to each game you own: the launcher downloads it
   into the app and checks every file (2–4 GB per game, use Wi-Fi).
2. **Or use your GOG installers**: put the `.exe` and `.bin` files on the device in the
   Files app (on the device or in iCloud Drive) or with Finder from a Mac, press *Use
   installer files…* in the same panel and choose them. Afterwards you can delete the
   installers.
3. **Already have the game installed?** Copy its folder to the device instead and choose
   it with *Add game files…*.

> [!TIP]
> In *Single Player*, the Campaign, Scenarios and Hall of Fame items stay grey until a
> profile is chosen: open *Change Profile*, pick yours in the list and press *Select*. This
> is the original game's behaviour, not a bug of the port.

## Controls

### Mac

Mouse and keyboard work as in the original game: hover for information, left click to
act, right click to see details, the mouse wheel and the game's own keys for the camera.
Double clicks, Shift-drop to split a stack and Alt for map labels all work.

With a trackpad or Magic Mouse, two fingers over the 3D adventure map move the camera
(*Settings → Trackpad*):

| Mode | Two fingers over the map |
|---|---|
| Height (default) | Scroll up and down raises and lowers the camera, with momentum |
| Like an iPad | Scroll pans, pinch zooms, twist turns the camera |
| Game's own | Scrolling goes to the game, as a mouse wheel |

In battle, a pinch or scroll zooms the camera towards the pointer and two fingers pan
the field. Over a list, the content follows your fingers. Over a town and the rest of the
interface, scrolling goes to the game as a mouse wheel.

### iPad and iPhone

| Gesture | What it does |
|---|---|
| Touch and hold | Hover: shows the game's tooltips and highlights, without clicking |
| Tap | Left click (the cursor jumps to your finger first) |
| Double tap | Double click |
| Second finger tap, beside the resting one | Right click on the thing under the first finger |
| Two-finger tap | Right click between the fingers |
| One-finger drag on the map | Pan the camera |
| Drag up or down over a list | Scroll the list |
| Tap a text field | Bring up the keyboard |
| Pinch | Zoom |
| Two-finger twist | Turn the camera |

**Touch-friendly battle** (on by default): tap an enemy your stack can reach and the
cells it can strike from light up. Tap one to walk there and strike, or tap the enemy
again to strike from the nearest cell; a tap anywhere else cancels. Hold a lit cell and
lift to only walk there.

On a loading screen, tap once more when loading is done to start: the game waits for a
click there. The three intro videos can each be skipped with a tap, or turned off in the
settings.

## Settings

The launcher's *Settings* apply to every game. On first start each device gets its own
defaults, shown in bold:

| Setting | Choices | What it does |
|---|---|---|
| Language | As in the system, or one of ten | The launcher's language |
| Frame limit | Game's own, **40**, 60, 120 | The highest frame rate the game runs at |
| Energy saving | Auto, Always, **Never** | A lower frame limit and render scale; Auto turns it on in Low Power Mode or when the device is hot |
| 30 fps after a minute idle | Auto, **On**, Off | Drops to 30 fps after a minute without input, back at once on any input |
| Graphics quality | Auto, Native, Quality, Balanced, Performance | How large the 3D is drawn before MetalFX upscales it; the interface stays sharp. **Performance** on Mac and iPhone, **Balanced** on iPad |
| Interface bounds | Screen edges, 21:9, 16:9, 4:3 | On wide screens, keeps the HUD in a box instead of at the far edges (**16:9** on Mac) |
| Startup videos | Show, Skip | The company logos and intro at start |
| Metal HUD | Show in game (**off**) | Apple's performance overlay over the game, from the next start |
| Model animation | On the GPU (**on**) | Skins and lights the models on the GPU instead of the CPU |
| Trackpad *(Mac)* | **Height**, Like an iPad, Game's own | What two fingers do over the map |
| Camera buttons *(iPad, iPhone)* | Show, **Hide** | The on-screen camera panel |
| Touch-friendly battle *(iPad, iPhone)* | Off, **On** | Tap an enemy, then the cell to strike from |
| Silent mode *(iPad, iPhone)* | **Respect**, Ignore | Whether the silent switch mutes the game |
| Phone interface *(iPhone)* | **Auto**, Always, Never | Larger panels and button rows instead of dial menus; Auto chooses by the window's size, so a foldable switches as it folds |
| Game folders | | Extra places where the launcher looks for installed games |

The game's own Options menu (graphics quality, sound) works as in the original. On iPad
and iPhone the game always draws at the screen's size, so the screen resolution is left
out of its options: *Graphics quality* in the launcher takes its place.

## FAQ and troubleshooting

**Does HommaRe include the game?** No. No game files are included or distributed.
You install the game from your own GOG copy, and every file is checked.

**Which version do I need?** The GOG releases: *Heroes of Might and Magic V* 2.1 (which
contains the original game 1.60 and Hammers of Fate 2.1) and *Tribes of the East* 3.1,
in English, Russian, German, French or Polish. The launcher checks each game's executable
by its SHA-256 and says if it doesn't match. GOG's other languages, disc versions and other
stores haven't been tested.

**Is this an emulator? Does it use Wine?** No. It's a static recompilation: the game's
x86 code was translated ahead of time into C and compiled for arm64. A runtime written for
this port stands in for the parts of Windows the game uses.

**Can I install on the iPad or iPhone without a Mac?** Yes. Press *Install from GOG…* in
the launcher and sign in to GOG: it downloads the games you own. Or put GOG's installer
files on the device (the Files app or iCloud Drive) and choose *Use installer files…*.

**Can I bring my Windows saves and profiles?** Profiles and saves use the game's own
formats. Copy them into the profile folder (see below); this hasn't been tested much yet.

**Where are my profiles, saves and settings?**

| | Mac | iPad and iPhone |
|---|---|---|
| Profiles and saves | `~/Library/Application Support/HommaRe/Documents/My Games/Heroes of Might and Magic V/` (the launcher's *Saves* button opens it) | Inside the app |
| Installed games | Where you chose in the launcher | Inside the app (`Games` in the Files app) |

**Hot-seat saves don't show in Load Game.** Hot-seat games are loaded from *Multiplayer →
Hot Seat → Load*, not from the single-player list, as in the original.

**My new profile isn't remembered.** The game writes its profile choice when it quits
normally. Leave with *Exit → OK* once after creating a profile.

**The game is silent on my iPhone.** The silent switch is on. Turn it off, or set
*Settings → Silent mode* to *Ignore*.

**Is multiplayer supported?** Hot-seat on one device and games on your local network,
yes. Ubisoft closed the game's Internet servers (Ubi.com) in 2015; a community server
can stand in for them (ubi-gs, below).
Network games use UDP ports 8888 and 8889.

**What is ubi-gs in the Multi Player menu?** Ubisoft closed the game's online service
(Ubi.com) in 2015. [ubi-gs](https://github.com/michal-kapala/ubi-gs) is a community
re-implementation of it that someone has to run; every player uses the same server.
Clicking *ubi-gs* asks for that server's address (`host` or `host:port`) and remembers the
ones that answered, in `ubi-gs-servers.txt` in the profile folder; until one has, it
suggests the community server `ubigs.wayofharm.com`. The game gets the server list from
it, logs in, joins the lobby's channels, and creates and joins games there. The server's
operator sees the CD key you log in with: any CD key works, so don't type your real one.

**Which name and password do I use there?** Make up new ones for that server: not your
Ubisoft (Ubi.com, Ubisoft Connect) login, since the servers are run by the community, and
not a password you use elsewhere. The game's login protocol carries the password
practically in the clear, so HommaRe sends a salted hash of it instead (scrypt, salted
with the server and your name): neither the operator nor anyone watching the network sees
the password itself, though a weak one can still be guessed from its hash. A server that
sends a one-time challenge (`LoginNonce` in its `servers.ini`) gets that hash only the
first time, then only answers to the challenge; `ubi-gs-logins.txt` in the profile folder
remembers where you registered (no password in it). A server that knows nothing of this
takes the hash for your password. Accounts made with the original game, which sends the
password as typed, can't be used from HommaRe.

**Playing online safely.** A ubi-gs server can be run by anyone, and local-network and
online play puts you in touch with other players' copies. The 2006 client parses what a
server or peer sends in old C++ that, in places, copied text into fixed stack buffers
without a length check — a hostile server could send an over-long field and overflow one.
HommaRe recompiles the game, so it replaces those copies with the game's own bounded
reader: a field that is too long is cut to fit and the game carries on with the shortened
text, rather than running off the end of a buffer. This covers every server-controlled
string field the client reads into a fixed buffer: all 52 places in each game that used
the unchecked copy — the player-info reply (the known overflow), the room list and member
list, member-join, the proxy login reply, the module proxy list, the game-start,
friend-update and ladder messages, and the lobby's other names and ids. A copy from
anywhere else would be cut at 16 bytes and logged rather than left unchecked. The room
list's first fields already had a length; the NAT and address replies carry no strings.
Local-network and direct peer-to-peer play uses a different, binary protocol: its
datagrams are read into a length-bounded buffer and parsed into fixed-size records, with
no unbounded copy found. Even so, if you would rather not trust an unknown ubi-gs server
or peer, stay on a local network or a VPN with people you know.

## What's next

- GOG's other languages (Czech, Spanish, Italian, Hungarian and Chinese): their installers
  are needed to check the files.
- iCloud saves sync

There are no dates and no promises: it's ready when it's ready.

## For developers

### How it works

HommaRe is built with **recomp-kit**, a static recompilation kit for 32-bit x86
Windows games.

1. **Analysis.** Ghidra finds the functions of `H5_Game.exe` (about 70,000) and of
   `granny2.dll`.
2. **Translation.** The kit's translator decodes each function with Capstone and writes
   C: x86 registers, flags, x87 and SSE as plain C. The eight registers live in locals
   inside each function, and flags are followed through the function's jumps and computed
   only where they are read.
3. **Compilation.** Clang compiles the C (about 100,000 functions) together with the
   runtime into one arm64 app. Each expansion becomes a module (`libgame.<id>.dylib`)
   that the app loads when you pick it in the launcher.
4. **Runtime.** In place of Windows, the runtime provides what the game calls:
   - `kernel32`, `user32` and friends: files, memory-mapped files, threads, messages,
     code pages (Windows-1251 for the Russian release);
   - the Microsoft C and C++ runtime (UCRT, VCRUNTIME140, MSVCP140);
   - **Direct3D 9 on Metal**: the fixed-function pipeline, `ps_1_x` / `vs_1_1` shaders
     translated to Metal Shading Language and kept in a Metal binary archive, DXT
     textures (decoded on the CPU where the GPU lacks BC formats), vertex buffers synced
     by page protection, MetalFX upscaling of the 3D with the interface drawn at full
     resolution;
   - DirectInput for mouse and keyboard, and SDL3 for windows, input and audio;
   - an **FMOD** stand-in that plays the game's sounds and music through the kit's mixer.
5. **Native code.** Hand-written C in `native/` hooks some of the game's functions,
   with each game's addresses in `games/*/native/addresses.h`:
   - `touch.c`, `camera.c`, `tap_attack.c`: touch and trackpad input, the camera
     gestures, the touch-friendly battle;
   - `widescreen.c`, `horplus.c`, `ui_bounds.c`, `hof_screen.c`: the wide virtual
     screen, Hor+, edge anchors and interface bounds (Hammers of Fate has no layout code
     of its own for them);
   - `phone_hud.c`, `phone_layout.c`: the iPhone layer: safe areas, scaled panels,
     corner menus, the layout by the window's size;
   - `gpu_skin.cpp`, `lighting.c`, `granny.c`, `scene.c`: the models' skinning and
     lighting on the GPU, and native versions of the hottest animation and scene code,
     each with a check mode that compares it with the translated original;
   - `render_split.c`: marks where the 3D ends, so the interface is drawn after the
     upscale;
   - `save.cpp`: saving settings when the app goes to the background.
6. **Interface mods from recipes.** Interface changes that need no code are the game's
   own mod packages: `Widescreen.h5u` (edge anchors from Tribes of the East),
   `PhoneUI.h5u` (button rows for the iPhone) and `MobileOptions.h5u`. The app carries
   only their *recipes* (`ui-mods/`): for each interface file, the SHA-256 of the file it
   was made from and the changed layout lines. At first start the app builds the mods
   from the player's own game data into the profile. Hammers of Fate reads no `UserMODs`,
   so it gets the same as `zz-*.pak` files mounted above its own data. The host also
   rebuilds `zz-port-texts.pak` at every start from the install's own files (game.toml
   `[texts]`): "Exit to Windows" reworded and the buttons' click at a quarter of its
   volume.

`granny2.dll` is translated at its own base address `0x50000000` and never rebased: the
game's `.gr2` model files carry raw pointers into it.

The game's data (maps, models, textures, sounds, videos) comes from the player's
installation. HommaRe contains no game data or assets, and `tools/check_bundle.py`
fails a build that would.

### Building from source

The repository contains no game code: the build translates **your own** executables, so
the Mac or Linux PC you build on needs the game installed (the build reads its executables:
`H5_Game.exe` and `granny2.dll`). The app that comes out carries no game data and no mod made from
it: on the device you install the game into it from your GOG installers, as in
[Quick start](#quick-start). The generated code, analysis and game files are ignored by
git. Never commit or share them.

Supported executables:

| Game | File | SHA-256 |
|---|---|---|
| Heroes V 1.60 | `bin/H5_Game.exe` | `820786b0f85c62ca96a1a5b18d03d60185047ae8dab09ef6f0b299e3abcfba7c` |
| Hammers of Fate 2.1 | `bina1/H5_Game.exe` | `c15cc3c68a5bd1f3111d7d2fda7ef7607dbe96d1aedadaf463b50cbe41b40730` |
| Tribes of the East 3.1 | `bin/H5_Game.exe` | `3eb6fce81071f48631360202d81d41c67b09d8b1807105b1f22c402e15d16268` |

**1. Dependencies.** Xcode, CMake, Ninja, Python 3 with the kit's requirements
(`capstone`, `pefile`, `numpy`, `Pillow`), and Ghidra with a JDK for the analysis.
On Linux, clang and lld in place of Xcode, SDL's X11, Wayland and audio development
headers (the list is in `kit/.github/workflows/checks.yml`), a Vulkan driver, and
`innoextract` for the launcher to install from GOG's installers. On Fedora:

```bash
sudo dnf install clang lld innoextract glslc java-21-openjdk-devel nas-devel \
  pulseaudio-libs-devel pipewire-jack-audio-connection-kit-devel libXScrnSaver-devel \
  mesa-libgbm-devel mesa-libGL-devel mesa-libEGL-devel libglvnd-devel ibus-devel \
  pipewire-devel libdecor-devel liburing-devel
```

On Debian 13, Ubuntu 24.04 or newer, and Raspberry Pi OS or a Chromebook's Linux
environment based on them (the build is tried with clang 20; Debian 12's clang 14 may be
too old for the kit's C++20, so there cross-compile as below):

```bash
sudo apt install clang lld make pkg-config python3-venv innoextract glslc openjdk-21-jdk \
  libasound2-dev libpulse-dev libaudio-dev libjack-jackd2-dev libsndio-dev libx11-dev \
  libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev libpipewire-0.3-dev libwayland-dev \
  libdecor-0-dev liburing-dev mesa-vulkan-drivers
```

**2. Link your game and analyse it.**

```bash
python3 -m venv .venv && .venv/bin/pip install -r kit/requirements-dev.txt
tools/setup.py --install "/path/to/Heroes of Might and Magic V" --link-only
tools/analyze.py
```

For the expansions, do the same in `games/hof` and `games/toe` (`games/run_analysis.py`).

**3. Build.** One command builds all three games into one app, with no game files in it,
and checks the result:

```bash
.venv/bin/python tools/build_app.py --target app
```

`--target ios-sim` builds for the iOS Simulator, and `--target ios` for a device, with
`RECOMP_IOS_TEAM` set to your Apple developer team. A bundle id belongs to the team that
registered it first, so for a device build change `bundle_id` in `game.toml`,
`games/hof/game.toml` and `games/toe/game.toml` to a prefix of your own (keeping the
`.hof` and `.toe` endings). The repository has no signing keys or profiles: Xcode signs
with your team. On Linux `--target app` leaves `build/package/HommaRe` (and a tarball beside
it): the `HommaRe` executable with the expansions in `modules/`, started from that folder. `--overlay` keeps the kit's
performance overlay (F11). Translation takes about 10–15 minutes per game and
compilation about 25 minutes per game and platform on an M1 Max; later builds are
incremental. Use `--regenerate` with `kit/tools/build.py` after changing the translator.

**Linux on 64-bit ARM** (ARM Chromebooks, Snapdragon X laptops, Asahi Linux, Raspberry Pi 5)
builds the same way on the ARM computer itself. An x86-64 Linux PC cross-compiles it with
its own clang and lld, against a Debian 12 sysroot that a rootless podman (or docker)
container makes; the container's apt runs under qemu-user (Fedora:
`qemu-user-static-aarch64`, Debian and Ubuntu: `qemu-user-static`):

```bash
kit/tools/linux_sysroot.py --arch arm64     # once: kit/build/sysroots/debian12-arm64
.venv/bin/python tools/build_app.py --target app --arch arm64
```

That leaves `build/linux-arm64/package/HommaRe` (and `HommaRe-linux-aarch64.tar.gz` beside
it) for Debian 12, Ubuntu 22.04 or anything newer (glibc 2.35, GCC 12's libstdc++). The translation is plain C and shared
with the x86-64 build, so nothing is translated twice; the same holds for `analysis/`, so
the analysis can run on another computer if your Ghidra has no decompiler for ARM Linux.
The ARM computer needs a Vulkan 1.3 driver, or Vulkan 1.1 with
`VK_KHR_dynamic_rendering`: Mesa's Honeykrisp (Asahi), Turnip (Adreno), PanVK (Mali) or
V3DV (Raspberry Pi). Kernels with 16 KiB pages (Asahi, as on the Mac) or 64 KiB pages
are fine: the app works in the kernel's own page size.

### Useful switches

| Variable | What it does |
|---|---|
| `RECOMP_LAUNCHER=0` | Start the game directly, without the launcher |
| `RECOMP_ADAPTIVE_FPS=1/0` | Force the 30 fps idle mode on or off |
| `RECOMP_ENERGY=on/off` | Force the energy mode |
| `RECOMP_D3D9_MAX_SCALE` | Cap the render scale |
| `RECOMP_D3D9_SPLIT=0` | Upscale the whole frame, interface included |
| `RECOMP_HOMM5_GPU_SKIN=0/1` | Model skinning and lighting on the CPU or the GPU |
| `RECOMP_HOMM5_UI_BOUNDS=0..3` | Interface bounds: screen edges, 21:9, 16:9, 4:3 |
| `RECOMP_HOMM5_BATTLE_CURSOR=0/1` | The battle cursor beside the damage hint, on any platform |
| `RECOMP_TRACKPAD_CAMERA=0..2` | Trackpad mode: height, like an iPad, game's own |
| `RECOMP_METAL_HUD=0/1` | Apple's Metal performance HUD |
| `RECOMP_CPUID_SSE=0` | Hide SSE from the game, so it takes its x87 paths |
| `RECOMP_AUTO_KEYS`, `RECOMP_MAX_SECONDS` | Scripted, unattended runs |
| `RECOMP_TRACE_FUNCS` | Trace calls to given game functions |

Design notes, with addresses, are in `docs/`: `expansions.md`, `trackpad-camera.md`,
`gog-installer.md`, `launcher-skin.md`, `ui-mods.md`, `performance-energy.md`.

## Credits

HommaRe stands on a lot of open-source work. Thank you to everyone behind:

| Project | What it's used for | License |
|---|---|---|
| recomp-kit (veritr1x) | The translator, runtime and host | MIT |
| Ghidra (NSA) | Finding the game's functions | Apache-2.0 |
| Capstone | x86 decoding in the translator | BSD-3-Clause |
| SDL3 | Windows, input, audio, app lifecycle | zlib |
| innoextract (Daniel Scharrer) | Unpacking GOG installers in the launcher | zlib |
| Boost, XZ Utils | Built into innoextract | BSL-1.0, 0BSD |
| miniz | Reading the game's packages to build the interface mods | MIT |
| FFmpeg | Video playback | LGPL-2.1-or-later |
| Lua 5.4 | The kit's mod foundation | MIT |
| Unicorn | Checking translated instructions in tests | GPL-2.0 (development only) |

Full license texts are in `kit/NOTICE` and `kit/third_party/`.

## License

HommaRe's own code and artwork are under the MIT License (`LICENSE`). recomp-kit in
`kit/` is under the MIT License too (`kit/LICENSE`, `kit/NOTICE`). Third-party
components in `kit/third_party/` keep their own licenses, listed above. Heroes of
Might and Magic V is not covered by either: no game files are included, and nothing
here relicenses them.

## Legal notice

HommaRe is an unofficial, fan-made project. It is not affiliated with, endorsed by
or sponsored by Ubisoft Entertainment, Nival or GOG.

"Heroes of Might and Magic", "Hammers of Fate" and "Tribes of the East" are trademarks of
Ubisoft Entertainment. Apple, Mac, macOS, iPad, iPhone, Metal and MetalFX are trademarks
of Apple Inc. All other trademarks belong to their respective owners. They are used here
only to say what HommaRe works with. The game content visible in screenshots belongs
to its owners.

**No game files are included or distributed.** HommaRe contains no game data or
assets: no maps, models, textures, music, sounds or videos. To play, you need your own
legally obtained copy of the game. The launcher verifies it and installs it.

HommaRe is provided "as is", without warranty of any kind.
