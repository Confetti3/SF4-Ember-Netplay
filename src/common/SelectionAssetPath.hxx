#pragma once
#include "FighterCatalog.hxx"
#include "StageCatalog.hxx"
#include <string>

namespace sf4e { namespace selection {

// Only the selector's data files may be added to an update package. Exact
// generated paths reject traversal, streams, executable extensions and suffixes.
inline bool IsSelectionAssetPath(const std::wstring& path) {
    const std::wstring root = L"assets\\selection\\";
    if (path.compare(0, root.size(), root) != 0) return false;
    const auto relative = path.substr(root.size());
    if (relative == L"sources.json" || relative == L"cutouts.json" ||
        relative == L"horror-sources.json" || relative == L"stage-sources.json" ||
        relative == L"ultra-sources.json" || relative == L"color-sources.json" || relative == L"README.md") return true;
    for (const auto& stage : StageList()) {
        const std::wstring stem = L"stages\\" + std::wstring(stage.code, stage.code + 3);
        if (relative == stem + L".jpg" || relative == stem + L".png") return true;
    }
    if (relative.size() < 5 || relative[3] != L'\\') return false;
    for (int id = 0; id < FighterCount; ++id) {
        const char* code = FindFighter(id)->code;
        if (relative[0] != code[0] || relative[1] != code[1] || relative[2] != code[2]) continue;
        const auto image = relative.substr(4);
        if (image == L"portrait.png" || image == L"portrait.jpg" || image == L"portrait-cutout.png") return true;
        if (image == L"ultra-0.png" || image == L"ultra-0.jpg" ||
            image == L"ultra-1.png" || image == L"ultra-1.jpg") return true;
        for (int costume = 0; costume < CostumeCount(id); ++costume) {
            const auto directory = L"costume-" + std::to_wstring(costume) + L"\\";
            if (image.compare(0, directory.size(), directory) != 0) continue;
            const auto filename = image.substr(directory.size());
            for (int color = 0; color < ColorCount(id, costume); ++color) {
                const auto stem = L"color-" + std::to_wstring(color);
                if (filename == stem + L".png" || filename == stem + L".jpg" ||
                    filename == stem + L"-cutout.png") return true;
            }
            return false;
        }
        return false;
    }
    return false;
}

} }
