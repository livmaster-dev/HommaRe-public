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
// not), at the path without the prefix.
void host_ui_mods(const std::string &game_root, const std::string &profile_dir, const std::string &recipe_dir,
                  bool phone);

// An app with two sets of recipes (Android: a desktop-class device's and a
// touch screen's) applies one: the mods only the other set makes (a recipe
// in other_dir by a name kept_dir does not have) leave the profile, with
// their stamps, so a device whose form changed plays without them.
void host_ui_mods_retire(const std::string &profile_dir, const std::string &other_dir,
                         const std::string &kept_dir);
