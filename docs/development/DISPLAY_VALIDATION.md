# Display override validation

The launcher stores its display preference in Ember settings and passes a per-launch selection to Sidecar. The game still renders a 16:9 image: on a 3440×1440 monitor, automatic Borderless uses 2560×1440 plus side bars. This does not extend the gameplay camera or HUD to ultrawide.

## Native boundaries

The Steam build 834219 executable was inspected directly when addressing PR #52. The present-parameter builder at 0x7719d0 reads device width +0x258, height +0x25c, refresh +0x260 and fullscreen +0x27c; the adapter ordinal is +0x08. The existing config writer at 0x77d5f0 reads Main +0xe8, +0xec, +0xf0 and +0x10c. The setter at 0x77ff30 copies 13 dwords to Main +0xdc and marks +0x114 dirty. Brightness application at 0x68f8e0 copies a 13-dword device block and calls the framework setter through its vtable.

Consequently, restoring the device only at Destroy cannot protect the saved config. The existing Main::WriteDisplayConfig hook now temporarily supplies the captured native width, height, refresh and fullscreen values for the write, then restores the live override. Other fields, including brightness and the existing Faster display protection, retain their owners. This is static executable evidence plus fixture coverage; it is not a runtime trace of the native save path.

The display owner publishes one immutable plan. Window code owns styles, position and bars; graphics hooks own device fields. A failed post/style/position operation cancels interception immediately, suppresses further applies and restores device fields on the next graphics hook before a native reset. The window thread does not write live graphics fields.

Windowed frame sizing queries a hidden per-monitor-aware window on the destination monitor, accounts for the game's awareness and verifies the resulting physical client rectangle after moving. A bounded correction handles Windows changing the non-client frame during a monitor transition. See Microsoft's [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow) and [AdjustWindowRectExForDpi](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-adjustwindowrectexfordpi) contracts.

## Automated checks

DisplaySettings covers resolution/mode selection, unsupported fallback, environment decoding and atomic settings persistence. LauncherHomeLinks uses a second process and private section names to deliver room, match, public-room, connect, replay and Discord launch intent, checks rejection of invalid slots, consumes each once and verifies receiver cleanup. DisplayConfigPersistence simulates the copied framework values and verifies what both options-save and shutdown writes see, including restoration after an exception.

Desktop integration tests are explicit so a headless build does not silently claim them:

    cmake -S . -B <configured-build> -DSF4E_DISPLAY_WINDOW_TESTS=ON
    cmake --build <configured-build>
    ctest --test-dir <configured-build> -R "^DisplayWindow-" --output-on-failure

These create hidden windows and D3D9 devices. Variants cover native-off, borderless, unaware windows, lower render resolution, windowed, missing monitor, secondary monitor, and post/style/position failures. Secondary-monitor cases report a skip when that hardware is absent; a skip is not acceptance evidence. Existing installed settings and the game executable are not changed.

## Manual acceptance still required

- Open a room link and accept a Discord invitation while Home or its Display page is open, including while minimized. The game should start with its existing confirmation/SDK flow.
- On a 150% and 200% desktop, verify the actual Windowed client size for an unaware game. Repeat from a primary monitor to a secondary monitor with a different scale.
- Confirm borderless sharpness, focus, Alt+Tab, bars, and available exclusive modes.
- Record native DISPLAY values, launch an override with different dimensions, reach the native menus, change another graphics option, exit through the game's menu, and verify the saved native resolution/refresh/fullscreen values.
- Repeat mode changes under DXVK/Wine/Proton. Native Windows fixtures alone do not establish compatibility there.
