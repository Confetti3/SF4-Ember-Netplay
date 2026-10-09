# Build SF4 Ember Netplay

The `release` branch contains Ember. The `main` branch preserves the legacy launcher.
Build and package locally; pushing a tag does not trigger a second build or publish assets.

## Requirements

- Windows x64 and PowerShell 7 (`pwsh`).
- Visual Studio 2026 C++ Build Tools with x86/x64 compilers, CMake, Ninja and the Windows SDK.
- Git, Python 3 and a bootstrapped vcpkg checkout. The manifest and overlay ports pin the dependency inputs.
- Rust through rustup; `rust/sf4-net/rust-toolchain.toml` pins the toolchain.
- Node.js 22.18 or later and npm for the locked SDK build and local bridge tests.
- The official Discord Social SDK archive identified by `cmake/discord-sdk-pin.json`. Obtain it through Discord's developer portal; configuration verifies its SHA-256. The SDK archive is not stored in this repository.

## Compile, test and stage

Check all prerequisites before starting a build:

```powershell
pwsh -NoProfile -File ./scripts/check-build-prerequisites.ps1 -WithoutDiscord
```

For development without the proprietary Discord SDK, run:

```powershell
pwsh -NoProfile -File ./scripts/build-current.ps1 -WithoutDiscord
```

This uses a separate `build/current-no-discord` directory and stage, omits the
Discord companion, and records `discordEnabled: false` in its receipt. It is
not a complete release package; the release packaging requirements still apply.
Remove `-WithoutDiscord` and supply the pinned SDK archive for a complete build.

Both commands find vcpkg bundled with Visual Studio when no explicit vcpkg path
is set. A project-local Rust installation under `build/tools/cargo` and
`build/tools/rustup` is used when present, without changing the machine PATH.
Set `SF4E_PYTHON` to an existing `python.exe` if Python 3 is not on PATH.
Set `SF4E_NODE` to a supported `node.exe` when another Node version is on PATH.
The prerequisite checker accepts `-JsonOutput <path>` to retain its results.

The game compatibility fingerprint and startup protocol are documented in
[Game compatibility](GAME_COMPATIBILITY.md).

From the root of a clone of the `release` branch:

```powershell
pwsh -NoProfile -File ./scripts/build-current.ps1 `
  -VcpkgRoot C:/dev/vcpkg `
  -DiscordSdkArchive C:/downloads/DiscordSocialSdk-1.10.19337.zip
```

Visual Studio is discovered with `vswhere`; override it with `-VisualStudioPath`.
`VCPKG_ROOT`, `SF4E_VISUAL_STUDIO_PATH` and `SF4E_DISCORD_SDK_ARCHIVE` can supply these paths through the environment.

The repository's `build-target.json` selects `build/current` and `build/current/stage`.
Inside a managed multi-checkout workspace, a parent `build-target.json` takes precedence and must designate this checkout. The guard rejects another checkout and rejects build paths outside the source root.

The script builds the x86 launcher, sidecar and updater, the x64 Iroh helper and Discord companion, runs local CTests, and stages the product. It checks source identity, patched GGPO dependencies and staged hashes, then writes `build/current/build-provenance.json`. Staging does not install into the game.

`-SkipTests` is available for development; packaging rejects a receipt without passing tests.
The local suite excludes public-network and live Discord tests. Explicit helper network testing is available through `scripts/test-current-network.ps1`; it does not establish actual SF4 gameplay acceptance.

The local gate also runs the standalone short-link and room-supervisor suites,
compiles the TypeScript SDK with `npm ci`, and requires its bridge integration
tests against a locally built bridge. It writes `local-tests.xml` (JUnit) beside
the build receipt. CTest's detailed log retains Rust's internal ignored-test
counts; the receipt's excluded expression identifies the external suites not run.
CI separately tests all four Rust components on Linux and Windows, SDK integration,
Linux room-host IPC, and dependency advisories. Those jobs do not build the native
game product or establish gameplay acceptance. Run `scripts/test-sdk.ps1` to check
the SDK alone. Package assembly relocates and validates Markdown file links.

## Package

```powershell
pwsh -NoProfile -File ./scripts/package-team.ps1 -VersionLabel 0.8.3
```

This creates `dist/sf4-ember-netplay-0.8.3.zip` and its `.sha256` sidecar. It requires the designated source, fresh build receipt and matching staged binaries, collects notices, validates the package inventory and runs preflight. Existing package destinations are never overwritten.

Every published release also includes a smaller incremental package for users of the previous release. `scripts/package-upgrade.ps1` compares the complete previous and target manifests, includes only changed product files, preserves unrelated user files, and exercises both check-only and real upgrade paths against temporary extraction of the exact previous package. The installer verifies the old installation, payload, reconstructed target and backup before completing.

Fresh installations use a per-user installer built from the same verified package folder:

```powershell
pwsh -NoProfile -File ./scripts/package-installer.ps1 -PackageDir dist/sf4-ember-netplay-0.8.3 -VersionLabel 0.8.3
```

This creates `dist/sf4-ember-netplay-0.8.3-setup.exe` and its `.sha256` sidecar. The first run downloads the Inno Setup compiler named by `scripts/installer/innosetup-pin.json`, verifies its SHA-256 and keeps a private copy under `build/tools`; nothing is installed system-wide (`SF4E_ISCC` names another `ISCC.exe` instead). It bundles the Visual C++ x86 runtime installer of the building toolset. The installer needs no administrator rights, places the package under `%LOCALAPPDATA%\Programs`, and installs the runtime only when the present one is older. It only places the first copy and refuses a folder that already has files in it; updates remain with `Updater.exe` and the ZIP packages. Its uninstaller runs `Updater.exe -InstallDir <folder> -Uninstall`, which removes the files the package inventory names or allows (including any a later update added) and the updater's own state, and leaves the player's files. `scripts/test-installer.ps1 -Installer <setup.exe>` exercises the refusal, the installation and the uninstall in a temporary folder.

## Publish a release

Commit the final source, README and screenshots before the final build. Build from that exact commit, review the package, then push the `release` branch and a version tag pointing to that commit. Keep legacy `main` unchanged. Keep the verified previous full package and checksum in `dist`, then use `scripts/github-release.ps1 -Tag v0.8.3`. It packages a fresh complete ZIP, constructs and validates the mandatory previous-version upgrade ZIP, builds the installer, and uploads all three with SHA-256 sidecars. The script refuses dirty source, mismatched tags, non-published base packages and existing releases.

A tag such as `v0.8.3-rc1` or `v0.8.3-beta2` publishes a pre-release: the complete ZIP and the installer, no upgrade ZIP, and the release is not marked latest. The CMake product version stays `0.8.3`. The updater offers it on the **Pre-release** channel only. The channel follows the installed version until the player chooses one in the updater, so a pre-release install checks the pre-release channel and is offered the finished `v0.8.3` when it is published. Stable offers finished releases only; to a pre-release install it offers the latest finished release as the way back, and the installed updater makes the folder exactly that package, removing what only the newer version had, under the same lock and journal as an update.

Public release notes must distinguish local tests, helper network tests and observed gameplay. A package or test pass alone is not a clean-machine or two-PC gameplay result.
