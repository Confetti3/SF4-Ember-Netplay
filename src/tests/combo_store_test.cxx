#include "../training/ComboStore.hxx"
#include <cstdio>
#include <fstream>
#include <stdexcept>
// The saved combos in a folder: keep, list, rename, delete, bring in, and what is refused.
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
SavedCombo Sample(const char* name, std::int64_t created) {
    SavedCombo c; c.name = name; c.fighter = 3; c.hits = 2; c.created = created;
    c.frames = {{0, 0, 0, 0}, {0x20, 0x20, 0, 0}};
    return c;
}
int main() {
    const auto folder = std::filesystem::temp_directory_path() / ("sf4e-combos-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        ComboStore store;
        Require(!store.Ready(), "A store with no folder was ready");
        std::string id, error;
        Require(!store.Add(Sample("a", 1), id, error), "A combo was kept with no folder");
        store.SetDirectory(folder / "combos");
        Require(store.Add(Sample("old", 100), id, error) && ValidComboId(id), "A combo was not kept");
        std::string second;
        Require(store.Add(Sample("new", 200), second, error) && second != id, "Two combos shared an id");
        auto list = store.List();
        Require(list.size() == 2 && list[0].name == "new" && list[1].name == "old" && list[0].length == 2, "The list was wrong or not newest first");
        SavedCombo loaded;
        Require(store.Load(id, loaded) && loaded.id == id && loaded.frames.size() == 2, "A combo did not load");
        Require(store.Rename(id, "renamed", error) && store.Load(id, loaded) && loaded.name == "renamed", "A rename was lost");
        Require(!store.Rename(id, "", error) && !store.Rename(id, "a\nb", error) && !store.Rename("missing", "x", error), "A bad rename was taken");
        // A file brought in gets a new id of its own, so it cannot replace a combo.
        std::string bytes;
        Require(store.Bytes(id, bytes), "The file was not read for a download");
        std::string imported;
        Require(store.Import(bytes, imported, error) && imported != id && store.List().size() == 3, "An import did not make a new combo");
        Require(!store.Import("not json", imported, error) && store.List().size() == 3, "A bad import was kept");
        // Ids that are paths are never opened; a stray or renamed file is not a combo.
        Require(!store.Load("../combos/" + id, loaded) && !store.Bytes("..", bytes) && !store.Delete("../x"), "A path was taken for an id");
        std::ofstream(folder / "combos" / "stray.json") << "{}";
        std::filesystem::copy_file(folder / "combos" / (id + ".json"), folder / "combos" / "copy-1.json");
        Require(store.List().size() == 3 && !store.Load("copy-1", loaded), "A file that is not under its own id was listed");
        Require(store.Delete(id) && !store.Delete(id) && store.List().size() == 2, "A delete did not remove one");
        // The folder holds only so many.
        std::filesystem::remove(folder / "combos" / "stray.json"); std::filesystem::remove(folder / "combos" / "copy-1.json");   // the limit counts files
        for (std::size_t i = store.List().size(); i < ComboStore::MaxCombos; ++i) Require(store.Add(Sample("x", 5), id, error), "A combo under the limit was refused");
        Require(!store.Add(Sample("one too many", 5), id, error) && error == "too many saved combos", "The limit was not kept");
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); std::filesystem::remove_all(folder); return 1; }
    std::filesystem::remove_all(folder);
    return 0;
}
