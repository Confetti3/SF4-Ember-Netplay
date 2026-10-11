# Drawing the game through Vulkan (DXVK)

`SSFIV.exe` draws with Direct3D 9. [DXVK](https://github.com/doitsujin/dxvk) is
a Direct3D 9 implementation on top of Vulkan (zlib licence). It is what Proton
already uses for this game on Linux. With `SF4E_VULKAN=1`, Sidecar makes the
game use DXVK on Windows too, or under a Wine that does not already do so.

The simulation is not involved. The game still issues the same Direct3D 9
calls; only the library that carries them out changes.

## How to turn it on

1. Download a DXVK release and take the 32-bit `d3d9.dll` from its `x32`
   folder.
2. Put it beside `Sidecar.dll` in the Ember folder, renamed to
   `dxvk_d3d9.dll`.
3. Set `SF4E_VULKAN=1` and start `Launcher.exe` from the same prompt.

The log says which one is in use:

```
Display: drawing through Vulkan (DXVK, C:\...\dxvk_d3d9.dll)
```

or, when the file is missing, is not a 32-bit DXVK, or there is no usable
Vulkan driver:

```
Display: SF4E_VULKAN is set but ... could not be loaded (Win32 126); using the system's Direct3D 9
```

DXVK is not part of the Ember package. Shipping it means adding a third-party
binary to the signed package and its licence to the notices, which is a
separate decision.

## What Sidecar does

The game calls `Direct3DCreate9` once, from the import it has on `d3d9.dll`.
When `SF4E_VULKAN=1`, `fD3D::Install` (`src/sf4e/sf4e__Platform.cxx`) detours
that export. The replacement loads `dxvk_d3d9.dll` from Sidecar's own folder
and returns DXVK's `IDirect3D9`; every later call the game makes (the device,
`Reset`, `Present`) then goes to DXVK through that object. If the library or
its `Direct3DCreate9` fails, the system's function is called instead, so the
worst case is the game drawing as it does today.

Nothing is written to the game folder and `SSFIV.exe` is not changed. The
file has a name of its own because a `d3d9.dll` in the Ember folder would be
picked up by the launcher and by anything else that imports Direct3D 9.

Sidecar's own hooks sit on the game's functions (`Reset`, `RunScene_Render`,
`BuildPresentParameters`, the frame limiter), not on Direct3D, and the overlay
draws through the device the game created, so they apply to DXVK's device the
same way.

## What has been checked

On Windows 11, RTX 3070, driver 591.86, DXVK 3.1.1, with the same redirect done
by a stand-in `d3d9.dll` beside the game instead of by Sidecar:

- the game starts, reaches a Training fight and runs at 60 fps;
- DXVK's own log shows a Vulkan swapchain with
  `VK_PRESENT_MODE_IMMEDIATE_KHR`;
- a capture of the same stage with and without DXVK differs by a mean of 0.33
  out of 255 on flat stage surfaces (the two captures were not taken on the
  same frame, so characters and effects differ).

## Not checked

- The redirect as built into Sidecar, started through the launcher.
- The overlay and the launcher-side screens on DXVK's device.
- A netplay match.
- Wine and Proton. Proton already runs the game on DXVK, so the switch should
  change nothing there.
- Switching between windowed and fullscreen: with the stand-in, one switch
  back from fullscreen left the window without a border.
- Frame pacing compared with Direct3D 9.
