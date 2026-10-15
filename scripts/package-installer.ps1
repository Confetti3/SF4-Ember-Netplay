# Wrap an already verified package folder in the per-user installer. Never rebuild or repackage here.
param([Parameter(Mandatory=$true)][string]$PackageDir, [Parameter(Mandatory=$true)][string]$VersionLabel,
      [string]$OutDir = 'dist', [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$InnoCompiler = $env:SF4E_ISCC, [string[]]$InnoDefines = @())
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
if ($VersionLabel -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid version label' }
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path.TrimEnd('\','/')
if (!(Test-Path -LiteralPath (Join-Path $PackageDir 'MANIFEST.txt') -PathType Leaf)) { throw "Not a package folder: $PackageDir" }
& (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $PackageDir -Strict
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Package preflight failed' }
# The compiler is pinned like the other build inputs: a private copy under
# build\tools, fetched once and checked against installer\innosetup-pin.json,
# the download on arrival and the kept ISCC.exe on every run.
# /PORTABLE=1 leaves no uninstall entry, shortcut or file association.
if (!$InnoCompiler) {
    $pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installer\innosetup-pin.json') -Raw | ConvertFrom-Json
    $tool = Join-Path $repo "build\tools\innosetup-$($pin.version)"
    $InnoCompiler = Join-Path $tool 'ISCC.exe'
    $cached = (Test-Path -LiteralPath $InnoCompiler -PathType Leaf) -and (Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -eq $pin.isccSha256
    if (!$cached) {
        if (Test-Path -LiteralPath $tool) { Remove-Item -LiteralPath $tool -Recurse -Force }
        $download = Join-Path ([IO.Path]::GetTempPath()) "innosetup-$($pin.version)-$PID.exe"
        try {
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $pin.url -OutFile $download -UseBasicParsing
            if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $pin.sha256) { throw 'The Inno Setup download does not match installer\innosetup-pin.json' }
            $setup = Start-Process -FilePath $download -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/PORTABLE=1',"/DIR=`"$tool`"" -Wait -PassThru
            if ($setup.ExitCode -ne 0) { throw "Inno Setup installation exited with $($setup.ExitCode)" }
        } finally { Remove-Item -LiteralPath $download -ErrorAction SilentlyContinue }
        if ((Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -ne $pin.isccSha256) { throw 'The installed ISCC.exe does not match installer\innosetup-pin.json' }
    }
}
if (!(Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) { throw "Inno Setup compiler missing: $InnoCompiler" }
# The runtime of the toolset that built the package: Launcher.exe refuses to
# start on an older one, so the installer carries exactly this one.
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
Enter-EmberVcEnvironment $tools.VisualStudioPath
$redist = Join-Path $env:VCToolsRedistDir 'vc_redist.x86.exe'
if (!(Test-Path -LiteralPath $redist -PathType Leaf)) { throw "Visual C++ runtime installer missing: $redist" }
$signature = Get-AuthenticodeSignature -LiteralPath $redist
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw "The Visual C++ runtime installer is not signed by Microsoft: $redist" }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$installer = Join-Path $OutDir "sf4-ember-netplay-$VersionLabel-setup.exe"
if (Test-Path -LiteralPath $installer) { throw "Installer destination already exists: $installer" }
& $InnoCompiler /Qp "/DPackageDir=$PackageDir" "/DAppVersion=$VersionLabel" "/DRedist=$redist" @InnoDefines "/O$OutDir" (Join-Path $PSScriptRoot 'installer\ember-setup.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$installer.sha256" -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($installer))"
$script:InstallerPath = $installer
Write-Host "SF4 Ember Netplay installer: $installer"
