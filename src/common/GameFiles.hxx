#pragma once
#include <filesystem>
#include <iterator>

namespace sf4e { namespace files {
// Installed content is searched newest first. Command data has its own
// regulation folder in each layer; an empty suffix means no command data.
struct Layer { const wchar_t* root; const char* commands; };
inline constexpr Layer GameLayers[] = {
    {L"patch_ae2_tu3", "battle/regulation/ae2_111"},
    {L"patch_ae2_tu2", "battle/regulation/ae2_110"},
    {L"patch_ae2_tu1b", ""},
    {L"patch_ae2_tu1", "battle/regulation/ae2_109"},
    {L"patch_ae2", ""},
    {L"dlc/04_ae2", "battle/regulation/ae2"},
    {L"dlc/03_character_free", "battle/regulation/latest"},
    {L"patch", "battle/regulation/latest_ae"},
    {L"dlc/03_ae", ""},
    {L"resource", "battle/chara"}
};
inline bool Exists(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::is_regular_file(path, ignored);
}
inline std::filesystem::path FindGameFile(const std::filesystem::path& game, const std::filesystem::path& relative) {
    for (const auto& layer : GameLayers) {
        const auto path = game / layer.root / relative;
        if (Exists(path)) return path;
    }
    return {};
}
} }
