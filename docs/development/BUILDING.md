# Build SF4 Ember Netplay

The `release` branch contains Ember. The `main` branch preserves the legacy launcher.
Build and package locally; pushing a tag does not trigger a second build or publish assets.

## Requirements

- Windows x64 and PowerShell 7 (`pwsh`).
- Visual Studio 2026 C++ Build Tools with x86/x64 compilers, CMake, Ninja and the Windows SDK.
- Git, Python 3 and a bootstrapped vcpkg checkout. The manifest and overlay ports pin the dependency inputs.
- Rust through rustup; `rust/sf4-net/rust-toolchain.toml` pins the toolchain.
- The official Discord Social SDK archive identified by `cmake/discord-sdk-pin.json`. Obtain it through Discord's developer portal; configuration verifies its SHA-256. The SDK archive is not stored in this repository.

## Compile, test and stage

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

## Package

```powershell
pwsh -NoProfile -File ./scripts/package-team.ps1 -VersionLabel 0.8.3
```

This creates `dist/sf4-ember-netplay-0.8.3.zip` and its `.sha256` sidecar. It requires the designated source, fresh build receipt and matching staged binaries, collects notices, validates the package inventory and runs preflight. Existing package destinations are never overwritten.

Every published release also includes a smaller incremental package for users of the previous release. `scripts/package-upgrade.ps1` compares the complete previous and target manifests, includes only changed product files, preserves unrelated user files, and exercises both check-only and real upgrade paths against temporary extraction of the exact previous package. The installer verifies the old installation, payload, reconstructed target and backup before completing.

## Publish a release

Commit the final source, README and screenshots before the final build. Build from that exact commit, review the package, then push the `release` branch and a version tag pointing to that commit. Keep legacy `main` unchanged. Keep the verified previous full package and checksum in `dist`, then use `scripts/github-release.ps1 -Tag v0.8.3`. It packages a fresh complete ZIP, constructs and validates the mandatory previous-version upgrade ZIP, and uploads both with SHA-256 sidecars. The script refuses dirty source, mismatched tags, non-published base packages and existing releases.

Public release notes must distinguish local tests, helper network tests and observed gameplay. A package or test pass alone is not a clean-machine or two-PC gameplay result.
