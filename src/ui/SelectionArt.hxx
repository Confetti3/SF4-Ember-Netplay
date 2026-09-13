#pragma once
#include <imgui.h>
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
    SelectionArt(IDirect3DDevice9* device, std::wstring gameRoot, std::wstring assetRoot);
    ~SelectionArt();
    SelectionArt(const SelectionArt&) = delete;
    SelectionArt& operator=(const SelectionArt&) = delete;
    void Pump();
    SelectionImage Portrait(int fighter, bool large = false);
    SelectionImage Appearance(int fighter, int costume, int color);
    SelectionImage Ultra(int fighter, int ultra);
    SelectionImage Stage(int nativeId);
    SelectionImage MenuBackdrop();
    SelectionImage InputPrompt(const std::string& name);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} }
