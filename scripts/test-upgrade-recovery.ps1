param([string]$NativeFixture='')
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -ErrorAction Stop
$source=Join-Path $PSScriptRoot 'upgrade\Install-Upgrade.ps1'
$root=Join-Path ([IO.Path]::GetTempPath()) ('ember-upgrade-recovery-test-'+[Guid]::NewGuid().ToString('N'))
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function ManifestLine([string]$Path,[string]$Relative){'{0}  {1}' -f (Hash $Path).ToLowerInvariant(),$Relative}
try {
 $package=Join-Path $root 'package';$payload=Join-Path $package 'payload';$install=Join-Path $root 'install'
 $null=New-Item -ItemType Directory -Path $payload,$install -Force
 Copy-Item -LiteralPath $source -Destination (Join-Path $package 'Install-Upgrade.ps1')
 Set-Content -LiteralPath (Join-Path $install 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'old-launcher'
 Set-Content -LiteralPath (Join-Path $install 'preflight.ps1') -Encoding ASCII -Value 'param([string]$PackageDir); exit 0'
 Set-Content -LiteralPath (Join-Path $install 'future.bin') -Encoding ASCII -NoNewline -Value 'user-collision'
 Set-Content -LiteralPath (Join-Path $install 'Launcher.exe.ember-new') -Encoding ASCII -NoNewline -Value 'user-temporary-name'
 $beforeLines=@(ManifestLine (Join-Path $install 'Launcher.exe') 'Launcher.exe';ManifestLine (Join-Path $install 'preflight.ps1') 'preflight.ps1')
 $baseManifest=Join-Path $package 'base-manifest.txt';Set-Content -LiteralPath $baseManifest -Encoding UTF8 -Value $beforeLines
 Copy-Item -LiteralPath $baseManifest -Destination (Join-Path $install 'MANIFEST.txt')
 Set-Content -LiteralPath (Join-Path $payload 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'new-launcher'
 Set-Content -LiteralPath (Join-Path $payload 'future.bin') -Encoding ASCII -NoNewline -Value 'product-file'
 $targetManifest=Join-Path $payload 'MANIFEST.txt'
 $afterLines=@(ManifestLine (Join-Path $payload 'Launcher.exe') 'Launcher.exe';ManifestLine (Join-Path $install 'preflight.ps1') 'preflight.ps1';ManifestLine (Join-Path $payload 'future.bin') 'future.bin')
 Set-Content -LiteralPath $targetManifest -Encoding UTF8 -Value $afterLines
 $changed=@('Launcher.exe','future.bin','MANIFEST.txt')|Sort-Object
 $metadata=[ordered]@{schema=1;from='1.0.0';to='1.0.1';baseManifestSha256=Hash $baseManifest;targetManifestSha256=Hash $targetManifest;
  changedFiles=$changed;removedFiles=@();installerSha256=Hash (Join-Path $package 'Install-Upgrade.ps1')}
 $upgradeJson=Join-Path $package 'upgrade.json';$metadata|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $upgradeJson -Encoding UTF8
 $inventory=@()
 foreach($file in Get-ChildItem -LiteralPath $package -File -Recurse|Sort-Object FullName){
  if($file.Name-ne'UPGRADE_MANIFEST.txt'){$inventory+=ManifestLine $file.FullName $file.FullName.Substring($package.Length+1)}
 }
 Set-Content -LiteralPath (Join-Path $package 'UPGRADE_MANIFEST.txt') -Encoding UTF8 -Value $inventory
 & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -CheckOnly
 if(Test-Path -LiteralPath (Join-Path $install '.ember-update.lock')){throw 'CheckOnly created a lock file'}
 $start=New-Object Diagnostics.ProcessStartInfo
 # Use the same PowerShell host as the fixture. Launching Windows PowerShell
 # from pwsh through ProcessStartInfo inherits pwsh's PSModulePath verbatim,
 # which can shadow the Windows PowerShell Utility module and hide Get-FileHash.
 $start.FileName=(Get-Process -Id $PID).Path;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 $start.Arguments="-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $package 'Install-Upgrade.ps1')`" -InstallDir `"$install`""
 $start.EnvironmentVariables['SF4E_UPDATE_TEST_TERMINATE_AFTER']='2'
 foreach($boundary in 1..3) {
 $start.EnvironmentVariables['SF4E_UPDATE_TEST_TERMINATE_AFTER']=[string]$boundary
 $process=[Diagnostics.Process]::Start($start);if(!$process.WaitForExit(30000)){throw 'Termination fixture timed out'}
 $childOutput=$process.StandardOutput.ReadToEnd()+$process.StandardError.ReadToEnd()
 if($process.ExitCode-eq 0){throw 'Termination fixture did not interrupt the installer'}
 if(!(Test-Path -LiteralPath (Join-Path $install '.ember-update-transaction-v1.json'))){throw "Interrupted installer left no transaction. Child output: $childOutput"}
 $transactionPath=Join-Path $install '.ember-update-transaction-v1.json'
 $original=Get-Content -LiteralPath $transactionPath -Raw -Encoding UTF8
 & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -CheckOnly
 if((Get-Content -LiteralPath $transactionPath -Raw -Encoding UTF8)-cne$original){throw 'CheckOnly modified pending journal'}
 if($boundary-eq 2) {
  $corrupt=$original|ConvertFrom-Json
  $corrupt.operations[-1].priorSha256=('0'*64)
  $corrupt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
  $launcherBefore=Hash (Join-Path $install 'Launcher.exe')
  $failed=$false;try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly}catch{$failed=$true}
  if(!$failed-or(Hash (Join-Path $install 'Launcher.exe'))-ne$launcherBefore){throw 'Damaged backup evidence changed live files'}
  Set-Content -LiteralPath $transactionPath -Encoding UTF8 -Value $original
 }
 if($NativeFixture -and $boundary-eq 1) {
  & $NativeFixture --recover $install
  if($LASTEXITCODE){throw 'Native recovery rejected PowerShell transaction'}
 } else { & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly }
 if((Get-Content -Raw (Join-Path $install 'Launcher.exe'))-ne'old-launcher'){throw 'Launcher was not restored'}
 if((Get-Content -Raw (Join-Path $install 'future.bin'))-ne'user-collision'){throw 'Colliding user file was not restored'}
 if(Test-Path -LiteralPath (Join-Path $install '.ember-update-transaction-v1.json')){throw 'Recovered transaction was not cleared'}
 if((Get-Content -Raw (Join-Path $install 'Launcher.exe.ember-new'))-ne'user-temporary-name'){throw 'Temporary-name collision was overwritten'}
 }
 if($NativeFixture) {
  $nativeRoot=Join-Path $root 'native';$nativeStage=Join-Path $nativeRoot 'staging';$nativeInstall=Join-Path $nativeRoot 'install'
  $null=New-Item -ItemType Directory -Path $nativeStage,$nativeInstall -Force
  foreach($line in Get-Content (Join-Path $PSScriptRoot '..\src\common\PackageInventory.inc')) {
   if($line-match'^SF4E_PACKAGE_REQUIRED\("(.*)"\)') {
    $file=Join-Path $nativeStage $Matches[1].Replace('\\','\');$null=New-Item -ItemType Directory -Path (Split-Path $file -Parent) -Force
    Set-Content -LiteralPath $file -Encoding ASCII -NoNewline -Value 'target'
   }
  }
  Set-Content -LiteralPath (Join-Path $nativeInstall 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'native-prior'
  $nativeStart=New-Object Diagnostics.ProcessStartInfo
  $nativeStart.FileName=$NativeFixture;$nativeStart.UseShellExecute=$false;$nativeStart.CreateNoWindow=$true
  $nativeStart.Arguments="--crash-child `"$nativeRoot`"";$nativeStart.EnvironmentVariables['SF4E_UPDATE_TEST_TERMINATE_AFTER']='2'
  $child=[Diagnostics.Process]::Start($nativeStart);if(!$child.WaitForExit(30000)-or$child.ExitCode-ne 86){throw 'Native crash fixture did not interrupt'}
  & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $nativeInstall -RecoverOnly
  if((Get-Content -Raw (Join-Path $nativeInstall 'Launcher.exe'))-ne'native-prior'){throw 'PowerShell recovery rejected native transaction'}
 }
 Write-Host 'PowerShell collision and interrupted-upgrade recovery checks passed.'
} finally {
 if((Split-Path $root -Leaf)-like'ember-upgrade-recovery-test-*' -and (Test-Path -LiteralPath $root)){Remove-Item -LiteralPath $root -Recurse -Force}
}
