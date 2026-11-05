// ui_mods.h - a game's interface mods made on the player's device from their
// recipes (the game repository's ui-mods/*.recipe, tools/ui_recipe.py) and
// the player's own install, so no game file, nor one made from it, ships
// with the app.
#pragma once

#include <string>

// Each recipe in recipe_dir is applied to the paks under game_root and the
// mod written into profile_dir at the recipe's `out` path, when it is not
// there already from the same recipe and paks. Recipes whose `out` starts
// with phone/ are made only when `phone` (and taken out of the profile when
// not), at the path without the prefix; those with touch/ the same way only
// when `native_touch` (a finger's gestures go to the game at their point).
void host_ui_mods(const std::string &game_root, const std::string &profile_dir,
                  const std::string &recipe_dir, bool phone, bool native_touch);
