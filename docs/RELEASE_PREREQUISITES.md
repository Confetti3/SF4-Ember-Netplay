## Prerequisites (install on each PC before playing)

**These are not included in the zip.** Install once per machine:

| Requirement | Why | Download |
|-------------|-----|----------|
| **Ultra Street Fighter IV** (Steam app **45760**) | The game this launcher hooks into | [Steam](https://store.steampowered.com/app/45760/) |
| **Microsoft Edge WebView2 Runtime** | Launcher UI (Host / Join / Offline) | [WebView2](https://go.microsoft.com/fwlink/p/?LinkId=2124703) |
| **Microsoft Visual C++ Redistributable (x86)** | `Launcher.exe`, `Sidecar.dll`, and relay binaries | [VC++ x86](https://aka.ms/vs/17/release/vc_redist.x86.exe) |

**OS:** Windows 10 or later (64-bit Windows; the launcher is **32-bit/x86** to match USF4).

**After you extract the zip:**

1. Run **`preflight.cmd`** in the folder (checks WebView2, VC++, and that all files are present).
2. Run **`Launcher.exe`**.

**Netplay:** Both players must download the **same release zip** and use the same version (`BUILD_INFO.txt` Git line must match). Do not mix builds.

**Broker (Simple mode):** Default URL is `https://74-208-200-95.nip.io` (set in launcher Advanced if needed).
