[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDirectory,
      [string]$PreparedStoreDirectory,
      [Parameter(Mandatory)][string]$BackupDirectory,
      [switch]$ValidateOnly,[switch]$Rollback,[switch]$FailAfterRouting)
$ErrorActionPreference='Stop'
$taskBuild=(Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$taskBackup=[IO.Path]::GetFullPath($BackupDirectory)
$taskNames=@('InputHost','PredictService','ZhixuImeBroker')
$taskUserRoot=Join-Path $env:LOCALAPPDATA 'ZhixuIME'
$taskPackages=Join-Path $taskUserRoot 'packages'
function Get-ScopedProcesses {
 param([switch]$AllowStarting)
 $taskSession=[Diagnostics.Process]::GetCurrentProcess().SessionId
 $taskItems=@(Get-CimInstance Win32_Process -Filter "Name='InputHost.exe' OR Name='PredictService.exe' OR Name='ZhixuImeBroker.exe'" | Where-Object {$_.SessionId -eq $taskSession})
 foreach($taskItem in $taskItems){
  if(!$taskItem.ExecutablePath){if($AllowStarting){continue};throw "Runtime process path not ready: $($taskItem.ProcessId)"}
  $taskExpected=[IO.Path]::GetFullPath((Join-Path $taskBuild $taskItem.Name))
  if([IO.Path]::GetFullPath($taskItem.ExecutablePath) -ne $taskExpected){throw "Unexpected runtime process: $($taskItem.ProcessId)"}
 }
 return @($taskItems | Where-Object {$_.ExecutablePath})
}
function Stop-ScopedProcesses {
 $taskItems=@(Get-ScopedProcesses)
 foreach($taskItem in ($taskItems | Sort-Object {if($_.Name -eq 'ZhixuImeBroker.exe'){0}else{1}})){
  Stop-Process -Id $taskItem.ProcessId -ErrorAction SilentlyContinue
 }
 foreach($taskItem in $taskItems){Wait-Process -Id $taskItem.ProcessId -Timeout 10 -ErrorAction SilentlyContinue}
 if(@(Get-ScopedProcesses).Count){throw 'Runtime processes did not stop'}
}
function Start-ScopedBroker {
 Start-Process -FilePath (Join-Path $taskBuild 'ZhixuImeBroker.exe') -WorkingDirectory $taskBuild -WindowStyle Hidden
 $taskDeadline=[DateTime]::UtcNow.AddSeconds(20)
 do {
  Start-Sleep -Milliseconds 200
  $taskRunning=@(Get-ScopedProcesses -AllowStarting)
  if(@($taskRunning | Select-Object -ExpandProperty Name -Unique).Count -eq 3){return}
 } while([DateTime]::UtcNow -lt $taskDeadline)
 throw 'Runtime restart failed'
}
if(-not ('ZhixuCorpusMove' -as [type])){
 Add-Type 'using System;using System.Runtime.InteropServices;public static class ZhixuCorpusMove{[DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]public static extern bool MoveFileEx(string from,string to,uint flags);}'
}
function Set-Active([string]$taskPath,$taskContent){
 if($null -eq $taskContent){if(Test-Path -LiteralPath $taskPath){Remove-Item -LiteralPath $taskPath -Force};return}
 New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($taskPath)) | Out-Null
 $taskPending=$taskPath+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
 [IO.File]::WriteAllText($taskPending,$taskContent,[Text.UTF8Encoding]::new($false))
 if(![ZhixuCorpusMove]::MoveFileEx($taskPending,$taskPath,9)){throw 'Atomic activation failed'}
}
function Restore-Routing($taskManifest){
 $taskRoutingFile=Join-Path $taskPackages '.routing\routing.sqlite'
 if($taskManifest.routing_present){
  $taskSaved=Join-Path $taskBackup 'routing.sqlite'
  if((Get-FileHash -LiteralPath $taskSaved -Algorithm SHA256).Hash -ne $taskManifest.routing_sha256){throw 'Routing backup hash mismatch'}
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($taskRoutingFile)) | Out-Null
  $taskRestore=$taskRoutingFile+'.restore';Copy-Item -LiteralPath $taskSaved -Destination $taskRestore -Force
  if(![ZhixuCorpusMove]::MoveFileEx($taskRestore,$taskRoutingFile,9)){throw 'Routing restore failed'}
 } elseif(Test-Path -LiteralPath $taskRoutingFile){Remove-Item -LiteralPath $taskRoutingFile -Force}
}
if($Rollback){
 $taskManifest=Get-Content -LiteralPath (Join-Path $taskBackup 'deployment.json') -Raw | ConvertFrom-Json
 if($taskManifest.build -ne $taskBuild -or $taskManifest.packages -ne $taskPackages){throw 'Rollback boundary mismatch'}
 foreach($taskFile in $taskManifest.files){
  if($taskFile.name -notin @($taskNames | ForEach-Object {$_+".exe"})){throw "Unsafe rollback filename"}
  $taskSaved=Join-Path $taskBackup $taskFile.name
  if((Get-FileHash -LiteralPath $taskSaved -Algorithm SHA256).Hash -ne $taskFile.sha256){throw 'Rollback hash mismatch'}
 }
 foreach($taskActivation in $taskManifest.activations){if($taskActivation.id -notmatch '^com\.zhixu\.[a-z0-9.-]+$'){throw 'Unsafe rollback package id'}}
 if($taskManifest.routing_present -and (Get-FileHash -LiteralPath (Join-Path $taskBackup 'routing.sqlite') -Algorithm SHA256).Hash -ne $taskManifest.routing_sha256){throw 'Routing backup hash mismatch'}
 if($ValidateOnly){'ROLLBACK_PRECHECK_OK';exit 0}
 Stop-ScopedProcesses
 foreach($taskFile in $taskManifest.files){Copy-Item -LiteralPath (Join-Path $taskBackup $taskFile.name) -Destination (Join-Path $taskBuild $taskFile.name) -Force}
 foreach($taskActivation in $taskManifest.activations){Set-Active (Join-Path (Join-Path $taskPackages $taskActivation.id) 'active.json') $taskActivation.previous}
 Restore-Routing $taskManifest
 Start-ScopedBroker
 'ROLLED_BACK';exit 0
}
foreach($taskName in $taskNames){
 foreach($taskSuffix in @('.exe','-corpus.exe')){if(!(Test-Path -LiteralPath (Join-Path $taskBuild ($taskName+$taskSuffix)) -PathType Leaf)){throw "Missing binary: $taskName$taskSuffix"}}
}
$taskPrepared=(Resolve-Path -LiteralPath $PreparedStoreDirectory).ProviderPath
if([IO.Path]::GetPathRoot($taskPrepared) -ne [IO.Path]::GetPathRoot($taskPackages)){throw 'Prepared store must be on the runtime volume for atomic directory moves'}
if(!(Test-Path -LiteralPath (Join-Path $taskPrepared '.routing\routing.sqlite') -PathType Leaf)){throw 'Prepared routing index missing'}
$taskActivations=@()
foreach($taskFolder in Get-ChildItem -LiteralPath $taskPrepared -Directory){
 if($taskFolder.Name.StartsWith('.')){continue}
 if($taskFolder.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Prepared package reparse point'}
 $taskId=$taskFolder.Name
 if($taskId -notmatch '^com\.zhixu\.[a-z0-9.-]+$'){throw "Unexpected package id: $taskId"}
 $taskActive=Get-Content -LiteralPath (Join-Path $taskFolder.FullName 'active.json') -Raw
 $taskVersion=($taskActive | ConvertFrom-Json).version
 if($taskVersion -notmatch '^\d+\.\d+\.\d+$'){throw 'Unsafe version'}
 $taskSource=Join-Path $taskFolder.FullName $taskVersion
 if(!(Test-Path -LiteralPath (Join-Path $taskSource 'indexes\lookup.sqlite'))){throw 'Prepared lookup index missing'}
 $taskDestination=Join-Path (Join-Path $taskPackages $taskId) $taskVersion
 if(![IO.Path]::GetFullPath($taskSource).StartsWith($taskPrepared+'\',[StringComparison]::OrdinalIgnoreCase) -or ![IO.Path]::GetFullPath($taskDestination).StartsWith($taskPackages+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Package move boundary mismatch'}
 if(Test-Path -LiteralPath $taskDestination){
  $taskOld=Get-Content -LiteralPath (Join-Path $taskDestination 'manifest.json') -Raw | ConvertFrom-Json
  $taskNew=Get-Content -LiteralPath (Join-Path $taskSource 'manifest.json') -Raw | ConvertFrom-Json
  if($taskOld.entries.sha256 -ne $taskNew.entries.sha256){throw 'Existing immutable version differs'}
 }
 $taskExisting=Join-Path (Join-Path $taskPackages $taskId) 'active.json';$taskPrevious=$null
 if(Test-Path -LiteralPath $taskExisting){$taskPrevious=Get-Content -LiteralPath $taskExisting -Raw}
 $taskActivations += [pscustomobject]@{id=$taskId;version=$taskVersion;source=$taskSource;destination=$taskDestination;active=$taskActive;previous=$taskPrevious}
}
if(!$taskActivations.Count){throw 'No prepared packages'}
[void](Get-ScopedProcesses)
if($ValidateOnly){"DEPLOYMENT_PRECHECK_OK packages=$($taskActivations.Count)";exit 0}
if(Test-Path -LiteralPath $taskBackup){throw 'Backup directory must be new'}
New-Item -ItemType Directory -Path $taskBackup | Out-Null
$taskFiles=@()
foreach($taskName in $taskNames){
 $taskFile=Join-Path $taskBuild ($taskName+'.exe');Copy-Item -LiteralPath $taskFile -Destination (Join-Path $taskBackup ($taskName+'.exe'))
 $taskFiles += [pscustomobject]@{name=$taskName+'.exe';sha256=(Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash}
}
$taskRoutingFile=Join-Path $taskPackages '.routing\routing.sqlite'
$taskRoutingPresent=Test-Path -LiteralPath $taskRoutingFile
$taskRoutingHash=$null
if($taskRoutingPresent){Copy-Item -LiteralPath $taskRoutingFile -Destination (Join-Path $taskBackup 'routing.sqlite');$taskRoutingHash=(Get-FileHash -LiteralPath $taskRoutingFile -Algorithm SHA256).Hash}
$taskDeployment=[pscustomobject]@{routing_present=$taskRoutingPresent;routing_sha256=$taskRoutingHash;build=$taskBuild;packages=$taskPackages;files=$taskFiles;activations=$taskActivations;created=[DateTime]::UtcNow.ToString('o')}
$taskDeployment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskBackup 'deployment.json') -Encoding utf8
$taskMoved=@()
try {
 Stop-ScopedProcesses
 foreach($taskActivation in $taskActivations){
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($taskActivation.destination)) | Out-Null
  if(!(Test-Path -LiteralPath $taskActivation.destination)){if(![ZhixuCorpusMove]::MoveFileEx($taskActivation.source,$taskActivation.destination,8)){throw 'Atomic package directory move failed'};$taskMoved+=$taskActivation}
  Set-Active (Join-Path (Join-Path $taskPackages $taskActivation.id) 'active.json') $taskActivation.active
 }
 $taskRouting=Join-Path $taskPackages '.routing';New-Item -ItemType Directory -Force -Path $taskRouting | Out-Null
 $taskPendingRouting=Join-Path $taskRouting 'routing.deploy.tmp';Copy-Item -LiteralPath (Join-Path $taskPrepared '.routing\routing.sqlite') -Destination $taskPendingRouting -Force
 if(![ZhixuCorpusMove]::MoveFileEx($taskPendingRouting,(Join-Path $taskRouting 'routing.sqlite'),9)){throw 'Routing activation failed'}
 if($FailAfterRouting){throw 'Injected failure after routing activation'}
 foreach($taskName in $taskNames){Copy-Item -LiteralPath (Join-Path $taskBuild ($taskName+'-corpus.exe')) -Destination (Join-Path $taskBuild ($taskName+'.exe')) -Force}
 Start-ScopedBroker
 [pscustomobject]@{state='deployed';packages=$taskActivations.Count;backup=$taskBackup;processes=@(Get-ScopedProcesses | Select-Object Name,ProcessId,ExecutablePath)} | ConvertTo-Json -Depth 5
} catch {
 $taskFailure=$_
 Stop-ScopedProcesses
 foreach($taskFile in $taskFiles){Copy-Item -LiteralPath (Join-Path $taskBackup $taskFile.name) -Destination (Join-Path $taskBuild $taskFile.name) -Force}
 foreach($taskActivation in $taskActivations){Set-Active (Join-Path (Join-Path $taskPackages $taskActivation.id) 'active.json') $taskActivation.previous}
 Restore-Routing $taskDeployment
 foreach($taskActivation in $taskMoved){
  if(![IO.Path]::GetFullPath($taskActivation.source).StartsWith($taskPrepared+'\',[StringComparison]::OrdinalIgnoreCase) -or ![IO.Path]::GetFullPath($taskActivation.destination).StartsWith($taskPackages+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Recovery move boundary mismatch'}
  if(![ZhixuCorpusMove]::MoveFileEx($taskActivation.destination,$taskActivation.source,8)){throw 'Atomic package recovery move failed'}
 }
 Start-ScopedBroker
 throw $taskFailure
}
