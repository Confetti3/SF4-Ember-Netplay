#pragma once
#include "../common/Country.hxx"
#include "../common/PointDigits.hxx"
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

// The game's point digits: their sheet, and each glyph's ink in it. `sheet` is
// set once the sheet has loaded and been measured.
struct PointDigitSheet {
    SelectionImage image;
    std::shared_ptr<const point_digits::Sheet> sheet;
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
    // The fighter's whole figure in a costume and colour (the packaged
    // cutout, or a custom costume's own picture), cropped to the figure: a
    // room's Characters pictures. Falls back to the original outfit.
    SelectionImage Character(int fighter, int costume, int color, bool large = false);
    SelectionImage CharacterFor(int fighter, int costume, int color, float drawnHeight) {
        return Character(fighter, costume, color, drawnHeight > ThumbnailSide);
    }
    SelectionImage Appearance(int fighter, int costume, int color);
    SelectionImage Ultra(int fighter, int ultra);
    // The game's own Ultra mark from its character select archives: the green
    // I, the purple II or the flaming W (`ultra` 0 to 2, uc1.dds to uc3.dds),
    // cropped to the painted mark. Read from the game at runtime and never
    // repackaged; missing where the game has none (only Ultra's own archive
    // has the W), so the caller draws its own badge.
    SelectionImage UltraIcon(int ultra);
    // RandomStageId is served from the game's character-select random tile.
    SelectionImage Stage(int nativeId);
    // An emblem from the game's own online ranking archive by its entry name,
    // such as "class_B.dds" (USF4's class emblems, NameMarks.cxx).
    SelectionImage RankingEmblem(const std::string& entry);
    // Ember's developer mark (assets/input/mark_developer.png), shown in place
    // of a class; wider than tall.
    SelectionImage DeveloperMark();
    // Ember's level digits (assets/input/level_digits.png), whose sprites
    // src/common/LevelDigits.hxx places; decoded at its own size and never
    // resized. Missing when the atlas is absent or not the size the header
    // lays out (LevelMark.cxx then draws the heading font's digits).
    SelectionImage LevelDigits();
    // The digits of the game's ranked result screen (point_digits, PointDigits.hxx),
    // read from the game at runtime and never repackaged. Missing, with a log
    // line, when the game lacks the sheet or it is not as measured.
    PointDigitSheet PointDigits();
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
