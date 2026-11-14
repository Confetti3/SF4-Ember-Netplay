#include "sf4e__OverlayWatchedPicture.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include <spdlog/spdlog.h>

namespace sf4e { namespace overlay {
namespace {
// The full copy keeps the target's format for the resolve; the shown one is
// X8R8G8B8, so it samples opaque whatever alpha the game left in an A8R8G8B8
// backbuffer (ImGui blends images by their alpha).
IDirect3DTexture9* s_watchedFull = nullptr, * s_watchedShown = nullptr;
bool s_watchedFailed = false;
}
ImTextureID WatchedPicture(unsigned width, unsigned height) {
	IDirect3DDevice9* device = Dimps::Platform::D3D::staticMethods.GetSingleton()->lpD3DDevice;
	IDirect3DSurface9* target = nullptr, * full = nullptr, * shown = nullptr;
	D3DSURFACE_DESC desc{};
	const bool fresh = !s_watchedShown;
	const bool done = !s_watchedFailed && SUCCEEDED(device->GetRenderTarget(0, &target)) && SUCCEEDED(target->GetDesc(&desc)) &&
		(s_watchedFull || SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &s_watchedFull, nullptr))) &&
		(s_watchedShown || SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &s_watchedShown, nullptr))) &&
		SUCCEEDED(s_watchedFull->GetSurfaceLevel(0, &full)) && SUCCEEDED(s_watchedShown->GetSurfaceLevel(0, &shown)) &&
		SUCCEEDED(device->StretchRect(target, nullptr, full, nullptr, D3DTEXF_NONE)) && SUCCEEDED(device->StretchRect(full, nullptr, shown, nullptr, D3DTEXF_LINEAR));
	for (auto* surface : {target, full, shown}) if (surface) surface->Release();
	// Once per overlay: the format names the backbuffer this picture came from.
	if (done && fresh) spdlog::info("Overlay: watched game's picture {}x{} from its {}x{} frame in format {}", width, height, desc.Width, desc.Height, static_cast<int>(desc.Format));
	// Once: the menu then stays translucent until the overlay is made again.
	// A driver that will not convert the format to X8R8G8B8 ends up here too.
	if (!done && !s_watchedFailed) { s_watchedFailed = true; spdlog::warn("Overlay: no {}x{} picture of the watched game's {}x{} frame in format {}", width, height, desc.Width, desc.Height, static_cast<int>(desc.Format)); }
	return done ? reinterpret_cast<ImTextureID>(s_watchedShown) : ImTextureID{};
}
void FreeWatchedPicture() {
	for (auto** texture : {&s_watchedFull, &s_watchedShown}) if (*texture) { (*texture)->Release(); *texture = nullptr; }
	s_watchedFailed = false;
}
} }
