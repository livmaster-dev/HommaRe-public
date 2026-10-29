// android_files.h - the native launcher's files on Android: what the
// system's pickers hand back (content URIs, not paths) made into paths the
// launcher and innoextract read, and the app's own folders.
//
// On a Chromebook the player downloads the GOG offline installers with the
// Files app and picks them here, or picks a folder holding them or an
// installed copy of the game. Installers are linked by name into a folder of
// the app's own (each link is /proc/self/fd/<n>, a descriptor the picker
// granted and innoextract inherits and reads through, so the 2-4 GB slices
// are read where they are; Android would not let it open them again by
// name); where the system will not even give a document's size through its
// link, the document is copied there instead. An installed copy is copied
// into the launcher's Games folder, as an install would put it.
#pragma once
#include "installer.h"

#include <functional>
#include <string>
#include <vector>

namespace launcher::native::android {

// The app's external files folder (Android/data/<package>/files): the host's
// resources, the launcher's Games folder and the profile are there.
std::string data_dir();

// Makes the picked documents readable by path: fills *paths with what
// Model::accept takes (the linked installers' folder, a copied game).
using Materialize = std::function<Outcome(const ProgressFn &progress, std::vector<std::string> *paths)>;

// Shows the system's picker for installers (several files) or a folder.
// `done` gets the work that turns the choice into paths (run it off the main
// thread: a copy takes minutes), on the activity's thread; nothing when the
// player cancelled. A copied game goes into `games_dir`, without the files
// `exclude` names (launcher-games.json, tools/stage_game_files.py's rule).
void pick(bool folder, const std::string &games_dir, const std::vector<std::string> &exclude,
          std::function<void(Materialize)> done);

// Lets go of the last pick (its descriptors, its links and any copies) once
// its installs are done.
void forget_picked();

} // namespace launcher::native::android
