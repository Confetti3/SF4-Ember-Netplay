// Draws red beside blue on a Direct3D 9 device and checks the NV12 bytes the
// grab hands over, the device's render target afterwards, and an odd size.
// Skips (77) where there is no device (a session without a display).
#include "../platform/FrameGrab.hxx"

#include <windows.h>
#include <d3d9.h>
#include <cstdlib>
#include <iostream>

namespace {
int s_failures = 0;
unsigned s_width = 0, s_height = 0;
void Near(const char* what, int got, int want) {
	if (std::abs(got - want) > 2) { std::cerr << what << ": " << got << ", not " << want << "\n"; s_failures++; }
}
// BT.709 at 16 to 235: red is Y 63, U 102, V 240 and blue Y 32, U 240, V 118.
void Check(const void* luma, int lumaPitch, const void* chroma, int chromaPitch) {
	const auto* y = static_cast<const unsigned char*>(luma);
	const auto* uv = static_cast<const unsigned char*>(chroma);
	for (const unsigned row : {0u, s_height / 2, s_height - 1}) {
		Near("red luma", y[row * lumaPitch + 1], 63); Near("red luma", y[row * lumaPitch + s_width / 2 - 2], 63);
		Near("blue luma", y[row * lumaPitch + s_width / 2 + 1], 32); Near("blue luma", y[row * lumaPitch + s_width - 1], 32);
	}
	for (const unsigned row : {0u, s_height / 2 - 1}) {
		const auto* line = uv + row * chromaPitch;
		Near("red U", line[2], 102); Near("red V", line[3], 240);
		Near("blue U", line[s_width - 2], 240); Near("blue V", line[s_width - 1], 118);
	}
}
}

int main() {
	namespace grab = sf4e::platform::grab;
	// 1283x721: three columns and a row more than NV12 can carry.
	const unsigned sourceWidth = 1283, sourceHeight = 721;
	const HWND window = CreateWindowA("STATIC", "frame grab test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	IDirect3DDevice9* device = nullptr;
	D3DPRESENT_PARAMETERS p = {};
	p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD; p.BackBufferFormat = D3DFMT_X8R8G8B8; p.BackBufferWidth = sourceWidth; p.BackBufferHeight = sourceHeight; p.hDeviceWindow = window;
	if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &device))) { std::cout << "No Direct3D 9 device here; skipped\n"; return 77; }

	IDirect3DSurface9* before = nullptr; IDirect3DSurface9* after = nullptr;
	device->GetRenderTarget(0, &before);
	const D3DRECT left = {0, 0, 640, static_cast<LONG>(sourceHeight)}, right = {640, 0, static_cast<LONG>(sourceWidth), static_cast<LONG>(sourceHeight)};
	device->Clear(1, &left, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
	device->Clear(1, &right, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

	if (!grab::Open(device, s_width, s_height)) { std::cerr << "The grab did not open\n"; return 1; }
	if (s_width != 1280 || s_height != 720) { std::cerr << "Size " << s_width << "x" << s_height << ", not 1280x720\n"; return 1; }
	// Twice, the second inside a scene and after a release, as after a device reset.
	if (!grab::Grab(device, Check)) { std::cerr << "The first grab failed\n"; return 1; }
	grab::Release();
	device->BeginScene();
	if (!grab::Grab(device, Check)) { std::cerr << "The grab inside a scene failed\n"; return 1; }
	if (FAILED(device->EndScene())) { std::cerr << "The grab closed the caller's scene\n"; s_failures++; }

	device->GetRenderTarget(0, &after);
	DWORD blend = FALSE;
	device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
	if (after != before || !blend) { std::cerr << "The device's state was not put back\n"; s_failures++; }
	before->Release(); after->Release();
	grab::Release();
	if (device->Release() != 0) { std::cerr << "The grab kept something of the device\n"; s_failures++; }
	d3d->Release();
	if (!s_failures) std::cout << "Frame grab: NV12 bytes, state and release passed\n";
	return s_failures ? 1 : 0;
}
