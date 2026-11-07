# Changelog

What changed for players in each release of HommaRe. The builds are in
[Releases](../../releases); the recompilation kit underneath keeps its own notes in
[kit/CHANGELOG.md](kit/CHANGELOG.md).

## 0.1-playable-443 (2026-11-07)

- **iPhone and iPad: no more crashes while dragging** a stack, an artifact or a hero.
  Tribes of the East closed again and again, most of all with *Touch: Native* but with
  the cursor too: while the touch layer looked at what was being dragged it could read
  past the game's memory.
- **iPhone and iPad:** the app no longer closes now and then as it goes to the
  background or loses focus (the app switcher, Control Center). The launcher's closed
  window still answered the system keyboard and read freed memory.

## 0.1-playable-439 (2026-11-06)

- **The command panel on an iPad or an opened foldable iPhone** now stands at the left,
  right above the minimap on the map and above the hero's portrait in battle, two
  buttons wide, and its buttons are the size of the game's own round ones. It went to
  the top right before. On a phone it is where it was.
- **iPhone and iPad crash reports:** when the game closes on a fault, its log now keeps
  the whole report, iOS writes its own crash report (in *Settings → Privacy & Security →
  Analytics & Improvements → Analytics Data*, "HommaRe-…"), and the report names the
  thread that faulted. Please attach both to an issue if the game closes on you.

## 0.1-playable-433 (2026-11-05)

- **A command panel on iPad and iPhone,** in all three games: the camera buttons' panel
  now also carries the game's hotkeys for the screen you are on: the kingdom overview,
  objectives, heroes' mana, sleep, dig, the underground, the Tear of Asha map, quick
  save and load on the map; creature info, the initiative bar, wait, Ctrl's melee
  attack and Shift's split in battle, arrange and start before it. Its blue arrow opens
  all of them with their names; hold a button and drag it to reorder. The panel keeps
  clear of the game's own buttons, leaves out a command whose button the game shows,
  and presses the keys your profile binds. *Command buttons* in the launcher turns it
  off.
- **Native touch (new, off by default):** *Touch: Native* in the launcher hands a
  finger's taps to the game at their point, with no cursor to steer. Rest a finger on a
  creature, a hero or a stack for its short card, with a light tick of the phone when it
  comes; it stays after you lift. Tap the card for the full one, anywhere else to close
  it. In battle the first tap shows the move or the attack with its damage, a second
  tap acts. The cards show a hand instead of the right mouse button. Movies and loading
  screens take a tap as a key.
- **iPhone:** the jog dial's hidden disc no longer catches a finger beside the phone's
  button rows (Tribes of the East could close after a long press there). On a foldable
  iPhone the safe area no longer changes every frame (that filled the log with hundreds
  of megabytes).

## 0.1-playable-421 (2026-10-31)

- **German, French and Polish GOG releases,** in all three games: install them from
  GOG.com or from their installer files. The launcher tells each language's installer
  apart and checks it against its own file list, the game's "exit to Windows" and ubi.com
  hints are rewritten in each language, and the native screens speak it too. Their words
  are a first translation; corrections from native speakers are welcome.
- **Hammers of Fate in a local network game no longer crashes,** at the start of a game
  or later on the joining side, in the create menu on the host. The game's network
  threads need more stack than HommaRe gave them.
- **The original game's arithmetic, on every device.** HommaRe now rounds the way the
  original game asks and gets the same results on a Mac, an iPhone and a Linux PC. Its
  game database checksum now matches the original's, so creating a game on ubi-gs works
  in Hammers of Fate and Tribes of the East. Builds before this one compute differently:
  for network games, update every device you play on together.
- **More native screens,** in all three games: Load and Save in local network and
  hot-seat games, and the players of a loaded network game. Save is native in Hammers of
  Fate and Tribes of the East too. In the expansions the local network lobbies, the name
  box and the duel's create form are native as well, and so is Hammers of Fate's
  hot-seat duel. The duel view fits an iPhone's screen.
- **Touch-friendly battle in Hammers of Fate and Tribes of the East:** tap an enemy, then
  the cell to strike from, as in the base game.
- **Split a stack with a finger:** drag a stack and hold it still; a ring counts down,
  and the drop opens the game's split window.
- **iPhone and iPad:** pulling down Control Center or Notification Center only pauses the
  sound; the game no longer saves its settings and stops drawing each time. The log of
  the run before stays in the app's *Logs* folder in Files.
- **ubi-gs:** the login sends a token, never your password. A mistyped password is
  refused instead of making a new account, the native login form takes only the
  characters the game's own fields take, and long strings from a server can no longer
  overrun the game's buffers.
- **Mouse:** the wheel zooms without bouncing, and right- and middle-button drags work.
- **Wide screens:** a dragged creature's icon starts under the cursor, the turn and
  saving messages are centred, and Hammers of Fate's damage hint in battle sits under the
  cursor again.
- **Linux:** builds for 64-bit ARM too, and Vulkan no longer hangs when the GPU falls
  behind. Build it yourself for now.
- **For developers:** the iOS build works with Xcode 27.2.

## 0.1-playable-344 (2026-10-29)

- **Install from GOG.com.** *Install from GOG…* opens a GOG.com panel: sign in on GOG's
  own page, and the app downloads the games you own straight onto the device, in English
  or Russian, and checks every file. No installer files, no Files app, no copying
  gigabytes to the phone. A stopped download resumes where it stopped, also after the app
  was closed. Installer files still work: *Use installer files…* in the same panel.
- **Hammers of Fate with the English game on iPhone and iPad:** full screen instead of
  4:3, and *Options* works (the English release lacks the settings file the expansion
  reads).
- **Hammers of Fate, native menus:** *Options* in the main menu opens again instead of
  leaving an empty screen.
- **Online play through ubi-gs.** *Multi Player → Ubi.com* asks for a community server
  standing in for Ubisoft's closed one (it suggests `ubigs.wayofharm.com`), logs in,
  joins the lobby's channels, and creates and joins games there. On iPhone its login,
  CD key, channels and lobby are native screens, in all three games. The server's
  operator sees the CD key and password you type: any CD key works, so don't use your
  real one.
- **Linux:** the three games build, run and package on x86-64 Linux, with Vulkan and the
  native launcher. Build it yourself for now.

## 0.1-playable-286 (2026-10-28)

- **Local network games.** *Multiplayer → Local Network* works: copies of the game on
  one network find each other's games, join them and play. Checked with a Mac hosting
  and an iPad joining a match; two copies on one Mac see each other too. On iPad and
  iPhone the app asks once for access to the local network.
- **Native menus on iPhone.** The main menu, profiles, campaigns, Load and Save, Options,
  duels, custom and hot-seat games, the in-game menu and the game's questions are drawn
  as native screens, built from the game's own pictures, words and frames. On by default
  on iPhone; on iPad, choose *Menus → For the screen* in the launcher's settings.
- **Foldable iPhone:** the game no longer falls back to 1024x768 after a screen lock or
  an app switch.
- **For developers:** development now happens in this repository, through pull requests
  that CI checks. `pip install -r kit/requirements-dev.txt` works again, on Python 3.12
  to 3.14.

## 0.1-playable-247 (2026-10-26)

- Battle stack counters stay outside the interface bounds on wide screens.
- The launcher's settings fit the window.
- iPad and iPhone: an unsigned IPA for sideloading, added to the release on 2026-10-27
  (its version reads 249).

## 0.1-playable-243 (2026-10-26)

- The first ready-made Mac build: Heroes V, Hammers of Fate and Tribes of the East in one
  app, installed from your own GOG offline installers.
