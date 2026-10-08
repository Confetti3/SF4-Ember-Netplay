param([string]$NativeFixture='')
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -ErrorAction Stop
$source=Join-Path $PSScriptRoot 'upgrade\Install-Upgrade.ps1'
$root=Join-Path ([IO.Path]::GetTempPath()) ('ember-upgrade-recovery-test-'+[Guid]::NewGuid().ToString('N'))
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function ManifestLine([string]$Path,[string]$Relative){'{0}  {1}' -f (Hash $Path).ToLowerInvariant(),$Relative}
function VerifyRestored([string]$Install,$Transaction) {
 foreach($operation in @($Transaction.operations)) {
  $path=Join-Path $Install $operation.path
  if($operation.existed) {
   if(!(Test-Path -LiteralPath $path -PathType Leaf)-or(Hash $path)-ine$operation.priorSha256){throw "Prior bytes were not restored: $($operation.path)"}
  } elseif(Test-Path -LiteralPath $path){throw "New operation file was not removed: $($operation.path)"}
 }
}
function NativeRecover([string]$Install) {
 $info=New-Object Diagnostics.ProcessStartInfo
 $info.FileName=$NativeFixture;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
 $info.Arguments="--recover `"$Install`""
 $child=[Diagnostics.Process]::Start($info)
 try {
  if(!$child.WaitForExit(30000)){throw 'Native recovery timed out'}
  return [pscustomobject]@{ExitCode=$child.ExitCode;Output=$child.StandardOutput.ReadToEnd()+$child.StandardError.ReadToEnd()}
 } finally {$child.Dispose()}
}
try {
 $package=Join-Path $root 'package';$payload=Join-Path $package 'payload';$install=Join-Path $root 'install'
 $null=New-Item -ItemType Directory -Path $payload,$install -Force
 Copy-Item -LiteralPath $source -Destination (Join-Path $package 'Install-Upgrade.ps1')
 Set-Content -LiteralPath (Join-Path $install 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'old-launcher'
 Set-Content -LiteralPath (Join-Path $install 'Updater.exe') -Encoding ASCII -NoNewline -Value 'old-updater'
 Set-Content -LiteralPath (Join-Path $install 'preflight.ps1') -Encoding ASCII -Value 'param([string]$PackageDir); exit 0'
 Set-Content -LiteralPath (Join-Path $install 'future.bin') -Encoding ASCII -NoNewline -Value 'user-collision'
 Set-Content -LiteralPath (Join-Path $install 'Launcher.exe.ember-new') -Encoding ASCII -NoNewline -Value 'user-temporary-name'
 $beforeLines=@(ManifestLine (Join-Path $install 'Launcher.exe') 'Launcher.exe';ManifestLine (Join-Path $install 'Updater.exe') 'Updater.exe';ManifestLine (Join-Path $install 'preflight.ps1') 'preflight.ps1')
 $baseManifest=Join-Path $package 'base-manifest.txt';Set-Content -LiteralPath $baseManifest -Encoding UTF8 -Value $beforeLines
 Copy-Item -LiteralPath $baseManifest -Destination (Join-Path $install 'MANIFEST.txt')
 Set-Content -LiteralPath (Join-Path $payload 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'new-launcher'
 Set-Content -LiteralPath (Join-Path $payload 'Updater.exe') -Encoding ASCII -NoNewline -Value 'new-updater'
 Set-Content -LiteralPath (Join-Path $payload 'future.bin') -Encoding ASCII -NoNewline -Value 'product-file'
 $targetManifest=Join-Path $payload 'MANIFEST.txt'
 $afterLines=@(ManifestLine (Join-Path $payload 'Launcher.exe') 'Launcher.exe';ManifestLine (Join-Path $payload 'Updater.exe') 'Updater.exe';ManifestLine (Join-Path $install 'preflight.ps1') 'preflight.ps1';ManifestLine (Join-Path $payload 'future.bin') 'future.bin')
 Set-Content -LiteralPath $targetManifest -Encoding UTF8 -Value $afterLines
 $changed=@('Launcher.exe','Updater.exe','future.bin','MANIFEST.txt')|Sort-Object
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
 foreach($boundary in 1..4) {
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
  # A rollback interrupted after restoring Launcher resumes its remaining operations.
  $rollingBack=$original|ConvertFrom-Json
  $rollingBack.state='rolling-back'
  Copy-Item -LiteralPath (Join-Path $rollingBack.backup 'Launcher.exe') -Destination (Join-Path $install 'Launcher.exe') -Force
  $rollingBack|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
 }
 if($boundary-eq 4) {
  # Released updaters recorded unchanged files too, with no missingSkipped marker.
  $legacy=$original|ConvertFrom-Json
  $unchanged=Join-Path $install 'preflight.ps1';$unchangedHash=Hash $unchanged
  Copy-Item -LiteralPath $unchanged -Destination (Join-Path $legacy.backup 'preflight.ps1')
  $legacy.operations+=@([pscustomobject]@{path='preflight.ps1';existed=$true;priorSha256=$unchangedHash;targetSha256=$unchangedHash})
  $legacy|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
 }
 if($NativeFixture -and $boundary-eq 3) {
  # Start from a real prepared PowerShell journal. Restore Launcher, then fail
  # on Updater while a skipped file has been edited since preparation.
  $prepared=$original|ConvertFrom-Json
  if($prepared.state-ne'prepared'){throw 'Rollback fixture did not start prepared'}
  $skipped=Join-Path $install 'preflight.ps1';$skippedOriginal=Get-Content -LiteralPath $skipped -Raw
  Set-Content -LiteralPath $skipped -Encoding ASCII -NoNewline -Value 'edited-skipped-file'
  $held=[IO.File]::Open((Join-Path $install 'Updater.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try {
   $failed=$false;try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly}catch{$failed=$true}
   if(!$failed){throw 'Locked Updater did not block PowerShell restoration'}
   if((Get-Content -Raw (Join-Path $install 'Launcher.exe'))-ne'old-launcher'-or(Get-Content -Raw (Join-Path $install 'Updater.exe'))-ne'new-updater'){throw 'Fixture did not reach a partial PowerShell restoration'}
   $rollback=Get-Content -LiteralPath $transactionPath -Raw -Encoding UTF8|ConvertFrom-Json
   if($rollback.state-ne'rolling-back'){throw 'PowerShell restoration did not persist rollback intent'}
   if(Test-Path -LiteralPath ($transactionPath+'.failed')){throw 'Unfinished rollback was set aside'}
   $retry=NativeRecover $install
   if($retry.ExitCode-eq 0-or!(Test-Path -LiteralPath $transactionPath)){throw 'Native retry cleared an unfinished rollback'}
  } finally {$held.Dispose()}
  $retry=NativeRecover $install
  if($retry.ExitCode-ne 0){throw "Native rollback retry failed: $($retry.Output)"}
  VerifyRestored $install $prepared
  if((Get-Content -LiteralPath $skipped -Raw)-ne'edited-skipped-file'){throw 'Rollback retry overwrote the skipped edit'}
  if(Test-Path -LiteralPath ($transactionPath+'.failed')){throw 'Successful rollback was set aside'}
  Set-Content -LiteralPath $skipped -Encoding ASCII -NoNewline -Value $skippedOriginal
  Write-Host 'PowerShell partial restoration -> native rollback retry passed.'
 } elseif($NativeFixture -and $boundary-eq 1) {
  & $NativeFixture --recover $install
  if($LASTEXITCODE){throw 'Native recovery rejected PowerShell transaction'}
 } else { & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly }
 if((Get-Content -Raw (Join-Path $install 'Launcher.exe'))-ne'old-launcher'){throw 'Launcher was not restored'}
 if((Get-Content -Raw (Join-Path $install 'Updater.exe'))-ne'old-updater'){throw 'Updater was not restored'}
 if((Get-Content -Raw (Join-Path $install 'future.bin'))-ne'user-collision'){throw 'Colliding user file was not restored'}
 if(Test-Path -LiteralPath (Join-Path $install '.ember-update-transaction-v1.json')){throw 'Recovered transaction was not cleared'}
 if((Get-Content -Raw (Join-Path $install 'Launcher.exe.ember-new'))-ne'user-temporary-name'){throw 'Temporary-name collision was overwritten'}
 if($boundary-eq 4) {
  # Legacy committed journals still verify the target and clear without rollback.
  foreach($relative in $changed){Copy-Item -LiteralPath (Join-Path $payload $relative) -Destination (Join-Path $install $relative) -Force}
  $legacy.state='committed'
  $legacy|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
  & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly
  foreach($property in $legacy.target.PSObject.Properties) {
   if((Hash (Join-Path $install $property.Name))-ine$property.Value){throw 'Legacy committed recovery restored prior bytes'}
  }
  if(Test-Path -LiteralPath $transactionPath){throw 'Legacy committed transaction was not cleared'}
  Write-Host 'Legacy prepared/full-inventory and committed journals without missingSkipped passed.'
 }
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
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\src\common\PackageInventory.inc') -Destination (Join-Path $nativeStage 'PackageInventory.inc') -Force
  $nativeManifest=@(); foreach($file in Get-ChildItem -LiteralPath $nativeStage -File -Recurse){ if($file.Name -ne 'MANIFEST.txt'){ $nativeManifest+=ManifestLine $file.FullName $file.FullName.Substring($nativeStage.Length+1) } }
  Set-Content -LiteralPath (Join-Path $nativeStage 'MANIFEST.txt') -Encoding ASCII -Value $nativeManifest
  Set-Content -LiteralPath (Join-Path $nativeInstall 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'native-prior'
  $nativeStart=New-Object Diagnostics.ProcessStartInfo
  $nativeStart.FileName=$NativeFixture;$nativeStart.UseShellExecute=$false;$nativeStart.CreateNoWindow=$true
  $nativeStart.Arguments="--crash-child `"$nativeRoot`"";$nativeStart.EnvironmentVariables['SF4E_UPDATE_TEST_TERMINATE_AFTER']='2'
  $child=[Diagnostics.Process]::Start($nativeStart);if(!$child.WaitForExit(30000)-or$child.ExitCode-ne 86){throw 'Native crash fixture did not interrupt'}
  & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $nativeInstall -RecoverOnly
  if((Get-Content -Raw (Join-Path $nativeInstall 'Launcher.exe'))-ne'native-prior'){throw 'PowerShell recovery rejected native transaction'}
  # A sparse native transaction has no backup for unchanged sf4-net.exe.
  # Native recovery records that loss before a blocked restoration; PowerShell
  # must carry the terminal repair outcome through the successful retry.
  $sparseRoot=Join-Path $root 'native-missing';$sparseStage=Join-Path $sparseRoot 'staging';$sparseInstall=Join-Path $sparseRoot 'install'
  $null=New-Item -ItemType Directory -Path $sparseStage,$sparseInstall -Force
  foreach($entry in Get-ChildItem -LiteralPath $nativeStage) {
   Copy-Item -LiteralPath $entry.FullName -Destination $sparseStage -Recurse
   Copy-Item -LiteralPath $entry.FullName -Destination $sparseInstall -Recurse
  }
  Set-Content -LiteralPath (Join-Path $sparseInstall 'Launcher.exe') -Encoding ASCII -NoNewline -Value 'sparse-prior-launcher'
  Set-Content -LiteralPath (Join-Path $sparseInstall 'Updater.exe') -Encoding ASCII -NoNewline -Value 'sparse-prior-updater'
  $priorManifest=@();foreach($file in Get-ChildItem -LiteralPath $sparseInstall -File -Recurse){if($file.Name-ne'MANIFEST.txt'){$priorManifest+=ManifestLine $file.FullName $file.FullName.Substring($sparseInstall.Length+1)}}
  Set-Content -LiteralPath (Join-Path $sparseInstall 'MANIFEST.txt') -Encoding ASCII -Value $priorManifest
  $nativeStart.Arguments="--crash-child `"$sparseRoot`"";$nativeStart.EnvironmentVariables['SF4E_UPDATE_TEST_TERMINATE_AFTER']='1'
  $child=[Diagnostics.Process]::Start($nativeStart);if(!$child.WaitForExit(30000)-or$child.ExitCode-ne 86){throw 'Sparse native crash fixture did not interrupt'}
  $sparsePath=Join-Path $sparseInstall '.ember-update-transaction-v1.json'
  $sparsePrepared=Get-Content -LiteralPath $sparsePath -Raw -Encoding UTF8|ConvertFrom-Json
  if(@($sparsePrepared.operations).Count-ne 3){throw 'Native fixture did not record sparse operations'}
  Remove-Item -LiteralPath (Join-Path $sparseInstall 'sf4-net.exe')
  $held=[IO.File]::Open((Join-Path $sparseInstall 'Launcher.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try {
   $retry=NativeRecover $sparseInstall
   if($retry.ExitCode-eq 0){throw 'Locked native restoration unexpectedly succeeded'}
   $missingJournal=Get-Content -LiteralPath $sparsePath -Raw -Encoding UTF8
   $missing=$missingJournal|ConvertFrom-Json
   if($missing.state-ne'rolling-back'-or$missing.missingSkipped-ne'sf4-net.exe'){throw 'Native recovery did not persist the missing skipped file'}
  } finally {$held.Dispose()}
  # Invalid markers must fail before any destination or journal mutation.
  foreach($invalidMarker in @('outside-target.bin','Launcher.exe',17)) {
   $invalid=$missingJournal|ConvertFrom-Json;$invalid.missingSkipped=$invalidMarker
   $invalid|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $sparsePath -Encoding UTF8
   $invalidJournal=Get-Content -LiteralPath $sparsePath -Raw -Encoding UTF8
   $launcherBefore=Hash (Join-Path $sparseInstall 'Launcher.exe')
   $failure='';try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $sparseInstall -RecoverOnly}catch{$failure=$_.Exception.Message}
   if($failure-notlike'*Invalid missing update file*'-or(Hash (Join-Path $sparseInstall 'Launcher.exe'))-ne$launcherBefore-or(Get-Content -LiteralPath $sparsePath -Raw -Encoding UTF8)-cne$invalidJournal){throw 'Invalid missingSkipped evidence was not rejected before restoration'}
  }
  Set-Content -LiteralPath $sparsePath -Encoding UTF8 -Value $missingJournal
  $failure='';try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $sparseInstall -RecoverOnly}catch{$failure=$_.Exception.Message}
  if($failure-ne'An update file is missing and has no backup; install the update again: sf4-net.exe'){throw "PowerShell lost the terminal reinstall outcome: $failure"}
  VerifyRestored $sparseInstall $missing
  if((Test-Path -LiteralPath $sparsePath)-or!(Test-Path -LiteralPath ($sparsePath+'.failed'))){throw 'Completed repair failure did not retain a failed journal'}
  $retained=Get-Content -LiteralPath ($sparsePath+'.failed') -Raw -Encoding UTF8|ConvertFrom-Json
  if($retained.state-ne'rolling-back'-or$retained.missingSkipped-ne'sf4-net.exe'){throw 'Failed journal lost native recovery evidence'}
  if(Test-Path -LiteralPath (Join-Path $sparseInstall 'sf4-net.exe')){throw 'Missing skipped file was unexpectedly recreated'}
  & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $sparseInstall -RecoverOnly
  Write-Host 'Native missingSkipped -> PowerShell restoration and reinstall outcome passed.'
 }
 Write-Host 'PowerShell collision and interrupted-upgrade recovery checks passed.'
} finally {
 if((Split-Path $root -Leaf)-like'ember-upgrade-recovery-test-*' -and (Test-Path -LiteralPath $root)){Remove-Item -LiteralPath $root -Recurse -Force}
}
