# Supplemental quality gate. Windows build/CTest and network acceptance have
# their own receipts; this gate names its supported-platform checks explicitly.
param([string]$OutputDirectory = '', [string]$LinuxDistribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
if (!$OutputDirectory) { $OutputDirectory = Join-Path $build ('quality-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw "Quality output directory already exists: $OutputDirectory" }
$OutputDirectory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$receipt = [ordered]@{sourceRoot=$repo; branch=(& git -C $repo branch --show-current); commit=(& git -C $repo rev-parse HEAD);
    sourceFingerprint=(Get-SourceFingerprint $repo); startedUtc=[DateTime]::UtcNow.ToString('o'); components=@();
    linuxComponents=@();
    coverageNotes=@('DPAPI and Windows bridge-process fixtures run only on Windows. Linux validates its passphrase backend and DPAPI refusal.',
        'Explicitly ignored Rust tests remain unexecuted; their counts and names are retained in each Cargo log.');
    exclusions=@('Game launch and gameplay','Live Discord and external provider acceptance','Package installation and deployment')}
# One selector for every cargo call here and in the Linux script, which owns the Linux component list.
$rustToolchain = '+1.98.0'
function Save-QualityReceipt {
    $copy = [ordered]@{}
    foreach ($key in $receipt.Keys) { $copy[$key] = $receipt[$key] }
    $copy.components = @($receipt.components | Select-Object * -ExcludeProperty run)
    $copy | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'quality.json')
}
function Add-Check([string]$name, [string]$platform, [scriptblock]$run) {
    $receipt.components += [pscustomobject]@{name=$name;platform=$platform;status='unexecuted';exitCode=$null;seconds=0;log=(Join-Path $OutputDirectory ($name+'.log'));error='';run=$run}
}
foreach ($crate in (Get-Content -LiteralPath (Join-Path $repo 'scripts/rust-workspaces.json') -Raw | ConvertFrom-Json)) {
    $name,$relative = $crate.name,$crate.path
    $targetName = switch ($name) {
        'helper' { 'rust-test-target' }
        'ember' { 'rust-ember-target' }
        default { "quality-$name-target" }
    }
    foreach ($operation in @('tests','clippy','format')) {
        $work = Join-Path $repo $relative; $cargoTarget = Join-Path $build $targetName; $kind = $operation
        Add-Check "$name-windows-$operation" 'windows' {
            $previousTarget=$env:CARGO_TARGET_DIR
            $previousFlags=$env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS
            $env:CARGO_TARGET_DIR = $cargoTarget
            $extra=@()
            if ($name -eq 'helper') {
                $extra=@('--target','x86_64-pc-windows-msvc')
                $env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS='-C target-feature=+crt-static'
            }
            Push-Location $work
            try {
                switch ($kind) {
                    'tests' { & cargo $rustToolchain test --locked -j 4 @extra }
                    'clippy' { & cargo $rustToolchain clippy --locked -j 4 @extra --all-targets --no-deps -- -D warnings }
                    'format' { & cargo $rustToolchain fmt --all -- --check }
                }
                if ($LASTEXITCODE) { throw "cargo $kind failed ($LASTEXITCODE)" }
            } finally {
                Pop-Location
                $env:CARGO_TARGET_DIR=$previousTarget
                $env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=$previousFlags
            }
        }.GetNewClosure()
    }
}
Add-Check 'powershell-syntax' 'windows' {
    foreach ($path in (& git -C $repo ls-files --cached --others --exclude-standard | Select-Object -Unique | Where-Object { $_ -like '*.ps1' })) {
        $tokens=$null; $errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo $path),[ref]$tokens,[ref]$errors)
        if ($errors.Count) { throw "$path : $errors" }
    }
    Write-Output 'All inventoried PowerShell scripts parsed.'
}
Add-Check 'python-syntax' 'windows' {
    & python -c 'import ast,pathlib,subprocess; files=subprocess.check_output(["git","ls-files","--cached","--others","--exclude-standard"],text=True).splitlines(); [ast.parse(pathlib.Path(p).read_text(encoding="utf-8-sig"),filename=p) for p in set(files) if p.endswith(".py")]; print("All inventoried Python files parsed.")'
    if ($LASTEXITCODE) { throw 'Python syntax failed' }
}
Add-Check 'linux-suite' 'linux' {
    $linuxSource = (& wsl -d $LinuxDistribution --exec wslpath -a $repo).Trim()
    if ($LASTEXITCODE) { throw 'Linux source path unavailable' }
    $linuxOutput = (& wsl -d $LinuxDistribution --exec wslpath -a $OutputDirectory).Trim()
    if ($LASTEXITCODE) { throw 'Linux output path unavailable' }
    & wsl -d $LinuxDistribution --exec python3 "$linuxSource/scripts/test-full-quality-linux.py" $linuxSource "$linuxOutput/linux" $rustToolchain
    if ($LASTEXITCODE) { throw 'Linux quality suite failed; see per-component receipt' }
}
Save-QualityReceipt
$tools = Get-EmberToolPaths $repo '' ''
Enter-EmberVcEnvironment $tools.VisualStudioPath
Push-Location $repo
try {
    foreach ($check in $receipt.components) {
        Write-Output "RUN $($check.name); log: $($check.log)"
        $started=[DateTime]::UtcNow
        try { & $check.run *> $check.log; $check.status='passed'; $check.exitCode=0 }
        catch { $check.status='failed'; $check.exitCode=1; $check.error=$_.Exception.Message }
        $check.seconds=([DateTime]::UtcNow-$started).TotalSeconds
        Save-QualityReceipt
        Write-Output "$($check.status.ToUpperInvariant()) $($check.name)"
    }
} finally { Pop-Location }
$linuxReceipt=Join-Path $OutputDirectory 'linux/linux-quality.json'
$receipt.linuxComponents = if (Test-Path -LiteralPath $linuxReceipt) { @(Get-Content -Raw -LiteralPath $linuxReceipt | ConvertFrom-Json) }
    else { @([pscustomobject]@{name='linux-receipt';platform='linux';status='missing'}) }
$receipt['sourceUnchanged'] = (Get-SourceFingerprint $repo) -eq $receipt.sourceFingerprint
$receipt['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
foreach ($check in $receipt.components) { $check.PSObject.Properties.Remove('run') }
Save-QualityReceipt
if (!$receipt.sourceUnchanged -or @(($receipt.components + $receipt.linuxComponents) | Where-Object { $_.status -ne 'passed' }).Count) { throw "Quality gate failed; see $OutputDirectory" }
Write-Output "Full quality gate passed; receipt: $OutputDirectory\quality.json"
