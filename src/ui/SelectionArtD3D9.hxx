#pragma once
#include "SelectionArt.hxx"
#include <memory>

struct IDirect3DDevice9;

namespace sf4e { namespace ui {
// SelectionArt's textures on a D3D9 device: managed A8R8G8B8, one level. Holds
// a reference to the device until the uploader is gone.
std::unique_ptr<TextureUploader> D3D9TextureUploader(IDirect3DDevice9* device);
} }
