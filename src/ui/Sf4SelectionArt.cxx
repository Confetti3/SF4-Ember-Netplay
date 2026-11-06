// SF4's pictures (SelectionArt.hxx): its fighters' portraits, figures and
// costumes, its Ultras and stages, read from the game's folder and Ember's
// package, and its ranking emblems from the game's own archive.
#include "SelectionArt.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include <cstring>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
namespace {
// Custom costumes and stages bring their pictures beside their files in the game's patch folder (the setup
// program that installs them puts them there): <CHR>_<NN>_<CC>.png per color or <CHR>_<NN>.png for all,
// STG_<code>.png or .jpg.
std::wstring PatchFolder(const std::wstring& gameRoot, const wchar_t* sub) { return gameRoot + L"/patch_ae2_tu3/battle/" + sub + L"/"; }
std::wstring TwoDigits(int n) { return std::to_wstring(n / 10) + std::to_wstring(n % 10); }
std::vector<std::wstring> CustomCostumePaths(const std::wstring& gameRoot, const char* code, int costume, int color) {
    const std::wstring chr(code, code + std::strlen(code));
    const std::wstring stem = PatchFolder(gameRoot, L"chara") + chr + L"/" + chr + L"_" + TwoDigits(costume + 1);
    return {stem + L"_" + TwoDigits(color + 1) + L".png", stem + L".png"};
}
std::vector<std::wstring> CustomStagePaths(const std::wstring& gameRoot, const char* code) {
    const std::wstring stem = PatchFolder(gameRoot, L"stage") + L"STG_" + std::wstring(code, code + std::strlen(code));
    return {stem + L".png", stem + L".jpg"};
}
}

SelectionImage SelectionArt::Portrait(int fighterId, bool large) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter) return {};
    const std::string& key = imageKey_.assign(fighter->code).append("/portrait");
    const auto paths = [&] {
        auto list = PackagedFiles(key);
        const std::wstring code(fighter->code, fighter->code + 3);
        for (const wchar_t* root : {L"patch_ae2_tu3", L"patch_ae2_tu2", L"patch_ae2", L"dlc/04_ae2",
                                   L"dlc/03_character_free", L"resource"})
            list.push_back(gameRoot_ + L"/" + root + L"/ui/chara_select/chara/sel_" + code + L".tex.emz");
        // Last resort when the game's portrait cannot be read or decoded: the
        // packaged original-outfit cutout, cropped the same way.
        const auto outfit = PackagedFiles(std::string(fighter->code) + "/costume-0/color-0");
        list.insert(list.end(), outfit.begin(), outfit.end());
        return list;
    };
    Policy portrait; portrait.cropToContent = portrait.reportAbsence = true;
    return Request(requestKey_.assign(key).append(large ? "-large" : "-thumb"), paths, large ? MaximumImageSide : ThumbnailSide, portrait);
}
SelectionImage SelectionArt::Character(int fighterId, int costume, int color, bool large) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter) return {};
    // A look this fighter has no such costume or colour for is drawn in the original outfit.
    if (!selection::ColorInRange(fighterId, costume, color)) costume = color = 0;
    const auto paths = [&] {
        // Custom content is this PC's own install, else what the game plays in
        // its place (ResolveAppearance): the original outfit for a custom
        // costume, the costume's first colour for a custom colour.
        const bool customCostume = selection::IsCustomCostume(costume), customColor = selection::IsCustomColor(color);
        auto list = customCostume || customColor ? CustomCostumePaths(gameRoot_, fighter->code, costume, color) : std::vector<std::wstring>{};
        const auto add = [&](int outfit, int palette) {
            const auto more = PackagedFiles(std::string(fighter->code) + "/costume-" + std::to_string(outfit) + "/color-" + std::to_string(palette));
            list.insert(list.end(), more.begin(), more.end());
        };
        if (!customCostume) add(costume, customColor ? 0 : color);
        if (costume != 0 || color != 0) add(0, 0);
        return list;
    };
    Policy figure; figure.cropToContent = figure.reportAbsence = true;
    auto& key = requestKey_.assign(fighter->code).append("/costume-").append(std::to_string(costume)).append("/color-").append(std::to_string(color));
    return Request(key.append(large ? "/figure-large" : "/figure-thumb"), paths, large ? MaximumImageSide : ThumbnailSide, figure);
}
SelectionImage SelectionArt::Appearance(int fighterId, int costume, int color) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter || !selection::ColorInRange(fighterId, costume, color)) return {};
    const std::string& key = imageKey_.assign(fighter->code).append("/costume-").append(std::to_string(costume)).append("/color-").append(std::to_string(color));
    if (selection::IsCustomCostume(costume) || selection::IsCustomColor(color))
        return Request(key, [&] { return CustomCostumePaths(gameRoot_, fighter->code, costume, color); });
    const auto result = Request(key, [&] { return PackagedFiles(key); });
    // Native selection art depicts the original outfit in its default palette.
    if (result.missing && costume == 0 && color == 0) return Portrait(fighterId, true);
    return result;
}
SelectionImage SelectionArt::Ultra(int fighterId, int ultra) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter || ultra < 0 || ultra > 1) return {};
    const std::string& key = imageKey_.assign(fighter->code).append("/ultra-").append(std::to_string(ultra));
    return Request(key, [&] { return PackagedFiles(key); });
}
SelectionImage SelectionArt::UltraIcon(int ultra) {
    if (ultra < 0 || ultra > 2) return {};
    // Asked for on every frame, up to sixteen times on an Arcade board: the
    // keys are made once. The three share one archive, which the worker keeps
    // for the icons queued together (Archive).
    static const std::string entries[] = {"uc1.dds", "uc2.dds", "uc3.dds"};
    static const std::string keys[] = {"ultra-icons/uc1.dds", "ultra-icons/uc2.dds", "ultra-icons/uc3.dds"};
    // Ultra's own archive first. The W is only there: an older archive opens
    // but lacks it, which would read as a failed file and be retried. The
    // game's icons are 32 pixels square; one far larger is not one of them.
    Policy mark; mark.cropToContent = true; mark.maxSide = 64;
    return Request(keys[ultra], [&] {
        std::vector<std::wstring> paths{gameRoot_ + L"/dlc/04_ae2/ui/chara_select/chara_select_dlc4.emz"};
        if (ultra < 2) {
            paths.push_back(gameRoot_ + L"/patch_ae2/ui/chara_select/chara_select_patch.emz");
            paths.push_back(gameRoot_ + L"/resource/ui/chara_select/chara_select.emz");
        }
        return paths;
    }, NativeSide, mark, entries[ultra]);
}
SelectionImage SelectionArt::RankingEmblem(const std::string& entry) {
    // Read from the game at runtime like the portraits, and never repackaged.
    // Cropped to the painted emblem: the Master and Grand Master crowns sit
    // small and off centre in their canvases, unlike the classes' bubbles.
    Policy emblem; emblem.cropToContent = emblem.reportAbsence = true;
    return Request("classes/" + entry, [&] {
        return std::vector<std::wstring>{gameRoot_ + L"/resource/ui/network_mode/Ranking.emz"};
    }, NativeSide, emblem, entry);
}
SelectionImage SelectionArt::Stage(int nativeId) {
    // The Random card borrows the game's character-select random tile, read at
    // runtime like the portraits and never repackaged.
    if (selection::IsRandomStage(nativeId)) {
        Policy tile; tile.cropToContent = tile.reportAbsence = true;
        return Request("stages/random", [&] {
            return std::vector<std::wstring>{gameRoot_ + L"/resource/ui/chara_select/chara/sel_RND.tex.emz"};
        }, MaximumImageSide, tile);
    }
    const std::string code = selection::StageCode(nativeId);
    if (code.empty()) return {};
    const std::string key = "stages/" + code;
    if (selection::IsCustomStage(nativeId))
        return Request(key, [&] { return CustomStagePaths(gameRoot_, code.c_str()); }, MaximumImageSide);
    return Request(key, [&] { return PackagedFiles(key); }, MaximumImageSide);
}
} }
