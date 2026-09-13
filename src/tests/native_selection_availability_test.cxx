#include "../Dimps/Dimps__Selection.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/Theme.hxx"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
sf4e::selection::Availability NativeAvailability(std::uint32_t flags) {
    sf4e::selection::Availability result;
    result.ready = true;
    result.costumes = Dimps::Selection::CostumeAvailabilityMask(flags);
    result.colors.fill(UINT32_MAX);
    result.personalActions = 0x3ff;
    return result;
}
}

int main() try {
    using namespace sf4e;
    using namespace sf4e::selection;
    using namespace sf4e::ui;
    // Read-only capture from Steam 1.05 SHA256 5d724595...b9eb, 2026-09-10.
    // The same low bits are checked by native saved/match-choice validation
    // (69FBD0 -> 69FFD0), independently of the native menu's high bits.
    Check(AllowedCostumes(0, NativeAvailability(0x000f007f)) ==
        (std::vector<int>{0, 1, 2, 3, 4, 5, 6}), "Owned Ryu DLC costumes disappeared from selectable choices");
    Check(AllowedCostumes(43, NativeAvailability(0x0003003f)) ==
        (std::vector<int>{0, 1, 2, 3, 4, 5}), "Owned later-roster DLC costumes disappeared from selectable choices");
    Check(AllowedCostumes(0, NativeAvailability(0x007f0001)) ==
        (std::vector<int>{0}), "Menu bits exposed an unowned costume");
    Check(AllowedCostumes(0, NativeAvailability(0x000000c1)) ==
        (std::vector<int>{0, 6}), "Sparse ownership or reserved costume bounds were lost");
    Check(AllowedCostumes(43, NativeAvailability(0x000000ff)).size() == 6,
        "Reserved native slots became selectable costumes");
    Check(AllowedCostumes(0, NativeAvailability(0)).empty(), "Empty native ownership invented costumes");

    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 960);
    ApplyTheme(1); io.Fonts->Build();
    std::vector<MenuEntry> rows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
    for (const int fighter : {0, 43}) {
        FighterSelector selector;
        Pick pick; pick.fighter = fighter;
        const auto availability = NativeAvailability(fighter == 0 ? 0x000f007f : 0x0003003f);
        const auto frame = [&](unsigned buttons = 0, bool editable = true) {
            SetMenuInput({buttons, 0}); ImGui::NewFrame();
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Selection regression");
            // No artwork: ownership must still yield working gallery choices.
            selector.Draw(pick, true, nullptr, [&](int) { return availability; }, nullptr, editable);
            ImGui::End(); ImGui::Render();
        };
        selector.Navigation().Push("costumes"); frame(); frame();
        Check(rows.size() == CostumeCount(fighter), "Missing artwork hid owned costume cards");
        for (int costume = BaseCostumeCount(fighter); costume < CostumeCount(fighter); ++costume) {
            const auto id = "costume-" + std::to_string(costume);
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& row) { return row.id == id; });
            Check(found != rows.end() && found->enabled, "Owned DLC gallery card is not selectable");
            selector.Navigation().Focus(id, rows); frame(MenuInput::Select); frame();
            Check(pick.costume == costume, "Selecting a DLC card did not save its native ID");
            frame(0, false); frame();
            Check(pick.costume == costume && Available(pick, true, availability),
                "Locking or refreshing selection reset an owned DLC costume");
        }
    }
    SetMenuEntriesProbe({}); ImGui::DestroyContext();
    std::cout << "Native ownership, DLC gallery, missing art and saved costume regressions passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
