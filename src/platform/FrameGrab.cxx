#include "FrameGrab.hxx"

#include <windows.h>
#include <d3d9.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace {
// The two passes of FrameGrab.hlsl, as the build compiled them.
#include "FrameGrabLuma.h"
#include "FrameGrabChroma.h"

UINT s_sourceWidth = 0, s_sourceHeight = 0, s_width = 0, s_height = 0;
D3DFORMAT s_format = D3DFMT_UNKNOWN;
IDirect3DPixelShader9* s_shaders[2] = {};
IDirect3DTexture9* s_copy = nullptr;
// Luma then chroma: the pass's target on the card, twice over, and where it
// is read to. A frame is drawn into one pair while the pair drawn a frame ago
// is read, which the card has finished by then: reading the pair just drawn
// would hold the game until the card caught up.
IDirect3DSurface9* s_planes[2][2] = {}; IDirect3DSurface9* s_read[2] = {};
// The pair this frame draws into, and whether the other holds a frame.
int s_turn = 0;
bool s_drawn = false;
IDirect3DStateBlock9* s_state = nullptr;

template <class T> void Drop(T*& object) { if (object) { object->Release(); object = nullptr; } }

bool Make(IDirect3DDevice9* device) {
	if (s_state) return true;
	bool made = SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(kFrameGrabLuma), &s_shaders[0])) &&
		SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(kFrameGrabChroma), &s_shaders[1])) && SUCCEEDED(device->CreateTexture(s_sourceWidth, s_sourceHeight, 1, D3DUSAGE_RENDERTARGET, s_format, D3DPOOL_DEFAULT, &s_copy, nullptr));
	for (int plane = 0; plane < 2 && made; plane++) {
		const UINT height = s_height >> plane;
		made = SUCCEEDED(device->CreateRenderTarget(s_width / 4, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_planes[0][plane], nullptr)) &&
			SUCCEEDED(device->CreateRenderTarget(s_width / 4, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_planes[1][plane], nullptr)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(s_width / 4, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s_read[plane], nullptr));
	}
	made = made && SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &s_state));
	if (!made) { spdlog::warn("Frame grab: the device made no surfaces for {}x{}", s_width, s_height); sf4e::platform::grab::Release(); }
	return made;
}
}

namespace sf4e { namespace platform { namespace grab {

void Release() {
	Drop(s_state); Drop(s_copy);
	for (int plane = 0; plane < 2; plane++) { Drop(s_planes[0][plane]); Drop(s_planes[1][plane]); Drop(s_read[plane]); Drop(s_shaders[plane]); }
	s_drawn = false;
}

bool Open(IDirect3DDevice9* device, unsigned& width, unsigned& height) {
	Release();
	IDirect3DSurface9* target = nullptr;
	D3DSURFACE_DESC desc = {};
	if (FAILED(device->GetRenderTarget(0, &target))) return false;
	target->GetDesc(&desc);
	target->Release();
	if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) { spdlog::warn("Frame grab: the render target's format {} is not BGRA", static_cast<int>(desc.Format)); return false; }
	s_sourceWidth = desc.Width; s_sourceHeight = desc.Height; s_format = desc.Format;
	// Four luma bytes to a pixel, two rows to a chroma row: up to three columns and a row are left off.
	width = s_width = desc.Width & ~3u; height = s_height = desc.Height & ~1u;
	return width && height && Make(device);
}

bool Grab(IDirect3DDevice9* device, Sink sink) {
	IDirect3DSurface9* target = nullptr; IDirect3DSurface9* depth = nullptr; IDirect3DSurface9* copy = nullptr;
	D3DSURFACE_DESC desc = {};
	if (FAILED(device->GetRenderTarget(0, &target))) return false;
	target->GetDesc(&desc);
	bool done = desc.Width == s_sourceWidth && desc.Height == s_sourceHeight && Make(device) && SUCCEEDED(s_copy->GetSurfaceLevel(0, &copy)) &&
		// Onto a texture the shaders can sample; this also resolves a multisampled target.
		SUCCEEDED(device->StretchRect(target, nullptr, copy, nullptr, D3DTEXF_NONE));
	if (done) {
		s_state->Capture();
		device->GetDepthStencilSurface(&depth);
		// Inside the caller's scene when it has one open, which then refuses a second.
		const bool scene = SUCCEEDED(device->BeginScene());
		device->SetDepthStencilSurface(nullptr);
		device->SetVertexShader(nullptr);
		device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		device->SetTexture(0, s_copy);
		const DWORD off[] = {D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE};
		for (const DWORD state : off) device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), FALSE);
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		const float pixel[4] = {1.0f / s_sourceWidth, 1.0f / s_sourceHeight, 0, 0};
		device->SetPixelShaderConstantF(0, pixel, 1);
		for (int plane = 0; plane < 2; plane++) {
			// Luma takes each pixel as it is; chroma the average of four.
			const DWORD filter = plane ? D3DTEXF_LINEAR : D3DTEXF_POINT;
			device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
			device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
			device->SetRenderTarget(0, s_planes[s_turn][plane]);
			device->SetPixelShader(s_shaders[plane]);
			// Direct3D 9 puts a pixel's middle half a pixel off its position.
			const float right = s_width / 4 - 0.5f, bottom = (s_height >> plane) - 0.5f;
			const float u = static_cast<float>(s_width) / s_sourceWidth, v = static_cast<float>(s_height) / s_sourceHeight;
			const float quad[4][6] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {right, -0.5f, 0, 1, u, 0}, {-0.5f, bottom, 0, 1, 0, v}, {right, bottom, 0, 1, u, v}};
			done = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0])) && done;
		}
		if (scene) device->EndScene();
		device->SetRenderTarget(0, target);
		device->SetDepthStencilSurface(depth);
		s_state->Apply();
		D3DLOCKED_RECT luma = {}, chroma = {};
		IDirect3DSurface9** const before = s_planes[1 - s_turn];
		if (done && s_drawn) {
			done = SUCCEEDED(device->GetRenderTargetData(before[0], s_read[0])) && SUCCEEDED(device->GetRenderTargetData(before[1], s_read[1])) &&
				SUCCEEDED(s_read[0]->LockRect(&luma, nullptr, D3DLOCK_READONLY));
			if (done) {
				done = SUCCEEDED(s_read[1]->LockRect(&chroma, nullptr, D3DLOCK_READONLY));
				if (done) { sink(luma.pBits, luma.Pitch, chroma.pBits, chroma.Pitch); s_read[1]->UnlockRect(); }
				s_read[0]->UnlockRect();
			}
		}
		s_drawn = done;
		s_turn = 1 - s_turn;
	}
	Drop(depth); Drop(copy); Drop(target);
	return done;
}

} } }
