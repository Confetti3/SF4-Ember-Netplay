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
function Sha([string]$Text){[BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-','')}
function WriteText([string]$Path,[string]$Text){$null=New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force;[IO.File]::WriteAllText($Path,$Text)}
# A fixture's journal override, with each "@sha:<text>" string as the hash of
# <text>, and {install} and {name} in other strings as the case's folder and name.
function ResolveFixture($Value,[string]$Install,[string]$Name) {
 if($Value -is [string]){if($Value.StartsWith('@sha:')){return Sha $Value.Substring(5)};return $Value.Replace('{install}',$Install).Replace('{name}',$Name)}
 if($Value -is [array]){return ,[object[]]@($Value|ForEach-Object{ResolveFixture $_ $Install $Name})}
 if($Value -is [pscustomobject]){$resolved=[ordered]@{};foreach($p in $Value.PSObject.Properties){$resolved[$p.Name]=ResolveFixture $p.Value $Install $Name};return $resolved}
 return $Value
}
# Takes away, or gives back, the current user's right to read a file's
# attributes and to list its folder (which would grant that anyway), so that
# not even the file's status can be read.
function Readable([string]$Path,[bool]$Allow) {
 $user="$env:USERDOMAIN\$env:USERNAME"
 # Given back folder first: until it can be listed, icacls cannot find the file.
 foreach($change in $(if($Allow){@(@((Split-Path $Path -Parent),'/remove:d',$user),@($Path,'/remove:d',$user))}else{@(@($Path,'/deny',"${user}:(RA,R)"),@((Split-Path $Path -Parent),'/deny',"${user}:(RD)"))})){
  $null=icacls $change[0] $change[1] $change[2];if($LASTEXITCODE){throw "icacls $($change[1]) failed for $($change[0])"}
 }
}
# The journals of src/tests/data/upgrade-recovery/fixtures.json, each in its own
# folder under $Root, recovered by Install-Upgrade.ps1 -RecoverOnly and held to
# the outcomes PackageInstallerTest holds native recovery to.
function RecoveryFixtures([string]$Installer,[string]$Root) {
 $fixtures=Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\src\tests\data\upgrade-recovery\fixtures.json') -Raw|ConvertFrom-Json
 $wrong=@()
 foreach($fixture in $fixtures.cases) {
  $install=Join-Path $Root $fixture.name;$backup=Join-Path $install '.ember-update-backups\fixture'
  $journalPath=Join-Path $install '.ember-update-transaction-v1.json';$failedPath=$journalPath+'.failed'
  $null=New-Item -ItemType Directory -Path $backup -Force
  $paths=@{};$target=[ordered]@{};$operations=@()
  foreach($p in $fixtures.target.PSObject.Properties){$target[$p.Name]=Sha $p.Value;$paths[$p.Name]=$true}
  foreach($op in $fixtures.operations){
   $operations+=[ordered]@{path=$op.path;existed=$null-ne$op.prior;priorSha256=$(if($null-ne$op.prior){Sha $op.prior}else{''})}
   if($null-ne$op.prior){WriteText (Join-Path $backup $op.path) $op.prior};$paths[$op.path]=$true
  }
  if($fixture.PSObject.Properties['backup']){foreach($p in $fixture.backup.PSObject.Properties){
   $at=Join-Path $backup $p.Name;if($null-eq$p.Value){Remove-Item -LiteralPath $at -Force}else{WriteText $at $p.Value}
  }}
  foreach($p in $fixture.install.PSObject.Properties){WriteText (Join-Path $install $p.Name) $p.Value;$paths[$p.Name]=$true}
  foreach($p in $fixture.expect.files.PSObject.Properties){$paths[$p.Name]=$true}
  $journal=[ordered]@{schema=1;state=$fixture.state;installation=$install;backup=$backup;operations=$operations;target=$target}
  if($fixture.PSObject.Properties['missingSkipped']){$journal.missingSkipped=$fixture.missingSkipped}
  if($fixture.PSObject.Properties['journal']){foreach($p in $fixture.journal.PSObject.Properties){$journal[$p.Name]=ResolveFixture $p.Value $install $fixture.name}}
  $text=$journal|ConvertTo-Json -Depth 8
  if($fixture.PSObject.Properties['journalText']){$text=$fixture.journalText.Replace('{journal}',$text)}
  WriteText $journalPath $text
  # Files whose status cannot be read stop recovery with every file and the
  # journal as they were; it then goes on once access returns.
  if($fixture.PSObject.Properties['inaccessible']){
   $blocked=@($fixture.inaccessible|ForEach-Object{if($_.StartsWith('backup:')){Join-Path $backup $_.Substring(7)}else{Join-Path $install $_}})
   $failure=$null;$denied=$true
   try {
    foreach($at in $blocked){Readable $at $false;try{$null=[IO.File]::GetAttributes($at);$denied=$false}catch{}}
    try{& $Installer -InstallDir $install -RecoverOnly *>$null}catch{$failure=$_.Exception.Message}
   } finally {foreach($at in $blocked){Readable $at $true}}
   if(!$denied){$wrong+="Recovery fixture $($fixture.name): a file's status could still be read"}
   if($null-eq$failure){$wrong+="Recovery fixture $($fixture.name): succeeded while a file could not be read"}
   if(!(Test-Path -LiteralPath $journalPath -PathType Leaf)-or[IO.File]::ReadAllText($journalPath)-cne$text-or(Test-Path -LiteralPath $failedPath)){$wrong+="Recovery fixture $($fixture.name): journal changed while a file could not be read"}
   foreach($path in $paths.Keys){
    $at=Join-Path $install $path;$want=$fixture.install.PSObject.Properties[$path]
    $have=if(Test-Path -LiteralPath $at -PathType Leaf){[IO.File]::ReadAllText($at)}else{$null}
    if(($want-and$have-cne$want.Value)-or(!$want-and(Test-Path -LiteralPath $at))){$wrong+="Recovery fixture $($fixture.name): $path changed while a file could not be read"}
   }
  }
  $held=@();if($fixture.PSObject.Properties['hold']){foreach($p in $fixture.hold){$held+=[IO.File]::Open((Join-Path $install $p),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)}}
  # Earlier evidence in the way of setting the journal aside.
  $earlier=if($fixture.PSObject.Properties['failed']){$fixture.failed}else{''}
  if($earlier-eq'directory'){$null=New-Item -ItemType Directory -Path $failedPath -Force}
  if($earlier-eq'held'){WriteText $failedPath 'earlier-evidence';$held+=[IO.File]::Open($failedPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)}
  $failure=$null
  try{& $Installer -InstallDir $install -RecoverOnly *>$null}catch{$failure=$_.Exception.Message}finally{foreach($h in $held){$h.Dispose()}}
  $expect=$fixture.expect;$problems=@()
  if(($null-eq$failure)-ne[bool]$expect.success){$problems+=$(if($null-ne$failure){"failed: $failure"}else{'succeeded'})}
  if($expect.PSObject.Properties['error']-and($null-eq$failure-or!$failure.Contains($expect.error))){$problems+="error was: $failure"}
  $setAside=[IO.File]::Exists($failedPath)-and!($earlier-eq'held'-and[IO.File]::ReadAllText($failedPath)-ceq'earlier-evidence')
  $outcome=if(Test-Path -LiteralPath $journalPath){if($setAside){'kept and set aside'}else{'kept'}}elseif($setAside){'setAside'}else{'cleared'}
  if($outcome-cne$expect.journal){$problems+="journal $outcome"}
  elseif($outcome-eq'kept'){
   $left=Get-Content -LiteralPath $journalPath -Raw -Encoding UTF8|ConvertFrom-Json
   if($expect.PSObject.Properties['state']-and$left.state-ne$expect.state){$problems+="journal state $($left.state)"}
   $leftMissing=if($left.PSObject.Properties['missingSkipped']){[string]$left.missingSkipped}else{''}
   $wantMissing=if($expect.PSObject.Properties['missingSkipped']){[string]$expect.missingSkipped}else{''}
   if($leftMissing-cne$wantMissing){$problems+="journal missingSkipped '$leftMissing'"}
  }
  foreach($path in $paths.Keys){
   $at=Join-Path $install $path;$want=$expect.files.PSObject.Properties[$path]
   $have=if(Test-Path -LiteralPath $at -PathType Leaf){[IO.File]::ReadAllText($at)}else{$null}
   if(($want-and$have-cne$want.Value)-or(!$want-and(Test-Path -LiteralPath $at))){$problems+="$path is $(if($null-eq$have){'absent'}else{"'$have'"})"}
  }
  foreach($p in $problems){$wrong+="Recovery fixture $($fixture.name): $p"}
 }
 if($wrong.Count){throw ($wrong -join "`n")}
 Write-Host "Shared recovery fixtures passed ($(@($fixtures.cases).Count) cases)."
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
 $changed=[string[]]@('Launcher.exe','Updater.exe','future.bin','MANIFEST.txt')
 [Array]::Sort($changed,[StringComparer]::Ordinal)
 $metadata=[ordered]@{schema=1;from='1.0.0';to='1.0.1';baseManifestSha256=Hash $baseManifest;targetManifestSha256=Hash $targetManifest;
  changedFiles=$changed;removedFiles=@();installerSha256=Hash (Join-Path $package 'Install-Upgrade.ps1')}
 $upgradeJson=Join-Path $package 'upgrade.json';$metadata|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $upgradeJson -Encoding UTF8
 $inventory=@()
 $manifestPaths=[string[]]@(Get-ChildItem -LiteralPath $package -File -Recurse|Where-Object{$_.Name-ne'UPGRADE_MANIFEST.txt'}|ForEach-Object{$_.FullName})
 [Array]::Sort($manifestPaths,[StringComparer]::Ordinal)
 foreach($path in $manifestPaths){
  $inventory+=ManifestLine $path $path.Substring($package.Length+1)
 }
 Set-Content -LiteralPath (Join-Path $package 'UPGRADE_MANIFEST.txt') -Encoding UTF8 -Value $inventory
 & (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -CheckOnly
 if(Test-Path -LiteralPath (Join-Path $install '.ember-update.lock')){throw 'CheckOnly created a lock file'}
 RecoveryFixtures (Join-Path $package 'Install-Upgrade.ps1') (Join-Path $root 'fixtures')
 # A journal that cannot be parsed is reported once, then set aside, as natively.
 $damaged=Join-Path $root 'damaged-journal';$damagedJournal=Join-Path $damaged '.ember-update-transaction-v1.json'
 WriteText $damagedJournal '{ damaged'
 $failed=$false;try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $damaged -RecoverOnly *>$null}catch{$failed=$true}
 if(!$failed-or(Test-Path -LiteralPath $damagedJournal)-or!(Test-Path -LiteralPath ($damagedJournal+'.failed'))){throw 'Damaged journal was not set aside'}
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
  # Like native recovery, the damaged journal is set aside, reported once.
  if((Test-Path -LiteralPath $transactionPath)-or!(Test-Path -LiteralPath ($transactionPath+'.failed'))){throw 'Damaged backup evidence was not set aside'}
  Remove-Item -LiteralPath ($transactionPath+'.failed')
  Set-Content -LiteralPath $transactionPath -Encoding UTF8 -Value $original
  # A rollback interrupted after restoring Launcher resumes its remaining operations.
  $rollingBack=$original|ConvertFrom-Json
  $rollingBack.state='rolling-back'
  Copy-Item -LiteralPath (Join-Path $rollingBack.backup 'Launcher.exe') -Destination (Join-Path $install 'Launcher.exe') -Force
  $rollingBack|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
 }
 if($boundary-eq 4) {
  # Released updaters recorded unchanged files too, with no missingSkipped
  # marker. Every write had finished, so the update is accepted, not undone.
  $legacy=$original|ConvertFrom-Json
  $unchanged=Join-Path $install 'preflight.ps1';$unchangedHash=Hash $unchanged
  Copy-Item -LiteralPath $unchanged -Destination (Join-Path $legacy.backup 'preflight.ps1')
  $legacy.operations+=@([pscustomobject]@{path='preflight.ps1';existed=$true;priorSha256=$unchangedHash;targetSha256=$unchangedHash})
  $legacy|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $transactionPath -Encoding UTF8
 }
 if($NativeFixture -and $boundary-eq 3) {
  # Start from a real prepared PowerShell journal. Restore Launcher, then fail
  # on Updater. A skipped file edited once the rollback is persisted is left
  # alone when native recovery resumes it. (Edited before, it would mean the
  # folder was replaced, and neither reader would roll back.)
  $prepared=$original|ConvertFrom-Json
  if($prepared.state-ne'prepared'){throw 'Rollback fixture did not start prepared'}
  $skipped=Join-Path $install 'preflight.ps1';$skippedOriginal=Get-Content -LiteralPath $skipped -Raw
  $held=[IO.File]::Open((Join-Path $install 'Updater.exe'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try {
   $failed=$false;try{& (Join-Path $package 'Install-Upgrade.ps1') -InstallDir $install -RecoverOnly}catch{$failed=$true}
   if(!$failed){throw 'Locked Updater did not block PowerShell restoration'}
   if((Get-Content -Raw (Join-Path $install 'Launcher.exe'))-ne'old-launcher'-or(Get-Content -Raw (Join-Path $install 'Updater.exe'))-ne'new-updater'){throw 'Fixture did not reach a partial PowerShell restoration'}
   $rollback=Get-Content -LiteralPath $transactionPath -Raw -Encoding UTF8|ConvertFrom-Json
   if($rollback.state-ne'rolling-back'){throw 'PowerShell restoration did not persist rollback intent'}
   if(Test-Path -LiteralPath ($transactionPath+'.failed')){throw 'Unfinished rollback was set aside'}
   Set-Content -LiteralPath $skipped -Encoding ASCII -NoNewline -Value 'edited-skipped-file'
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
 if($boundary-eq 4) {
  foreach($relative in $changed){if((Hash (Join-Path $install $relative))-ine(Hash (Join-Path $payload $relative))){throw "Finished update was undone: $relative"}}
 } else {
  if((Get-Content -Raw (Join-Path $install 'Launcher.exe'))-ne'old-launcher'){throw 'Launcher was not restored'}
  if((Get-Content -Raw (Join-Path $install 'Updater.exe'))-ne'old-updater'){throw 'Updater was not restored'}
  if((Get-Content -Raw (Join-Path $install 'future.bin'))-ne'user-collision'){throw 'Colliding user file was not restored'}
 }
 if(Test-Path -LiteralPath (Join-Path $install '.ember-update-transaction-v1.json')){throw 'Recovered transaction was not cleared'}
 if((Get-Content -Raw (Join-Path $install 'Launcher.exe.ember-new'))-ne'user-temporary-name'){throw 'Temporary-name collision was overwritten'}
 if($boundary-eq 4) {
  # Legacy committed journals clear without rollback.
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
