#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace selection {
constexpr int VersusStageCount = 28;
struct Stage { int id; const char* code; const char* name; };
// IDs are native table offsets, not grid positions. 22/23 are bonus rounds.
const std::array<Stage, VersusStageCount>& StageList();
// A stock versus stage, or nullptr (a custom stage has no catalog entry: see StageCode).
const Stage* FindStage(std::int64_t nativeId);
// A stock versus stage or a well-formed custom stage id: the check for values from the network and the room host.
// It allocates nothing and does not need the stage installed.
bool ValidStage(std::int64_t id);
// A valid stage id stays; anything else becomes 0.
int NormalizeStage(std::int64_t nativeId);
// Custom stages, added by a separate setup program (Ember never downloads anything): any three-character code of
// capital letters and digits that isn't one of the game's (C12, D05, XYZ ...), its files battle\stage\STG_<code>.emz.
// The code is the stage's id, its characters packed ('C' << 16 | '1' << 8 | '2'), so a custom stage travels as one
// number like any other. A PC without it shows its fallback, a stock stage chosen by rule. Random can land on one P1
// has installed (P1 resolves Random), and the other side then sees it or its fallback, as if it had been picked.
int CustomStageId(const char* code);   // -1 for anything but a custom code
bool IsCustomStage(std::int64_t id);
int CustomStageFallback(std::int64_t id);
// A valid stage's three-letter code (TRN, C12 ...), or "" for anything else.
std::string StageCode(std::int64_t id);
// A local choice only, like the 255 sentinel used for personal action and win
// quote. P1 resolves it to a catalog stage before the stage is sent, so it is
// never valid on the wire and FindStage, NormalizeStage and ReadStage reject it.
constexpr int RandomStageId = 255;
bool IsRandomStage(std::int64_t id);
// A valid stage id or RandomStageId stays; anything else becomes 0.
int NormalizeStageChoice(std::int64_t id);
// The Random stage pool: the stages Random may land on, the game's and the custom stages installed here alike, kept
// as the stages taken out (any number of them, by id, saved as one list). A local choice like Random itself: P1
// applies it when resolving. At least one of the game's stages always stays in: taking out the last is refused, and
// a saved list that takes them all out puts them all back. A custom stage taken out stays out while it isn't
// installed, so installing it again keeps the choice.
class RandomPool {
public:
    RandomPool() {}
    // From a saved list of the stages taken out: anything that isn't a valid stage id is dropped.
    explicit RandomPool(const std::vector<std::int64_t>& excluded);
    // A valid stage not taken out (whether a custom one is installed is the caller's to say).
    bool Contains(int id) const;
    // Whether Toggle would change the stage: false for an invalid id, and for the last game stage left in.
    bool CanToggle(int id) const;
    // Takes a stage out, or puts it back. False (nothing changed) where CanToggle is false.
    bool Toggle(int id);
    // The stages Random may land on: the game's not taken out, then the installed custom stages not taken out.
    std::vector<int> Stages(const std::vector<int>& installedCustoms) const;
    int Count(const std::vector<int>& installedCustoms) const { return static_cast<int>(Stages(installedCustoms).size()); }
    // The stages taken out, ascending: what is saved.
    const std::vector<int>& Excluded() const { return excluded_; }
    bool operator==(const RandomPool& other) const { return excluded_ == other.excluded_; }
    bool operator!=(const RandomPool& other) const { return !(*this == other); }
private:
    int StockIn() const;
    std::vector<int> excluded_;
};
// Random becomes the stage picked by roll among the pool's stages (the custom ones installed here among them); any
// other choice is normalized.
int ResolveStage(int choice, std::uint32_t roll, const RandomPool& pool = RandomPool(), const std::vector<int>& installedCustoms = std::vector<int>());
} }
