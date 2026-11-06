// native_installer_tests.cpp - the native launcher's GOG installer on Linux
// (launcher/linux/installer.cpp): finding installers among files, reading
// innoextract's output, and installing, resuming, repairing and patching
// through a stand-in innoextract that answers from fixture folders. No game.
#include "../launcher/linux/detect.h"
#include "../launcher/linux/installer.h"
#include "../launcher/linux/l10n.h"

#include "../../platform/os.h"
#include "../../runtime/sha256.h"
#include "../../third_party/miniz/miniz.h"

// miniz's zlib names are macros; gog::inflate is the launcher's own.
#undef inflate

#include <cstdio>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace launcher::native;

namespace {

std::string g_scratch;

void write_file(const std::string &path, const std::string &text) {
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/')
            os_mkdir(path.substr(0, i).c_str());
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

std::string read_file(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
        out.append(buf, n);
    fclose(f);
    return out;
}

void remove_tree(const std::string &path) {
    OsStat st{};
    if (os_lstat(path.c_str(), &st) != 0)
        return;
    if (st.is_dir && !st.is_symlink) {
        std::vector<std::string> names;
        os_listdir(
            path.c_str(),
            [](const char *name, void *user) {
                static_cast<std::vector<std::string> *>(user)->push_back(name);
                return 0;
            },
            &names);
        for (const std::string &n : names)
            remove_tree(path + "/" + n);
        os_rmdir(path.c_str());
    } else {
        os_unlink(path.c_str());
    }
}

// A fresh folder under the scratch directory.
std::string fresh(const std::string &name) {
    const std::string dir = g_scratch + "/" + name;
    remove_tree(dir);
    os_mkdir(dir.c_str());
    return dir;
}

std::string name_of(const std::string &path) {
    return path.substr(path.find_last_of('/') + 1);
}

std::vector<std::string> names_of(const std::vector<std::string> &paths) {
    std::vector<std::string> out;
    for (const std::string &p : paths)
        out.push_back(name_of(p));
    return out;
}

std::string zlib(const std::string &data) {
    mz_ulong size = mz_compressBound(mz_ulong(data.size()));
    std::string out(size_t(size), '\0');
    mz_compress(reinterpret_cast<unsigned char *>(&out[0]), &size,
                reinterpret_cast<const unsigned char *>(data.data()), mz_ulong(data.size()));
    out.resize(size_t(size));
    return out;
}

// Bytes that do not compress to nothing, `n` of them.
std::string noise(size_t n, uint32_t seed) {
    std::string out(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = char(seed >> 24);
    }
    return out;
}

// innoextract as the launcher runs it, answering from <installer>.fake/:
// "id" holds the GOG id, files/ what the installer holds. Each file it
// unpacks is logged to "extracted"; with "slow" there it takes its time.
const char *kFakeInnoextract = R"sh(#!/bin/sh
mode= out= incl=
while [ $# -gt 1 ]; do
    case "$1" in
    --gog-game-id) mode=id ;;
    --list) mode=list ;;
    --extract) mode=extract ;;
    --output-dir) shift; out=$1 ;;
    --include) shift; incl="$incl $1" ;;
    esac
    shift
done
fake="${1%.exe}.fake"
if [ ! -d "$fake" ]; then
    echo "Not a supported Inno Setup installer!" >&2
    echo "Done with 1 error." >&2
    exit 1
fi
case $mode in
id)
    echo "Inspecting \"Fake\" - setup data version 5.6.2 (unicode)"
    [ -f "$fake/id" ] && echo "GOG.com game ID is $(cat "$fake/id")"
    echo "Done." ;;
list)
    echo "Listing \"Fake\" - setup data version 5.6.2 (unicode)"
    cd "$fake/files" && find . -type f | sed 's|^\./\(.*\)$| - "\1" [en-US] (1 KiB)|' ;;
extract)
    for p in $incl; do
        src="$fake/files$p"
        if [ -d "$src" ]; then
            mkdir -p "$out$p" && cp -R "$src/." "$out$p/"
            continue
        fi
        [ -f "$src" ] || continue
        mkdir -p "$(dirname "$out$p")"
        echo " - \"${p#/}\""
        cp "$src" "$out$p"
        echo "${p#/}" >> "$fake/extracted"
        [ -f "$fake/slow" ] && sleep 0.4
    done ;;
esac
exit 0
)sh";

// ---------------------------------------------------------------------------

void test_names() {
    std::string base;
    int index = 0;
    CHECK(finder::slice("setup_x-1.bin", &base, &index) && base == "setup_x" && index == 1);
    CHECK(finder::slice("Setup_X-12.BIN", &base, &index) && base == "Setup_X" && index == 12);
    CHECK(!finder::slice("setup_x.bin", &base, &index));
    CHECK(!finder::slice("setup_x-0.bin", &base, &index));
    CHECK(!finder::slice("setup_x-1.exe", &base, &index));
    CHECK(!finder::slice("setup_x-1 (1).bin", &base, &index));
    CHECK(finder::normalized("Setup_X (1)") == "setup_x");
    CHECK(finder::normalized("setup_x copy 2") == "setup_x");
    CHECK(finder::normalized("setup_x copy") == "setup_x");
    CHECK(finder::normalized("setup_x \xD0\x9A\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x8F 3") ==
          "setup_x"); // "Копия"
    CHECK(finder::normalized("  setup_x  ") == "setup_x");
    CHECK(finder::is_setup_name("SETUP_game.EXE") && !finder::is_setup_name("game.exe"));
    CHECK(finder::is_patch_name("patch_game_1_to_2.exe") &&
          !finder::is_patch_name("patch_game.bin"));
}

// Installers the Android launcher links to the descriptors a picker granted
// (launcher/linux/android_files.cpp): each link stands for the document
// under its own name, so the slices are found beside the installer and
// innoextract is handed the link, not the file only the descriptor reaches.
void test_descriptor_links() {
    const std::string dir = fresh("links"), elsewhere = fresh("links-elsewhere");
    write_file(elsewhere + "/document-17", "x");
    write_file(elsewhere + "/document-18", "x");
    const int exe = open((elsewhere + "/document-17").c_str(), O_RDONLY);
    const int bin = open((elsewhere + "/document-18").c_str(), O_RDONLY);
    CHECK(exe >= 0 && bin >= 0);
    CHECK(symlink(("/proc/self/fd/" + std::to_string(exe)).c_str(), (dir + "/setup_l.exe").c_str()) == 0);
    CHECK(symlink(("/proc/self/fd/" + std::to_string(bin)).c_str(), (dir + "/setup_l-1.bin").c_str()) == 0);
    CHECK(canonical(dir + "/setup_l.exe") == canonical(dir) + "/setup_l.exe");
    const finder::Found f = finder::find({dir});
    CHECK(f.setups == std::vector<std::string>{canonical(dir) + "/setup_l.exe"} && f.problems.empty());
    // An ordinary link still resolves to what it names.
    CHECK(symlink((elsewhere + "/document-17").c_str(), (dir + "/plain").c_str()) == 0);
    CHECK(canonical(dir + "/plain") == canonical(elsewhere + "/document-17"));
    close(exe);
    close(bin);
}

void test_find() {
    const std::string dir = fresh("find");
    // A pair with a second slice.
    write_file(dir + "/a/setup_a.exe", "x");
    write_file(dir + "/a/setup_a-1.bin", "x");
    write_file(dir + "/a/setup_a-2.bin", "x");
    // A slice alone.
    write_file(dir + "/b/setup_b-1.bin", "x");
    // A slice renamed on download.
    write_file(dir + "/c/setup_c.exe", "x");
    write_file(dir + "/c/setup_c-1 (1).bin", "x");
    // An installer renamed on download.
    write_file(dir + "/d/setup_d (1).exe", "x");
    write_file(dir + "/d/setup_d-1.bin", "x");
    // Two levels down is found, three is not.
    write_file(dir + "/e/x/y/setup_e.exe", "x");
    write_file(dir + "/e/x/y/setup_e-1.bin", "x");
    write_file(dir + "/e2/x/y/z/setup_e2.exe", "x");
    write_file(dir + "/e2/x/y/z/setup_e2-1.bin", "x");
    os_mkdir((dir + "/f").c_str());
    write_file(dir + "/g/readme.txt", "x");
    write_file(dir + "/g/other.exe", "x");
    write_file(dir + "/patches/patch_z.exe", "x");
    write_file(dir + "/patches/patch_a.exe", "x");

    finder::Found f = finder::find({dir + "/a"});
    CHECK(names_of(f.setups) == std::vector<std::string>{"setup_a.exe"} && f.problems.empty());
    // The installer picked by itself, or by one of its slices.
    f = finder::find({dir + "/a/setup_a-2.bin", dir + "/a/setup_a.exe"});
    CHECK(f.setups.size() == 1 && f.problems.empty());

    f = finder::find({dir + "/b"});
    CHECK(f.setups.empty() && f.problems.size() == 1 &&
          f.problems[0].kind == finder::Problem::MissingSetup &&
          f.problems[0].expected == "setup_b.exe" && f.problems[0].renamed.empty());
    CHECK(finder::describe(f.problems[0]) ==
          "setup_b.exe is not beside setup_b-1.bin. The installer needs both files in one folder.");

    f = finder::find({dir + "/c"});
    CHECK(f.setups.empty() && f.problems.size() == 1 &&
          f.problems[0].kind == finder::Problem::MissingSlice &&
          name_of(f.problems[0].renamed) == "setup_c-1 (1).bin");
    CHECK(finder::describe(f.problems[0]) ==
          "setup_c-1.bin is not beside setup_c.exe, but setup_c-1 (1).bin is: "
          "it looks renamed on download. Rename it to setup_c-1.bin.");

    // Found from both files, told once: the installer is the renamed one.
    f = finder::find({dir + "/d"});
    CHECK(f.setups.empty() && f.problems.size() == 1);
    if (!f.problems.empty())
        CHECK(finder::describe(f.problems[0]) ==
              "setup_d.exe is not beside setup_d-1.bin, but setup_d (1).exe is: "
              "it looks renamed on download. Rename it to setup_d.exe.");

    f = finder::find({dir + "/e", dir + "/e2"});
    CHECK(names_of(f.setups) == std::vector<std::string>{"setup_e.exe"});
    CHECK(names_of(f.empty_folders) == std::vector<std::string>{"e2"});

    f = finder::find({dir + "/f", dir + "/g/readme.txt", dir + "/g/other.exe"});
    CHECK(f.setups.empty() && f.problems.empty() &&
          names_of(f.empty_folders) == (std::vector<std::string>{"f", "readme.txt", "other.exe"}));
    CHECK(finder::describe(
              {finder::Problem::MissingSlice, dir + "/x/setup_q.exe", "setup_q-1.bin", ""}) ==
          "setup_q-1.bin is not beside setup_q.exe. Put both files in one folder.");

    // Updates come apart from the installers, in name order.
    f = finder::find({dir + "/patches/patch_z.exe", dir + "/a", dir + "/patches/patch_a.exe"});
    CHECK(names_of(f.setups) == std::vector<std::string>{"setup_a.exe"});
    CHECK(names_of(f.patches) == (std::vector<std::string>{"patch_a.exe", "patch_z.exe"}));
    f = finder::find({dir});
    CHECK(names_of(f.setups) == std::vector<std::string>{"setup_a.exe"}); // e/x/y is three down
    CHECK(f.patches.size() == 2);
}

void test_listing_and_inflate() {
    const std::string listing = "Listing \"Game\" - setup data version 5.6.2 (unicode)\n"
                                " - \"app/music/DS/\"\n"
                                " - \"bin/H5_Game.exe\" [it-IT or ru-RU] (13.9 MiB)\n"
                                " - \"Data/Data.pak\" (1.2 GiB)\n"
                                " - \"tmp/a\" [temp] (3 KiB) - overwritten\n"
                                "Done.\n";
    const std::set<std::string> paths = gog::listed_paths(listing);
    CHECK(paths == (std::set<std::string>{"bin/h5_game.exe", "data/data.pak", "tmp/a"}));

    const std::string text = noise(5000, 7) + std::string(5000, 'a');
    std::string out;
    CHECK(gog::inflate(zlib(text), &out) && out == text);
    CHECK(!gog::inflate("hello, not a zlib stream", &out));
    std::string broken = zlib(text);
    broken.resize(broken.size() / 2);
    broken[broken.size() - 3] ^= 0x55;
    CHECK(!gog::inflate(broken, &out) || out != text);
    CHECK(!gog::inflate(std::string("\x78\x9c\x01", 3), &out));
}

void test_marker() {
    const std::string dir = fresh("marker");
    InstallMarker m;
    CHECK(!InstallMarker::read(dir, &m));
    m.installer = "gog-game";
    m.setup = "/games/setup_game.exe";
    m.manifest = "m.json";
    m.games = {"one", "two"};
    m.helper_pid = 4321;
    CHECK(m.write(dir));
    InstallMarker back;
    CHECK(InstallMarker::read(dir, &back) && back.installer == m.installer &&
          back.setup == m.setup && back.manifest == m.manifest && back.games == m.games &&
          back.helper_pid == 4321);
    write_file(dir + "/" + InstallMarker::kName, "{not json");
    CHECK(!InstallMarker::read(dir, &back));
}

// ---------------------------------------------------------------------------

struct Fixture {
    std::string dir, manifests, base;
    Config config;
    std::map<std::string, std::string> files; // what the installer holds
};

std::string manifest_json(const std::string &installer,
                          const std::map<std::string, std::string> &files) {
    std::string s = "{\"gogId\": \"42\", \"installer\": \"" + installer + "\", \"files\": [";
    bool first = true;
    for (const auto &[path, data] : files) {
        s += std::string(first ? "" : ",") + "\n  {\"path\": \"" + path +
             "\", \"size\": " + std::to_string(data.size()) + ", \"sha256\": \"" +
             sha256_hex(data.data(), data.size()) + "\"}";
        first = false;
    }
    return s + "]}\n";
}

// An installer (setup_<name>.exe, its slice and the stand-in's answers).
std::string make_installer(const std::string &dir, const std::string &name, const std::string &id,
                           const std::map<std::string, std::string> &files) {
    const std::string setup = dir + "/setup_" + name + ".exe";
    write_file(setup, "MZ");
    write_file(dir + "/setup_" + name + "-1.bin", "slice");
    const std::string fake = dir + "/setup_" + name + ".fake";
    remove_tree(fake);
    write_file(fake + "/id", id + "\n");
    for (const auto &[path, data] : files)
        write_file(fake + "/files/" + path, data);
    return setup;
}

int extracted(const std::string &setup) {
    const std::string log = read_file(setup.substr(0, setup.size() - 4) + ".fake/extracted");
    int n = 0;
    for (char c : log)
        n += c == '\n';
    return n;
}

void clear_log(const std::string &setup) {
    os_unlink((setup.substr(0, setup.size() - 4) + ".fake/extracted").c_str());
}

Fixture make_fixture() {
    Fixture fx;
    fx.dir = fresh("install");
    fx.manifests = fx.dir + "/manifests";
    fx.base = fx.dir + "/Games";
    const std::string helper = fx.dir + "/innoextract";
    write_file(helper, kFakeInnoextract);
    chmod(helper.c_str(), 0755);
    setenv("RECOMP_INNOEXTRACT", helper.c_str(), 1);

    fx.files = {{"bin/Game.exe", "game-exe-bytes"},
                {"data/data.pak", noise(300000, 1)},
                {"data/Extra.pak", noise(40000, 2)},
                {"app/readme.txt", "readme"}};
    write_file(fx.manifests + "/m-good.json", manifest_json("setup_game.exe", fx.files));
    // A build of the installer with a file this one does not hold.
    std::map<std::string, std::string> other = fx.files;
    other["data/other-language.pak"] = "x";
    write_file(fx.manifests + "/m-other.json", manifest_json("setup_game.exe", other));

    InstallerSpec spec;
    spec.id = "gog-game";
    spec.gog_id = "42";
    spec.folder = "Game Folder";
    spec.title.values["en"] = "The Game";
    spec.manifests = {"m-other.json", "m-good.json"};
    spec.games = {"game"};
    fx.config.installers.push_back(spec);
    return fx;
}

void test_install() {
    Fixture fx = make_fixture();
    std::map<std::string, std::string> held = fx.files;
    held["notes.txt"] = "not in the manifest";
    const std::string setup = make_installer(fx.dir, "game", "42", held);

    gog::Plan plan;
    Outcome o = gog::plan(setup, fx.config, fx.manifests, fx.base, &plan);
    CHECK(o.ok && plan.manifest_name == "m-good.json" && plan.dest == fx.base + "/Game Folder" &&
          plan.manifest.files.size() == 4);
    // Another product, a missing slice, no installer at all, no helper.
    const std::string stranger = make_installer(fx.dir, "stranger", "77", held);
    o = gog::plan(stranger, fx.config, fx.manifests, fx.base, &plan);
    CHECK(!o.ok &&
          o.error == "setup_stranger.exe is not an installer of a game this app supports.");
    os_unlink((fx.dir + "/setup_stranger-1.bin").c_str());
    o = gog::plan(stranger, fx.config, fx.manifests, fx.base, &plan);
    CHECK(!o.ok && o.error.find("setup_stranger-1.bin is not beside") == 0);
    write_file(fx.dir + "/setup_junk.exe", "MZ");
    write_file(fx.dir + "/setup_junk-1.bin", "x");
    o = gog::plan(fx.dir + "/setup_junk.exe", fx.config, fx.manifests, fx.base, &plan);
    CHECK(!o.ok && o.error == "Not a supported Inno Setup installer! Done with 1 error.");
    // A build of the installer no manifest describes.
    const std::string lean =
        make_installer(fx.dir, "lean", "42", {{"bin/Game.exe", "game-exe-bytes"}});
    o = gog::plan(lean, fx.config, fx.manifests, fx.base, &plan);
    CHECK(!o.ok && o.error.find("is another build of the installer") != std::string::npos);
    const std::string helper = getenv("RECOMP_INNOEXTRACT");
    setenv("RECOMP_INNOEXTRACT", (fx.dir + "/no-such-helper").c_str(), 1);
    CHECK(gog::helper().empty());
    o = gog::plan(setup, fx.config, fx.manifests, fx.base, &plan);
    CHECK(!o.ok && o.error == t("noHelper"));
    setenv("RECOMP_INNOEXTRACT", helper.c_str(), 1);
    CHECK(gog::helper() == helper);

    // A fresh install: every phase, every file, no marker left.
    CHECK(gog::plan(setup, fx.config, fx.manifests, fx.base, &plan).ok);
    std::set<int> phases;
    std::string name;
    o = gog::run(plan, [&](const Progress &p) {
        phases.insert(p.phase);
        name = p.name;
        return true;
    });
    CHECK(o.ok);
    CHECK(phases.count(Progress::Checking) && phases.count(Progress::Extracting) &&
          phases.count(Progress::Verifying));
    CHECK(name == "The Game");
    for (const auto &[path, data] : fx.files)
        CHECK(read_file(plan.dest + "/" + path) == data);
    CHECK(!exists(plan.dest + "/notes.txt"));
    CHECK(!exists(plan.dest + "/" + InstallMarker::kName));
    CHECK(extracted(setup) == 4);

    // Again over the install: a repair. Only the damaged file comes back.
    clear_log(setup);
    std::string pak = read_file(plan.dest + "/data/data.pak");
    pak[1000] ^= 1;
    write_file(plan.dest + "/data/data.pak", pak);
    CHECK(gog::run(plan, [](const Progress &) { return true; }).ok);
    CHECK(read_file(plan.dest + "/data/data.pak") == fx.files["data/data.pak"]);
    CHECK(extracted(setup) == 1);

    // Cancelled mid-unpack: the marker stays, the files unpacked stay; the
    // next run unpacks only the rest.
    remove_tree(plan.dest);
    clear_log(setup);
    write_file(setup.substr(0, setup.size() - 4) + ".fake/slow", "");
    o = gog::run(plan, [](const Progress &p) {
        return !(p.phase == Progress::Extracting && p.files_done >= 1);
    });
    CHECK(!o.ok && o.cancelled);
    InstallMarker marker;
    CHECK(InstallMarker::read(plan.dest, &marker) && marker.installer == "gog-game" &&
          marker.setup == setup && marker.manifest == "m-good.json" &&
          marker.games == std::vector<std::string>{"game"} && marker.helper_pid > 0);
    const int first = extracted(setup);
    CHECK(first >= 1 && first < 4);
    os_unlink((setup.substr(0, setup.size() - 4) + ".fake/slow").c_str());
    os_sleep_us(500000); // the stand-in's last copy, if the cancel caught it in one
    clear_log(setup);
    CHECK(gog::run(plan, [](const Progress &) { return true; }).ok);
    CHECK(extracted(setup) <= 4 - first + 1);
    CHECK(!exists(plan.dest + "/" + InstallMarker::kName));
    for (const auto &[path, data] : fx.files)
        CHECK(read_file(plan.dest + "/" + path) == data);

    // A cancel while checking.
    o = gog::run(
        plan, [](const Progress &p) { return p.phase != Progress::Checking || p.files_done < 2; });
    CHECK(o.cancelled && exists(plan.dest + "/" + InstallMarker::kName));
}

void test_patches() {
    Fixture fx = make_fixture();
    // The newer build's manifest; an older installer unpacks the old pak, an
    // update carries the new one, zlib-compressed under tmp/.
    std::map<std::string, std::string> fresh_files = fx.files, old_files = fx.files;
    old_files["data/data.pak"] = noise(300000, 99);
    write_file(fx.manifests + "/m-new.json", manifest_json("setup_old.exe", fresh_files));
    Config config = fx.config;
    config.installers[0].manifests = {"m-new.json"};
    const std::string setup = make_installer(fx.dir, "old", "42", old_files);
    const std::string patch = fx.dir + "/patch_game_1_to_2.exe";
    write_file(patch, "MZ");
    write_file(fx.dir + "/patch_game_1_to_2.fake/files/tmp/0123abcd",
               zlib(fresh_files["data/data.pak"]));
    write_file(fx.dir + "/patch_game_1_to_2.fake/files/tmp/background.png", "not zlib");
    write_file(fx.dir + "/patch_game_1_to_2.fake/files/app/script.iss", "x");

    // An update for a game that is not installed.
    std::vector<gog::PatchResult> results;
    auto go = [](const Progress &) { return true; };
    Outcome o = gog::apply_patch(patch, config, fx.manifests, fx.base, go, &results);
    CHECK(!o.ok &&
          o.error ==
              "patch_game_1_to_2.exe is an update for The Game. Install the game itself from its "
              "setup_….exe first, then the update.");
    CHECK(!exists(fx.base + "/.recomp-patch"));

    // The old installer alone: one file does not check out.
    gog::Plan plan;
    CHECK(gog::plan(setup, config, fx.manifests, fx.base, &plan).ok &&
          plan.manifest_name == "m-new.json");
    o = gog::run(plan, go);
    CHECK(!o.ok && !o.cancelled &&
          o.error == "1 files do not match the installer's manifest (data/data.pak). The installer "
                     "may be damaged: "
                     "download it again.");
    CHECK(exists(plan.dest + "/" + InstallMarker::kName) && !exists(plan.dest + "/data/data.pak"));

    // The update by itself brings it; a second time it has nothing to do.
    o = gog::apply_patch(patch, config, fx.manifests, fx.base, go, &results);
    CHECK(o.ok && results.size() == 1 && results[0].title == "The Game" &&
          results[0].replaced == 1);
    CHECK(read_file(plan.dest + "/data/data.pak") == fresh_files["data/data.pak"]);
    o = gog::apply_patch(patch, config, fx.manifests, fx.base, go, &results);
    CHECK(o.ok && results.size() == 1 && results[0].replaced == 0);
    CHECK(!exists(fx.base + "/.recomp-patch"));
    CHECK(gog::run(plan, go).ok && !exists(plan.dest + "/" + InstallMarker::kName));

    // With the installer: the install takes what the update carries.
    remove_tree(plan.dest);
    plan.patches = {patch};
    std::set<int> phases;
    o = gog::run(plan, [&](const Progress &p) {
        phases.insert(p.phase);
        return true;
    });
    CHECK(o.ok && phases.count(Progress::Patching));
    for (const auto &[path, data] : fresh_files)
        CHECK(read_file(plan.dest + "/" + path) == data);
    CHECK(!exists(plan.dest + "/.recomp-patch") && !exists(plan.dest + "/" + InstallMarker::kName));

    // An update for nothing this app knows.
    const std::string stranger = fx.dir + "/patch_other.exe";
    write_file(stranger, "MZ");
    write_file(fx.dir + "/patch_other.fake/files/tmp/x", zlib("something else"));
    o = gog::apply_patch(stranger, config, fx.manifests, fx.base, go, &results);
    CHECK(!o.ok && o.error == "patch_other.exe is not an update for a game this app supports.");
}

} // namespace

int main() {
    setenv("RECOMP_LAUNCHER_LANG", "en", 1);
    const char *base = getenv("RECOMP_LAUNCHER_TEST_DIR");
    g_scratch = std::string(base && *base ? base : "build/recomp") + "/native-installer-test";
    for (size_t i = g_scratch.find('/'); i != std::string::npos; i = g_scratch.find('/', i + 1))
        os_mkdir(g_scratch.substr(0, i).c_str());
    remove_tree(g_scratch);
    os_mkdir(g_scratch.c_str());
    // Absolute: the installs record where their installer is.
    g_scratch = canonical(g_scratch);

    struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"installer names", test_names},
        {"finding installers", test_find},
        {"installers linked to descriptors", test_descriptor_links},
        {"listing and inflate", test_listing_and_inflate},
        {"install marker", test_marker},
        {"install, repair, resume", test_install},
        {"GOG updates", test_patches},
    };
    for (const Test &t : tests) {
        const int before = g_failures;
        t.run();
        printf("%-28s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    remove_tree(g_scratch);
    return g_failures ? 1 : 0;
}
