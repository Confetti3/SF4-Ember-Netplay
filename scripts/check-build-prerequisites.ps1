param([string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE,
      [switch]$WithoutDiscord, [string]$JsonOutput = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
Enter-EmberLocalTools $repo
$checks = [Collections.Generic.List[object]]::new()
function Record-Check([string]$Name, [bool]$Passed, [string]$Detail) {
    $checks.Add([pscustomobject]@{name=$Name;passed=$Passed;detail=$Detail})
}
Record-Check 'PowerShell' ($PSVersionTable.PSVersion.Major -ge 7) "Need PowerShell 7; running $($PSVersionTable.PSVersion)."
Record-Check 'Git' ([bool](Get-Command git -ErrorAction SilentlyContinue)) 'Git must be on PATH.'
$node = Get-Command $(if ($env:SF4E_NODE) { $env:SF4E_NODE } else { 'node' }) -ErrorAction SilentlyContinue
if ($node) {
    & $node.Source -e 'const [a,b]=process.versions.node.split(".").map(Number); if(a<22 || (a===22 && b<18)) process.exit(1)'
    Record-Check 'Node.js' ($LASTEXITCODE -eq 0) 'SDK validation needs Node 22.18 or later; set SF4E_NODE to its executable.'
} else { Record-Check 'Node.js' $false 'Install Node 22.18 or later, or set SF4E_NODE.' }
Record-Check 'npm' ([bool](Get-Command npm -ErrorAction SilentlyContinue)) 'npm is required for the locked SDK build.'
try {
    $target = Get-EmberBuildTarget $repo -WithoutDiscord:$WithoutDiscord
    Record-Check 'Build target' $true "$($target.buildDirectory); stage: $($target.installDirectory)"
} catch { Record-Check 'Build target' $false $_.Exception.Message }
try {
    $tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
    Record-Check 'C++ and vcpkg' $true "$($tools.VisualStudioPath); $($tools.VcpkgRoot)"
    foreach ($entry in @(
        @('CMake', 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'),
        @('Ninja', 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'))) {
        $path = Join-Path $tools.VisualStudioPath $entry[1]
        Record-Check $entry[0] (Test-Path -LiteralPath $path -PathType Leaf) $path
    }
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Lib'
    $sdk = @(Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Test-Path -LiteralPath (Join-Path $_.FullName 'um/x86/kernel32.lib')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt/x86/ucrt.lib')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um/x64/kernel32.lib')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt/x64/ucrt.lib')) })
    Record-Check 'Windows SDK' ($sdk.Count -gt 0) 'Need x86 and x64 Windows SDK and UCRT libraries.'
} catch { Record-Check 'C++ and vcpkg' $false $_.Exception.Message }
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python -and $python.Source -notlike '*\Microsoft\WindowsApps\*') {
    $version = & $python.Source -c 'import sys; print(sys.version.split()[0]); sys.exit(0 if sys.version_info.major == 3 else 1)' 2>&1
    Record-Check 'Python 3' ($LASTEXITCODE -eq 0) "$($python.Source): $version"
} else {
    Record-Check 'Python 3' $false 'Install Python 3 or set SF4E_PYTHON to its python.exe; a Windows Store alias is not an installed interpreter.'
}
$pin = Get-Content -LiteralPath (Join-Path $repo 'rust/sf4-net/rust-toolchain.toml') -Raw
if ($pin -notmatch 'channel\s*=\s*"([^"]+)"') { throw 'Missing Rust toolchain pin' }
$channel = $Matches[1]
if (Get-Command rustup -ErrorAction SilentlyContinue) {
    foreach ($tool in @('rustc','cargo','rustfmt','clippy-driver')) {
        $version = & rustup run $channel $tool --version 2>&1
        Record-Check "Rust $tool" ($LASTEXITCODE -eq 0) "$version"
    }
    $targets = @(& rustup target list --installed --toolchain $channel 2>&1)
    Record-Check 'Rust Windows target' ($LASTEXITCODE -eq 0 -and $targets -contains 'x86_64-pc-windows-msvc') "Need x86_64-pc-windows-msvc for Rust $channel."
} else { Record-Check 'Rust' $false "Install Rust $channel with rustup (including rustfmt and clippy)." }
if ($WithoutDiscord) {
    Record-Check 'Discord SDK' $true 'Omitted explicitly; this development build cannot be packaged as a complete release.'
} else {
    $discordPin = Get-Content -LiteralPath (Join-Path $repo 'cmake/discord-sdk-pin.json') -Raw | ConvertFrom-Json
    $valid = $DiscordSdkArchive -and (Test-Path -LiteralPath $DiscordSdkArchive -PathType Leaf)
    if ($valid) { $valid = (Get-FileHash -LiteralPath $DiscordSdkArchive -Algorithm SHA256).Hash -eq $discordPin.sha256 }
    Record-Check 'Discord SDK' ([bool]$valid) "Need the hash-verified $($discordPin.archive), or specify -WithoutDiscord."
}
$checks | Format-Table name,passed,detail -Wrap
$passed = @($checks | Where-Object { !$_.passed }).Count -eq 0
if ($JsonOutput) {
    [ordered]@{passed=$passed;withoutDiscord=[bool]$WithoutDiscord;checks=@($checks.ToArray())} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $JsonOutput -Encoding utf8
}
if (!$passed) { exit 1 }
exit 0
