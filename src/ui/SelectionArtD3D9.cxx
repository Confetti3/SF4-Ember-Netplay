#include "SelectionArtD3D9.hxx"
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
using Microsoft::WRL::ComPtr;
std::string Hex(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

class D3D9Uploader final : public TextureUploader {
public:
    explicit D3D9Uploader(IDirect3DDevice9* device) : device_(device) {}
    ImTextureID Create(unsigned width, unsigned height, const unsigned char* bgra, std::string& failure) override {
        ComPtr<IDirect3DTexture9> texture;
        HRESULT hr = device_->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
        if (FAILED(hr)) { failure = "CreateTexture " + Hex(hr); return 0; }
        D3DLOCKED_RECT locked{};
        if (FAILED(hr = texture->LockRect(0, &locked, nullptr, 0))) { failure = "LockRect " + Hex(hr); return 0; }
        for (unsigned row = 0; row < height; ++row)
            std::memcpy(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch, bgra + row * width * 4, width * 4);
        texture->UnlockRect(0);
        return reinterpret_cast<ImTextureID>(texture.Detach());
    }
    void Release(ImTextureID texture) override {
        if (texture) reinterpret_cast<IDirect3DTexture9*>(static_cast<std::uintptr_t>(texture))->Release();
    }
private:
    ComPtr<IDirect3DDevice9> device_;
};
}

std::unique_ptr<TextureUploader> D3D9TextureUploader(IDirect3DDevice9* device) {
    return std::unique_ptr<TextureUploader>(new D3D9Uploader(device));
}
} }
