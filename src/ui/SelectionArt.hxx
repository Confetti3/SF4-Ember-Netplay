#pragma once
#include "../common/Country.hxx"
#include <imgui.h>
#include <functional>
#include <memory>
#include <string>

struct IDirect3DDevice9;

namespace sf4e { namespace ui {
struct SelectionImage {
    ImTextureID texture = 0;
    int width = 0, height = 0;
    ImVec2 uvMin = ImVec2(0, 0), uvMax = ImVec2(1, 1);
    bool missing = false;
};

// File reads, decompression, and WIC decoding run on a worker. Only Pump touches
// D3D9. Construct/destroy with the overlay's device lifecycle, outside DllMain.
class SelectionArt {
public:
    // Receives one line per image that could not be loaded, or that needed a
    // fallback, on Pump's thread. Art never logs by itself: the host decides
    // where lines go.
    using Logger = std::function<void(const std::string&)>;
    SelectionArt(IDirect3DDevice9* device, std::wstring gameRoot, std::wstring assetRoot, Logger log = {});
    ~SelectionArt();
    SelectionArt(const SelectionArt&) = delete;
    SelectionArt& operator=(const SelectionArt&) = delete;
    void Pump();
    // The thumbnail's longest side; `large` decodes up to 512 pixels.
    static constexpr int ThumbnailSide = 128;
    SelectionImage Portrait(int fighter, bool large = false);
    // The thumbnail unless it would be stretched: a large portrait costs
    // about sixteen times the memory.
    SelectionImage PortraitFor(int fighter, float drawnHeight) { return Portrait(fighter, drawnHeight > ThumbnailSide); }
    SelectionImage Appearance(int fighter, int costume, int color);
    SelectionImage Ultra(int fighter, int ultra);
    // RandomStageId is served from the game's character-select random tile.
    SelectionImage Stage(int nativeId);
    // An emblem from the game's own online ranking archive by its entry name,
    // such as "class_B.dds" (USF4's class emblems, NameMarks.cxx).
    SelectionImage RankingEmblem(const std::string& entry);
    // Ember's developer mark (assets/input/mark_developer.png), shown in place
    // of a class; wider than tall.
    SelectionImage DeveloperMark();
    SelectionImage MenuBackdrop();
    SelectionImage InputPrompt(const std::string& name);
    // A country's flag: its cell of the flag atlas (assets/flags/flags.png),
    // which is decoded at its own size and never resized. Empty for none;
    // missing when the atlas is absent or not the size Countries.hxx lays out.
    SelectionImage Flag(const CountryCode& country);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} }
