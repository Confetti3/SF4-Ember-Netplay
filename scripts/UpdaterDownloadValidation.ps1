# Windows PowerShell started from pwsh (CTest under build-current.ps1) inherits
# pwsh's PSModulePath, which hides Get-FileHash; load this host's own module.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -ErrorAction Stop

# Removes a temp directory this script created: only a GUID-suffixed folder
# directly under the temp path, so a wrong path can never delete anything else.
function Remove-DownloadTempDirectory([string]$Path, [string]$Prefix) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (!$resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch "^$Prefix-[0-9a-f]{32}$") { throw 'Unsafe temporary cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
function Test-UpdaterDownload {
    param([string]$Label, [string]$Url, [string]$ExpectedSha256,
        [string]$Validator, [scriptblock]$Download)
    $root = Join-Path ([IO.Path]::GetTempPath()) ('ember-download-check-' + [guid]::NewGuid().ToString('N'))
    $result = [ordered]@{ method=$Label; passed=$false; bytes=0; sha256=''; error='' }
    try {
        if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Selected asset has no valid expected SHA-256' }
        if (!(Test-Path -LiteralPath $Validator -PathType Leaf)) { throw 'Native package validator is missing' }
        $null = New-Item -ItemType Directory -Path $root
        $archive = Join-Path $root 'download.zip'
        & $Download $Url $archive | Out-Null
        if (!(Test-Path -LiteralPath $archive -PathType Leaf)) { throw 'Download produced no file' }
        $result.bytes = (Get-Item -LiteralPath $archive).Length
        $result.sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($result.sha256 -ne $ExpectedSha256) { throw 'Selected asset SHA-256 mismatch' }
        # The shipped updater's own extractor (ZipExtract.cxx), then the
        # shipped package rules applied to what it wrote.
        $extract = Join-Path $root 'extracted'
        $null = New-Item -ItemType Directory -Path $extract
        & $Validator --extract $archive $extract | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed ($LASTEXITCODE)" }
        $entries = @(Get-ChildItem -LiteralPath $extract -Force)
        $package = $extract
        if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) { $package = $entries[0].FullName }
        & $Validator --validate $package | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Native package validation failed ($LASTEXITCODE)" }
        $result.passed = $true
    } catch { $result.error = $_.Exception.Message }
    finally { Remove-DownloadTempDirectory $root 'ember-download-check' }
    [pscustomobject]$result
}
