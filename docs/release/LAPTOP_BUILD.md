# Building releases on the Linux build PC

Stable and Nightly builds compile and test on a Linux build PC (the laptop) with the clang-cl cross toolchain.
The Windows PC only packages and publishes what that build made, and nothing compiles there. SFV Ember's releases
have been built this way since 0.1.2 (its `docs/release/PUBLISHING.md`, "The build PC").

## What runs where

| Step | Where | What |
| --- | --- | --- |
| Build, tests, staging, symbols | build PC | `scripts/build-current.ps1 -CompilerCache -Symbols` under `pwsh` on Linux |
| Import, Windows-only tests | Windows PC | `scripts/import-build.ps1 -BuildHost <host> -RemoteSource <checkout>` (a few minutes, no compile) |
| Package, installer, upload | Windows PC | `scripts/github-release.ps1` (Stable) or `publish-nightly.ps1` (Nightly), unchanged |

## The build PC

`build-current.ps1` on Linux is the cross build. clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt compile against
the Microsoft CRT and Windows SDK that [xwin](https://github.com/Jake-Shadle/xwin) splats (`cmake/cross`). It has
the same options, staging, tests and receipt as the Windows build, and the receipt names the toolchain (`toolchain`:
compiler, CRT, SDK, splat).

- `cmake/cross/clang-cl-x86.cmake` covers the game's modules, Launcher, the tests and vcpkg's ports.
  `clang-cl-x64.cmake` covers the Discord companion and its fixture (`cmake/sf4e_x64_subbuild.cmake`). Both include
  `clang-cl.cmake`.
- `_MSC_VER` is the splat CRT's toolset (`crtversion.h`), as MSVC's would be, so the Launcher's runtime check
  (`RuntimeIsCurrent`) asks for at least that runtime.
- vcpkg builds the ports with the repository's own triplet, `x86-windows-wchar-filenames`, with the cross toolchain
  chainloaded and release packages only (`build-current-dependencies.ps1`). The ports carry debug information, so
  the release's Breakpad symbols cover GGPO, spdlog, fmt and zlib as on Windows. Detours builds with CMake from
  `cmake/cross/vcpkg-ports` (its own port needs nmake); the port carries a patch for a null-module walk that clang
  otherwise folds into a trap.
- The Rust helper builds for `x86_64-pc-windows-msvc` through `cmake/cross/rust-wrappers.sh` (linker, C
  compiler, rc.exe, ml64.exe). Its tests run through the test emulator (`CARGO_TARGET_..._RUNNER`).
- `-CompilerCache` is sccache here. It covers the project, the ports (`SF4E_COMPILER_LAUNCHER`) and the helper
  (`RUSTC_WRAPPER`), and the build log ends with its statistics.
- Tests run through `SF4E_TEST_EMULATOR` (Proton's Wine; DXVK for Direct3D, so a desktop session must be up:
  `DISPLAY`/`WAYLAND_DISPLAY`). `-Symbols` uses the native `dump_syms`. The helper's Rust tests run with
  `RUST_TEST_THREADS=6`: at one thread per processor, Wine starves its timed network tests, which pass alone.
- Tests that need Windows itself carry the CTest label `windows_host` (`cmake/sf4e_test_targets.cmake`).
  These are the publisher's PowerShell suites, the Windows PowerShell checks, and programs whose subject Wine
  implements differently: denied file access, WinHTTP cancel timing, minidump streams, file names and locks, and
  Media Foundation. The build PC leaves them out and requires the rest, including HelperRust, EmberRust and
  BridgeSdk. It writes CTest's list of them to `windows-host-tests.json`, and the receipt marks them pending.

Environment (`~/sf4/env.sh` on the laptop sets most of it):

| Variable | Meaning |
| --- | --- |
| `SF4E_XWIN` | the xwin splat (x86 and x86_64, desktop variant) |
| `SF4E_LLVM_BIN` | LLVM's bin folder (clang-cl, lld-link, llvm-lib, llvm-rc, llvm-mt, llvm-ml) |
| `VCPKG_ROOT` | vcpkg |
| `SF4E_TEST_EMULATOR` | runs a Windows program (`~/sf4/bin/ember-wine`) |
| `SF4E_DISCORD_SDK_ARCHIVE` | the pinned Discord SDK archive |
| `NUMBER_OF_PROCESSORS`, `SF4E_TEST_JOBS` | compile and test jobs |

### The checkout

The checkout must have the Windows PC's bytes, because the import compares source fingerprints. Clone it with
`core.autocrlf true` and put a copy of the Windows workspace's `build-target.json` (same bytes) in its parent
folder. `cmake/cross` stays LF in every checkout (`.gitattributes`), because the Detours patch applies to LF
sources. On the laptop, the publish checkout is `/mnt/games/sf4-publish/sf4-current`, and
`/mnt/games/sf4-publish/build.sh <log> [args]` runs `build-current.ps1` there with the environment above.

## The import

`import-build.ps1` brings the build PC's build into the designated build directory here, so packaging runs as for a
local build. It refuses unless all of these hold:

- the receipt is a cross build of this checkout's exact commit and branch;
- the source fingerprint matches;
- the tests passed;
- there is no self-test.

It copies the stage, the receipt, the symbol store, the dependency files the receipt and the notices name, and
`PackageInstallerTest.exe`. It then rewrites the receipt's paths to this checkout's and records the copy under
`import`: the host, its paths and the SHA-256 of its receipt. The original receipt is kept as
`build-provenance.buildpc.json` and the build PC's cache as `CMakeCache.buildpc.txt`. Finally it runs
`Assert-BuildReceipt`, which checks every staged binary's hash, the dependency artifacts and the symbol store.

Before that it runs the `windows_host` tests here, one at a time, against the build PC's own programs, and records each
result in the receipt under `import.windowsHostTests`. Each command is adapted to this PC:

- the emulator is left out;
- the paths are this checkout's;
- PowerShell and Bash are the local ones;
- `SF4E_SOURCE_ROOT` names the checkout for tests that look for their sources.

A test that fails runs once more, as on the build PC. A test whose child process outlives it is cleaned up.
Packaging refuses a cross build whose `windows_host` tests have not all passed here.

The upgrade tests (PackageFiles, UpgradeRecoveryPowerShell) check, as the upgrade does, that Ultra Street Fighter IV is
not running. Import while the game is closed.

A local build tree is replaced only with `-ReplaceBuildTree`.
