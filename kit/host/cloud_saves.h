// cloud_saves.h - the game's saves kept in step with a folder another device
// sees too (iCloud Drive, or any folder the player picks), when the player
// turned that on in the launcher.
//
// The game never works in that folder: it reads and writes its own profile as
// always, and this copies between the two. A synced folder may hold a file
// that is not downloaded yet, half of one still arriving, or the system's
// "Save 2" copy of a conflict, none of which the game should meet.
//
// What is copied: the saves of every player profile, "<game>/Profiles/<player>/
// Saves/**" under the profile's Documents/My Games, both ways, the newer one
// winning; and the player profile's settings files (*.cfg beside Saves/, and
// those in Profiles/) only to a side that has none, since they hold this
// device's screen resolution and graphics. Whatever either side loses (a file
// overwritten or deleted by the other side) goes first to a backup folder in
// the profile, the newest ten syncs kept.
//
// Each side's files are compared with how they looked at the last sync (a
// state file in the profile), so a change is told from a deletion and from
// the other side's change. Nothing is deleted on a side whose synced files
// are all gone, or when the synced folder cannot be read: that is a lost
// folder, not ten deletions.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace cloud_saves {

// What a path under Documents/My Games is to the sync ('/'-separated).
enum class Kind { None, Sync, Seed };
Kind classify(const std::string &rel);

struct Stamp {
    int64_t size = -1;
    int64_t mtime_ns = 0;
    bool operator==(const Stamp &o) const { return size == o.size && mtime_ns == o.mtime_ns; }
    bool operator!=(const Stamp &o) const { return !(*this == o); }
};

struct Entry {
    Stamp stamp;
    // Known to the synced folder but not on this device yet (an iCloud
    // placeholder): left alone this time.
    bool pending = false;
};
using Listing = std::map<std::string, Entry>; // relative path -> entry

// The synced folder's side. The local side is plain files (the profile).
class Io {
public:
    virtual ~Io() = default;
    // Every file under `root` with its relative path; false when the folder
    // cannot be read at all. A missing `root` is an empty listing.
    virtual bool list(const std::string &root, Listing *out) = 0;
    // Copies the synced file `src` to the local `dst` (a new file), keeping
    // its modification time.
    virtual bool read(const std::string &src, const std::string &dst) = 0;
    // Puts the local file `src` at `dst` in the synced folder (replacing it,
    // making its folders), keeping its modification time; `*now` is how `dst`
    // looks afterwards.
    virtual bool write(const std::string &src, const std::string &dst, Stamp *now) = 0;
    virtual bool remove(const std::string &path) = 0;
};

// Plain files, for a folder the system syncs by itself (and the tests).
Io &plain_io();
// The platform's: Apple's coordinates every access with the folder's file
// provider and downloads iCloud placeholders; elsewhere plain_io().
Io &platform_io();

struct Stats {
    int pulled = 0, pushed = 0, deleted_local = 0, deleted_cloud = 0, conflicts = 0, waiting = 0;
    std::string error; // the sync did not run: why
    bool changed() const { return pulled + pushed + deleted_local + deleted_cloud > 0; }
};

struct Paths {
    std::string local;  // the profile's Documents/My Games
    std::string cloud;  // this game's folder in the synced one
    std::string state;  // the state file (in the profile)
    std::string backup; // where lost files go (in the profile)
};

// One sync. `now_ns` is the wall clock (a local file younger than 3 s may be
// being written by the game and waits for the next sync).
Stats sync(const Paths &paths, Io &cloud, int64_t now_ns);

// The paths for this game: `profile_dir` the game's profile, `cloud_root`
// the folder the player chose, `game_id` this game's (RECOMP_GAME_ID).
Paths paths_for(const std::string &profile_dir, const std::string &cloud_root, const std::string &game_id);

// The running game's sync: a sync now (waiting up to `wait_s` for it, as the
// game is about to read its saves), then one whenever the game's saves change
// and every minute (another device's saves), on a thread of its own.
void start(const Paths &paths, double wait_s);
// A sync soon (the app came back to the foreground).
void request();
// A sync now, waiting up to `wait_s` (the app goes to the background).
void flush(double wait_s);
// The last sync, then the thread goes (the game has ended).
void stop(double wait_s);
bool running();

// The file helpers the platform's Io shares.
namespace detail {
bool stat_path(const std::string &path, Stamp *out);
bool set_mtime(const std::string &path, int64_t mtime_ns);
std::string temp_beside(const std::string &path);
// A copy that keeps `src`'s modification time, put in place by a rename.
bool copy_keeping_time(const std::string &src, const std::string &dst);
bool same_bytes(const std::string &a, const std::string &b);
// Every regular file under `root` but dot files (true and none when `root`
// is missing).
bool list_files(const std::string &root, Listing *out);
} // namespace detail

} // namespace cloud_saves
