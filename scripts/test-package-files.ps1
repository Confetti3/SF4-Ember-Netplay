#Requires -Version 7
# Pins the package hash formats and the packaging functions' contract. Everything runs under TEMP; nothing
# builds, publishes or touches a checkout.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PackageFiles.ps1')
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('ember-package-files-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
function Check([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Pattern, [string]$What) {
    $failure = $null
    try { & $Action | Out-Null } catch { $failure = $_.Exception.Message }
    Check ($null -ne $failure) "$What was accepted"
    Check ($failure -match $Pattern) "$What failed with '$failure', expected '$Pattern'"
}
function Pass([string]$Name) { Write-Host "PASS $Name" }
function New-Tree([string]$Path, [hashtable]$Files) {
    foreach ($name in $Files.Keys) {
        $file = Join-Path $Path $name
        $null = New-Item -ItemType Directory -Path (Split-Path $file -Parent) -Force
        [IO.File]::WriteAllText($file, $Files[$name])
    }
}
try {
    # The writers produce the bytes the scripts wrote before (pwsh 7 Set-Content -Encoding UTF8 / ASCII).
    $tree = Join-Path $root 'tree'
    New-Tree $tree @{ 'b.txt' = 'bee'; 'Beta.txt' = 'upper'; 'a/z.bin' = 'zed'; 'a-b.txt' = 'dash'; '_x' = 'under'; 'Z/y.txt' = 'why'; 'assets/selection/one.png' = 'png' }
    $paths = [string[]]@(Get-ChildItem -LiteralPath $tree -File -Recurse | ForEach-Object { $_.FullName })
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $legacy = foreach ($path in $paths) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant(), $path.Substring($tree.Length + 1)
    }
    Set-Content -LiteralPath (Join-Path $root 'legacy-manifest.txt') -Encoding UTF8 -Value $legacy
    Write-HashManifest $tree 'MANIFEST.txt'
    $written = [IO.File]::ReadAllBytes((Join-Path $tree 'MANIFEST.txt'))
    Check ([Linq.Enumerable]::SequenceEqual($written, [IO.File]::ReadAllBytes((Join-Path $root 'legacy-manifest.txt')))) 'Write-HashManifest differs from the earlier inline manifest bytes'
    Check (!($written[0] -eq 0xEF -and $written[1] -eq 0xBB)) 'the manifest carries a byte-order mark'
    Check (@(Get-Content -LiteralPath (Join-Path $tree 'MANIFEST.txt')).Count -eq 7) 'the manifest lists itself or misses a file'
    Write-HashManifest "$tree\" 'SECOND.txt'
    Check (@(Get-Content -LiteralPath (Join-Path $tree 'SECOND.txt')) -notcontains $null) 'a trailing separator broke the relative paths'
    Check ((Get-Content -LiteralPath (Join-Path $tree 'SECOND.txt'))[0] -match '^[0-9a-f]{64}  \S') 'a trailing separator cut the first relative path'
    Pass 'Write-HashManifest writes the earlier bytes: ordinal order, lowercase hex, no byte-order mark, never itself'

    $zip = Join-Path $root 'sample.zip'
    [IO.File]::WriteAllText($zip, 'zip bytes')
    $legacyHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $root 'legacy.zip.sha256') -Encoding ASCII -Value "$legacyHash  $([IO.Path]::GetFileName($zip))"
    $returned = Write-Sha256Sidecar $zip
    Check ($returned -ceq $legacyHash) 'Write-Sha256Sidecar returned a different hash'
    Check ([Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes("$zip.sha256"), [IO.File]::ReadAllBytes((Join-Path $root 'legacy.zip.sha256')))) 'the sidecar bytes differ from the earlier inline sidecar'
    Check ((Assert-Sha256Sidecar $zip) -ceq $legacyHash) 'Assert-Sha256Sidecar rejected its own sidecar'
    Pass 'Write-Sha256Sidecar writes the earlier ASCII sidecar and Assert-Sha256Sidecar accepts it'

    Reject { Assert-Sha256Sidecar (Join-Path $root 'absent.zip') } 'sidecar missing' 'a missing package'
    $other = Join-Path $root 'other.zip'
    [IO.File]::WriteAllText($other, 'x')
    Reject { Assert-Sha256Sidecar $other } 'sidecar missing' 'a missing sidecar'
    [IO.File]::WriteAllText("$other.sha256", "$legacyHash  OTHER.zip")
    Reject { Assert-Sha256Sidecar $other } 'Malformed package checksum sidecar' 'a sidecar naming the file in another case'
    [IO.File]::WriteAllText("$other.sha256", "$legacyHash  other.zip")
    Reject { Assert-Sha256Sidecar $other } 'Package checksum mismatch' 'a sidecar with another file''s hash'
    [IO.File]::WriteAllText("$other.sha256", "$legacyHash other.zip")
    Reject { Assert-Sha256Sidecar $other } 'Malformed package checksum sidecar' 'a sidecar with one space'
    $exe = Join-Path $root 'setup.exe'
    [IO.File]::WriteAllText($exe, 'x')
    $null = Write-Sha256Sidecar $exe
    Reject { Assert-Sha256Sidecar $exe } 'Malformed package checksum sidecar' 'a .exe checked as a zip'
    Check ((Assert-Sha256Sidecar $exe '.exe') -match '^[0-9a-f]{64}$') 'an .exe sidecar was refused for .exe'
    Pass 'Assert-Sha256Sidecar refuses a missing, malformed, renamed or mismatched sidecar'

    $manifest = Join-Path $root 'read.txt'
    $a = 'a' * 64; $b = 'B' * 64
    [IO.File]::WriteAllLines($manifest, @("$a  one.txt", "$b  dir\two.txt"))
    $entries = Read-HashManifest $manifest
    Check ($entries.Count -eq 2 -and $entries['one.txt'] -ceq $a -and $entries['dir\two.txt'] -ceq $b) 'Read-HashManifest returned the wrong table'
    [IO.File]::WriteAllLines($manifest, @("$a  one.txt", "$b  one.txt"))
    Reject { Read-HashManifest $manifest } 'Malformed manifest' 'a duplicate manifest path'
    [IO.File]::WriteAllLines($manifest, @("$a  one.txt", 'not a hash line'))
    Reject { Read-HashManifest $manifest } 'Malformed manifest' 'a malformed manifest line'
    Pass 'Read-HashManifest refuses duplicates and malformed lines'

    # The readers that ship inside packages keep their own copies; they must accept what is written here,
    # with and without a byte-order mark.
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'upgrade\Install-Upgrade.ps1'), [ref]$null, [ref]$null)
    $shipped = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in 'ReadManifest', 'SafePath', 'ValidRelative' }, $true)
    Check ($shipped.Count -ge 1) 'Install-Upgrade.ps1 no longer has ReadManifest'
    $shippedReader = Join-Path $root 'shipped-reader.ps1'
    [IO.File]::WriteAllText($shippedReader, (($shipped | ForEach-Object { $_.Extent.Text }) -join "`n"))
    . $shippedReader
    $inventory = "SF4E_PACKAGE_REQUIRED(`"PackageInventory.inc`")`nSF4E_PACKAGE_REQUIRED(`"app.txt`")`nSF4E_PACKAGE_REQUIRED(`"build-provenance.json`")`nSF4E_PACKAGE_REQUIRED(`"notes.txt`")`nSF4E_PACKAGE_REQUIRED(`"MANIFEST.txt`")`nSF4E_PACKAGE_REQUIRED(`"preflight.ps1`")`n"
    function New-MiniPackage([string]$Path, [string]$App) {
        $files = @{ 'PackageInventory.inc' = $inventory; 'app.txt' = $App; 'build-provenance.json' = '{"baseRevision":"abc"}'; 'notes.txt' = 'n'; 'preflight.ps1' = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'tester-preflight.ps1')) }
        for ($index = 0; $index -lt 44; $index++) { $files["assets\selection\f$index.png"] = "png$index" }
        New-Tree $Path $files
        Write-HashManifest $Path 'MANIFEST.txt'
    }
    foreach ($bom in $false, $true) {
        $package = Join-Path $root "mini-$bom"
        New-MiniPackage $package 'one'
        $manifestPath = Join-Path $package 'MANIFEST.txt'
        if ($bom) { [IO.File]::WriteAllBytes($manifestPath, [byte[]](0xEF, 0xBB, 0xBF) + [IO.File]::ReadAllBytes($manifestPath)) }
        $mine = Read-HashManifest $manifestPath
        $theirs = ReadManifest $manifestPath $package
        Check ($mine.Count -eq $theirs.Count -and $mine.Count -eq 49) "manifest entries differ (bom $bom)"
        foreach ($key in $mine.Keys) { Check ($theirs[$key] -ceq $mine[$key]) "Install-Upgrade.ps1 read another hash for $key (bom $bom)" }
        $output = & (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $package -Strict 6>&1 | Out-String
        Check ($output -match 'preflight passed') "tester-preflight.ps1 rejected the written manifest (bom $bom): $output"
    }
    Pass 'the shipped manifest readers accept the written manifest, with and without a byte-order mark'

    # The entry scripts hand their bound parameters to the functions, so the names must stay equal.
    foreach ($pair in @(@('package-team.ps1', 'New-EmberPackage'), @('package-upgrade.ps1', 'New-EmberUpgradePackage'), @('package-installer.ps1', 'New-EmberInstaller'))) {
        $entry = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $pair[0]), [ref]$null, [ref]$null)
        $scriptParameters = @($entry.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) | Sort-Object
        $functionParameters = @((Get-Command $pair[1]).Parameters.Keys | Where-Object { $_ -notin [Management.Automation.PSCmdlet]::CommonParameters }) | Sort-Object
        Check (($scriptParameters -join ',') -ceq ($functionParameters -join ',')) "$($pair[0]) and $($pair[1]) take different parameters: $($scriptParameters -join ',') / $($functionParameters -join ',')"
        Check ((Get-Command $pair[1]).ScriptBlock.File -ceq (Join-Path $PSScriptRoot 'Packaging.ps1')) "$($pair[1]) is not defined in Packaging.ps1"
    }
    Pass 'each entry script takes the parameters of the function it calls'

    # Strictness stays inside the functions: the upgrade function runs strict, and the caller does not.
    Set-StrictMode -Off
    Reject { New-EmberUpgradePackage -FromVersion 'x' -ToVersion '1.0.1' -BaseZip 'a' -TargetZip 'b' } 'distinct semantic versions' 'a bad upgrade version'
    $undefined = $notDefinedAnywhere
    Check ($null -eq $undefined) 'the packaging functions left strict mode on in the caller'
    Set-StrictMode -Version Latest
    Pass 'the upgrade function does not leak strict mode into its caller'

    # A whole upgrade over two small complete packages: one result object, the earlier files and sidecars.
    $packages = Join-Path $root 'packages'
    foreach ($version in @(@('1.0.0', 'one'), @('1.0.1', 'two'))) {
        $folder = Join-Path $packages "sf4-ember-netplay-$($version[0])"
        New-MiniPackage $folder $version[1]
        Compress-Archive -LiteralPath $folder -DestinationPath "$folder.zip"
        $null = Write-Sha256Sidecar "$folder.zip"
    }
    $upgradeOut = Join-Path $root 'upgrade-out'
    $result = @(New-EmberUpgradePackage -FromVersion '1.0.0' -ToVersion '1.0.1' -BaseZip (Join-Path $packages 'sf4-ember-netplay-1.0.0.zip') `
        -TargetZip (Join-Path $packages 'sf4-ember-netplay-1.0.1.zip') -OutDir $upgradeOut 6>$null)
    Check ($result.Count -eq 1) "the upgrade function returned $($result.Count) values; console output must stay out of the result"
    Check ($result[0].zipPath -ceq (Join-Path $upgradeOut 'upgrade-sf4-ember-netplay-1.0.0-to-1.0.1.zip')) "unexpected upgrade zip path: $($result[0].zipPath)"
    Check ((Test-Path -LiteralPath $result[0].folderPath -PathType Container) -and (Test-Path -LiteralPath "$($result[0].zipPath).sha256" -PathType Leaf)) 'the upgrade folder or its sidecar is missing'
    $null = Assert-Sha256Sidecar $result[0].zipPath
    $inventoryManifest = Read-HashManifest (Join-Path $result[0].folderPath 'UPGRADE_MANIFEST.txt')
    Check ($inventoryManifest.ContainsKey('upgrade.json') -and $inventoryManifest.ContainsKey('payload\app.txt') -and !$inventoryManifest.ContainsKey('UPGRADE_MANIFEST.txt')) 'UPGRADE_MANIFEST.txt lists the wrong files'
    Pass 'New-EmberUpgradePackage over two small packages returns one object and writes manifest and sidecar'
    Write-Host 'Package file and function tests passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    if ((Split-Path $resolved -Leaf) -like 'ember-package-files-test-*' -and $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
