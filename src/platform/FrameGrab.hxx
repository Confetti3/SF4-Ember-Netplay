#pragma once

struct IDirect3DDevice9;

// Reads a Direct3D 9 device's render target as NV12 (BT.709, 16 to 235), the
// encoder's own format (VideoEncoder.hxx). The conversion is two pixel shader
// passes on the graphics card into targets whose bytes are the planes: one
// BGRA pixel holds four luma bytes, or two chroma pairs. What crosses to
// system memory is then 1.5 bytes a pixel instead of BGRA's 4, which is what
// makes a 4K picture affordable in time (the readback) and in memory.
namespace sf4e { namespace platform { namespace grab {

// The size Grab will hand over for the device's render target: its width
// down to a multiple of four and its height to an even number. False when
// the target is not 32-bit BGRA or the device made no shaders or surfaces.
bool Open(IDirect3DDevice9* device, unsigned& width, unsigned& height);
// The render target as it is now, to sink(luma, lumaPitch, chroma, chromaPitch).
// The device's state is as before on return. False when the target's size is
// no longer the one Open saw.
typedef void (*Sink)(const void* luma, int lumaPitch, const void* chroma, int chromaPitch);
bool Grab(IDirect3DDevice9* device, Sink sink);
// Lets go of everything on the device; before it resets or goes. Grab makes it again.
void Release();

} } }
