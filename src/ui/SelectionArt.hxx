#pragma once
#include "../common/Country.hxx"
#include <imgui.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct SelectionImage {
    ImTextureID texture = 0;
    int width = 0, height = 0;
    ImVec2 uvMin = ImVec2(0, 0), uvMax = ImVec2(1, 1);
    bool missing = false;
};

// Makes the renderer's textures for SelectionArt, on Pump's thread. Pixels
// are 32-bit BGRA, rows packed with no padding. Create returns 0 and says why
// in failure when the renderer refuses; Release frees what Create returned.
class TextureUploader {
public:
    virtual ~TextureUploader() = default;
    virtual ImTextureID Create(unsigned width, unsigned height, const unsigned char* bgra, std::string& failure) = 0;
    virtual void Release(ImTextureID texture) = 0;
};

// File reads, decompression, and WIC decoding run on a worker. Only Pump uses
// the uploader. Construct/destroy with the overlay's device lifecycle, outside DllMain.
//
// The pictures of the game's fighters, stages, Ultras and ranking emblems are
// the game's: each game defines those members once, as it defines Catalog()
// (GameCatalog.hxx), naming its files through Request. SF4's are
// Sf4SelectionArt.cxx, and the runtime's tests have the fake game's
// (tests/fake_selection.cxx). Ember's own pictures, the cache and the
// decoding are the same for every game.
class SelectionArt {
public:
    // Receives one line per image that could not be loaded, or that needed a
    // fallback, on Pump's thread. Art never logs by itself: the host decides
    // where lines go.
    using Logger = std::function<void(const std::string&)>;
    // gameRoot is the game's folder, which the game's pictures read from;
    // assetRoot holds Ember's packaged selection pictures.
    SelectionArt(std::unique_ptr<TextureUploader> uploader, std::wstring gameRoot, std::wstring assetRoot, Logger log = {});
    ~SelectionArt();
    SelectionArt(const SelectionArt&) = delete;
    SelectionArt& operator=(const SelectionArt&) = delete;
    void Pump();
    // The thumbnail's longest side; `large` decodes up to MaximumImageSide.
    static constexpr int ThumbnailSide = 128;
    static constexpr unsigned MaximumImageSide = 512;
    // A request's side that keeps the image at its own size.
    static constexpr unsigned NativeSide = 0;

    // The game's pictures.
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

    // Ember's own pictures.
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
    // What the pictures above ask for one with.
    //
    // Portraits fit their painted content rather than the transparent canvas,
    // and every fighter should have one, so an absent portrait is reported.
    // An atlas must decode to exactly its layout's size (exactWidth by exactHeight).
    // An image whose longer side is over maxSide (0: any) fails before it is
    // decoded, so a hostile file cannot make it decode a huge image.
    struct Policy { bool cropToContent = false, reportAbsence = false; unsigned exactWidth = 0, exactHeight = 0, maxSide = 0; };
    // The candidate files, the first usable one decoded. Listed only when a
    // decode is queued: screens request every visible image on every frame.
    using Files = std::function<std::vector<std::wstring>()>;
    // The picture cached as `key`, decoded to at most `side` pixels. An EMZ
    // archive candidate yields its image named `archiveEntry`, or with none
    // its first.
    SelectionImage Request(const std::string& key, const Files& files, unsigned side = 256, Policy policy = {},
        const std::string& archiveEntry = {});
    // Ember's packaged picture for `key` under assetRoot: its cutout, then
    // .png, then .jpg.
    std::vector<std::wstring> PackagedFiles(const std::string& key) const;

    std::wstring gameRoot_, assetRoot_;
    // The keys a request is built in, kept from one request to the next, so
    // that the same strings are made without an allocation on every frame.
    // imageKey_ names the packaged file (PackagedFiles), requestKey_ the entry
    // asked for. Used only on the drawing thread, and a request's key is read
    // only until it returns.
    std::string imageKey_, requestKey_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} }
