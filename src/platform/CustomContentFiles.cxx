#include "CustomContentFiles.hxx"
#include "Utf8.hxx"
#include "../common/StageCatalog.hxx"
#include <algorithm>
#include <fstream>
#include <windows.h>

namespace sf4e { namespace platform { namespace custom_content {
namespace {
// The name the setup program gives an item: the first line of the .txt beside it (UTF-8), at most 40 bytes. Only
// that much is read: a byte-order mark, 40 bytes, and the rest of a character the 40th byte may be in.
constexpr std::size_t NameBytes = 40;
std::string ReadName(const std::wstring& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    char buffer[3 + NameBytes + 4];
    in.read(buffer, sizeof(buffer));
    std::string line(buffer, static_cast<std::size_t>(in.gcount()));
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    const auto end = line.find_first_of("\r\n");
    if (end != std::string::npos) line.resize(end);
    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c >= 0 && c < ' '; }), line.end());
    if (line.size() > NameBytes) {
        std::size_t cut = NameBytes;
        while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;
        line.resize(cut);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    return Utf8ToWide(line.c_str()).empty() ? std::string() : line;
}

// A regular file (not a folder of that name).
bool Exists(const std::wstring& file) {
    const DWORD attributes = GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
// The two digits at `at` of a file name (RYU_12...), or -1.
int Number(const std::wstring& name, std::size_t at) {
    if (name.size() < at + 2 || name[at] < L'0' || name[at] > L'9' || name[at + 1] < L'0' || name[at + 1] > L'9') return -1;
    return (name[at] - L'0') * 10 + (name[at + 1] - L'0');
}
// Calls `found` with the name of each regular file matching `pattern`.
template<class F> void Each(const std::wstring& pattern, F found) {
    WIN32_FIND_DATAW data;
    struct Search {
        HANDLE handle;
        ~Search() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
    } search{FindFirstFileW(pattern.c_str(), &data)};
    if (search.handle == INVALID_HANDLE_VALUE) return;
    do if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found(std::wstring(data.cFileName));
    while (FindNextFileW(search.handle, &data));
}

}

selection::InstalledContentPtr Scan(const std::wstring& battleDirectory) {
    auto content = std::make_shared<selection::InstalledContent>();
    std::wstring battle = battleDirectory;
    if (battle.empty()) return content;
    if (battle.back() != L'\\' && battle.back() != L'/') battle += L"\\";
    const std::wstring root = battle + L"chara\\";
    for (int id = 0; id < selection::FighterCount; id++) {
        const std::string code = selection::FindFighter(id)->code;
        const std::wstring chr(code.begin(), code.end()), folder = root + chr + L"\\";
        auto& costumes = content->costumes[id];
        // A color is <name>.col.emb and <name>.obj.emm.
        const auto colorFiles = [&](const std::wstring& name) { return Exists(folder + name + L".col.emb") && Exists(folder + name + L".obj.emm"); };
        // Custom costumes: <CHR>_<NN>.obj.emo, NN 08..99, with all ten colors (an opponent may pick any of them).
        Each(folder + chr + L"_??.obj.emo", [&](const std::wstring& name) {
            const int costume = Number(name, 4) - 1;
            if (name.size() != 14 || !selection::IsCustomCostume(costume)) return;
            for (int color = 1; color <= selection::CustomColorCount; color++)
                if (!colorFiles(name.substr(0, 6) + L"_" + std::to_wstring(color / 10) + std::to_wstring(color % 10))) return;
            costumes.push_back(costume);
            const std::string label = ReadName(folder + name.substr(0, 6) + L".txt");
            if (!label.empty()) content->names[std::make_tuple(id, costume, -1)] = label;
        });
        std::sort(costumes.begin(), costumes.end());
        // Custom colors: <CHR>_<NN>_<CC>, CC 30..99, of one of the game's costumes or an installed custom one.
        Each(folder + chr + L"_??_??.col.emb", [&](const std::wstring& name) {
            const int costume = Number(name, 4) - 1, colorId = Number(name, 7) - 1;
            const bool costumeHere = costume >= 0 && (costume < selection::CostumeCount(id) ||
                std::binary_search(costumes.begin(), costumes.end(), costume));
            if (name.size() != 17 || !costumeHere || !selection::IsCustomColor(colorId) || !colorFiles(name.substr(0, 9))) return;
            content->colors[std::make_pair(id, costume)].push_back(colorId);
            const std::string label = ReadName(folder + name.substr(0, 9) + L".txt");
            if (!label.empty()) content->names[std::make_tuple(id, costume, colorId)] = label;
        });
    }
    for (auto& entry : content->colors) std::sort(entry.second.begin(), entry.second.end());
    // Custom stages: STG_<code>.emz and STG_<code>.tex.emz with a custom code (the pattern also finds the game's own).
    Each(battle + L"stage\\STG_???.emz", [&](const std::wstring& name) {
        if (name.size() != 11 || name[4] > 127 || name[5] > 127 || name[6] > 127) return;
        const char code[4] = {static_cast<char>(name[4]), static_cast<char>(name[5]), static_cast<char>(name[6]), 0};
        const int id = selection::CustomStageId(code);
        if (id < 0 || !Exists(battle + L"stage\\" + name.substr(0, 7) + L".tex.emz")) return;
        content->stages.push_back(id);
        const std::string label = ReadName(battle + L"stage\\" + name.substr(0, 7) + L".txt");
        if (!label.empty()) content->stageNames[id] = label;
    });
    std::sort(content->stages.begin(), content->stages.end());
    return content;
}

} } }
