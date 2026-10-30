[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDirectory,
      [Parameter(Mandatory)][string]$CompiledProfileDirectory,
      [Parameter(Mandatory)][string]$UserDataDirectory,
      [Parameter(Mandatory)][string]$PreviousSchemaPath,
      [Parameter(Mandatory)][string]$BackupDirectory,
      [ValidatePattern('^\d+\.\d+\.\d+$')][string]$ExpectedPreviousVersion = '2.0.0',
      [ValidatePattern('^\d+\.\d+\.\d+$')][string]$ExpectedVersion = '2.1.0',
      [switch]$ValidateOnly, [switch]$Rollback)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$compiled = Join-Path (Resolve-Path -LiteralPath $CompiledProfileDirectory).ProviderPath 'build'
$userData = (Resolve-Path -LiteralPath $UserDataDirectory).ProviderPath
$previousSchema = (Resolve-Path -LiteralPath $PreviousSchemaPath).ProviderPath
$schemaName = 'zhixu_pinyin.schema.yaml'
$sourceSchema = Join-Path $build ('rime\' + $schemaName)
$userBuild = Join-Path $userData 'build'
$testExe = Join-Path $build 'zhixu_input_host_tests.exe'
$oldCompiledSchema = Join-Path $userBuild $schemaName
foreach ($file in @((Join-Path $compiled $schemaName),$sourceSchema,$testExe,
                    (Join-Path $compiled 'zhixu_pinyin.prism.bin'),
                    (Join-Path $compiled 'zhixu_pinyin.table.bin'),(Join-Path $build 'ZhixuImeBroker.exe'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing deployment input: $file" }
}
$versionPattern = '(?m)^\s*version: ["'']?' + [regex]::Escape($ExpectedVersion) + '["'']?\s*$'
$previousVersionPattern = '(?m)^\s*version: ["'']?' + [regex]::Escape($ExpectedPreviousVersion) + '["'']?\s*$'
if ([IO.File]::ReadAllText((Join-Path $compiled $schemaName)) -notmatch $versionPattern -or
    [IO.File]::ReadAllText((Join-Path $compiled $schemaName)) -notmatch 'page_size: 8') { throw "Expected verified compiled schema $ExpectedVersion with eight candidates" }
if ([IO.File]::ReadAllText($sourceSchema) -notmatch $versionPattern) { throw 'Runtime schema not staged' }
if (-not $Rollback -and [IO.File]::ReadAllText($oldCompiledSchema) -notmatch $previousVersionPattern) { throw "Expected existing user profile $ExpectedPreviousVersion" }
$servicePaths = @('ZhixuImeBroker.exe','InputHost.exe','PredictService.exe') | ForEach-Object { Join-Path $build $_ }
$currentSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
function Get-ScopedServices {
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='ZhixuImeBroker.exe' OR Name='InputHost.exe' OR Name='PredictService.exe'" | Where-Object SessionId -eq $currentSession)
    foreach ($process in $processes) {
        if (-not $process.ExecutablePath -or $servicePaths -notcontains $process.ExecutablePath) { throw "Unexpected IME process path for PID $($process.ProcessId)" }
    }
    return $processes | Select-Object Name,ProcessId,ParentProcessId,SessionId,CreationDate,ExecutablePath
}
function Stop-ScopedServices {
    foreach ($name in @('ZhixuImeBroker.exe','InputHost.exe','PredictService.exe')) {
        foreach ($process in @(Get-ScopedServices | Where-Object Name -eq $name)) {
            $current = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.ProcessId)"
            if (-not $current) { continue }
            if ($current.ExecutablePath -ne $process.ExecutablePath -or $current.CreationDate -ne $process.CreationDate) { throw 'Process identity changed; refusing to stop' }
            Stop-Process -Id $process.ProcessId
            $waiting = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
            if ($waiting -and -not $waiting.WaitForExit(5000)) { throw 'IME service did not exit' }
        }
    }
}
function Get-FileRecords([string]$Directory) {
    return @(Get-ChildItem -LiteralPath $Directory -File | ForEach-Object {
        [pscustomobject]@{ Name=$_.Name; Sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function Validate-Backup($Metadata,[string]$Backup) {
    if ($Metadata.Build -ne $build -or $Metadata.UserData -ne $userData -or -not $Metadata.PreviousFiles) { throw 'Backup identity or file inventory missing' }
    foreach ($file in @($Metadata.PreviousFiles) + @($Metadata.NewFiles)) {
        if ([IO.Path]::GetFileName($file.Name) -ne $file.Name -or $file.Name -notmatch '^[a-zA-Z0-9_.-]+$') { throw 'Unsafe compiled file name' }
    }
    foreach ($file in $Metadata.PreviousFiles) {
        $saved = Join-Path $Backup ('user-data\build\' + $file.Name)
        if (-not (Test-Path -LiteralPath $saved -PathType Leaf) -or
            (Get-FileHash -LiteralPath $saved -Algorithm SHA256).Hash -ne $file.Sha256) { throw "Backup file missing or changed: $($file.Name)" }
    }
    $savedSchema = Join-Path $Backup ('rime\' + $schemaName)
    if (-not (Test-Path -LiteralPath $savedSchema -PathType Leaf) -or
        (Get-FileHash -LiteralPath $savedSchema -Algorithm SHA256).Hash -ne $Metadata.PreviousSchemaHash) { throw 'Backup runtime schema missing or changed' }
}
function Restore-Cache($Metadata,[string]$Backup) {
    Validate-Backup $Metadata $Backup
    foreach ($file in $Metadata.NewFiles) {
        $target = [IO.Path]::GetFullPath((Join-Path $userBuild $file.Name))
        if (-not $target.StartsWith($userBuild + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe rollback target' }
        $old = @($Metadata.PreviousFiles | Where-Object Name -eq $file.Name)
        if ($old.Count -eq 1) {
            Copy-Item -LiteralPath (Join-Path $Backup ('user-data\build\' + $file.Name)) -Destination $target -Force
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $old[0].Sha256) { throw 'Restored cache hash mismatch' }
        } else { Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue }
    }
    Copy-Item -LiteralPath (Join-Path $Backup ('rime\' + $schemaName)) -Destination $sourceSchema -Force
}
function Start-And-Probe([string]$LogPath) {
    $broker = Start-Process -FilePath (Join-Path $build 'ZhixuImeBroker.exe') -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 200
        $live = @(Get-ScopedServices)
        $hostReady = @($live | Where-Object { $_.Name -eq 'InputHost.exe' -and $_.ParentProcessId -eq $broker.Id }).Count -eq 1
        $predictReady = @($live | Where-Object { $_.Name -eq 'PredictService.exe' -and $_.ParentProcessId -eq $broker.Id }).Count -eq 1
    } while ((-not $hostReady -or -not $predictReady) -and (Get-Date) -lt $deadline)
    if (-not $hostReady -or -not $predictReady) { throw 'Broker children not started' }
    # Serve/listen is reached only after RimeEngine reports available in main.cpp.
    # Do not send Hello/keys: production deliberately rejects non-foreground clients.
    if (-not ('PinyinPipeIdentity' -as [type])) {
        Add-Type 'using System.Runtime.InteropServices;using Microsoft.Win32.SafeHandles;public static class PinyinPipeIdentity{[DllImport("kernel32.dll",SetLastError=true)]public static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe,out uint id);}'
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $pipeName = 'ZhixuIME.tsf-input.' + $sid
    $ready = $false
    $deadline = (Get-Date).AddSeconds(15)
    do {
        $client = [IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
        try {
            $client.Connect(500)
            $serverId = [uint32]0
            if ([PinyinPipeIdentity]::GetNamedPipeServerProcessId($client.SafePipeHandle,[ref]$serverId)) {
                $live = @(Get-ScopedServices)
                $ready = @($live | Where-Object { $_.Name -eq 'InputHost.exe' -and $_.ProcessId -eq $serverId -and $_.ParentProcessId -eq $broker.Id }).Count -eq 1
            }
        } catch [TimeoutException] {} catch [IO.IOException] {} finally { $client.Dispose() }
        if (-not $ready) { Start-Sleep -Milliseconds 100 }
    } while (-not $ready -and (Get-Date) -lt $deadline)
    if (-not $ready) { throw 'Live InputHost pipe identity/readiness not verified' }
    "PIPE_READY_SERVER_PID=$serverId; Broker=$($broker.Id); No input sent." | Set-Content -LiteralPath $LogPath -Encoding UTF8
    $live = @(Get-ScopedServices)
    if (@($live | Where-Object ProcessId -eq $broker.Id).Count -ne 1 -or $live.Count -ne 3) { throw 'Service identity changed after readiness check' }
    return $live
}
$previous = @(Get-ScopedServices)
if ($previous.Count -ne 3) { throw 'Expected exactly three scoped IME services' }
$backup = [IO.Path]::GetFullPath($BackupDirectory)
if ($Rollback) {
    $metadata = Get-Content -LiteralPath (Join-Path $backup 'deployment.json') -Raw | ConvertFrom-Json
    Validate-Backup $metadata $backup
    if ($metadata.State -ne 'deployed' -or (Get-FileHash -LiteralPath $sourceSchema -Algorithm SHA256).Hash -ne $metadata.SchemaHash) { throw 'Runtime changed since deployment; refusing overwrite' }
    if ($ValidateOnly) { 'ROLLBACK_VALIDATION_PASS'; return }
    try {
        Stop-ScopedServices
        Restore-Cache $metadata $backup
        $live = @(Start-And-Probe (Join-Path $backup 'rollback-probe.log'))
        'ROLLBACK_VERIFIED_LEARNING_PRESERVED'
    } catch {
        $failure = $_
        try {
            Stop-ScopedServices
            Restore-Cache $metadata $backup
            [void](Start-And-Probe (Join-Path $backup 'rollback-recovery-probe.log'))
            Write-Warning 'Rollback reported an error; old resources and service readiness were recovered.'
        } catch { Write-Warning "Recovery also failed: $_. Backup preserved at $backup" }
        throw $failure
    }
    return
}
if ($ValidateOnly) { 'VALIDATION_PASS'; return }
if (Test-Path -LiteralPath $backup) { throw 'Backup destination must be new' }
New-Item -ItemType Directory -Path $backup | Out-Null
$enteredStop = $false
$changed = $false
try {
    # Enter recovery before the first stop: even a partial stop must be repaired.
    $enteredStop = $true
    Stop-ScopedServices
    Copy-Item -LiteralPath $userData -Destination (Join-Path $backup 'user-data') -Recurse
    Copy-Item -LiteralPath (Join-Path $build 'rime') -Destination (Join-Path $backup 'rime') -Recurse
    Copy-Item -LiteralPath $previousSchema -Destination (Join-Path $backup ('rime\' + $schemaName)) -Force
    $metadata = [ordered]@{ Build=$build; UserData=$userData; CompiledProfile=$compiled;
        Version=$ExpectedVersion; PreviousVersion=$ExpectedPreviousVersion;
        SchemaHash=(Get-FileHash -LiteralPath $sourceSchema -Algorithm SHA256).Hash;
        PreviousSchemaHash=(Get-FileHash -LiteralPath $previousSchema -Algorithm SHA256).Hash;
        PreviousFiles=@(Get-FileRecords (Join-Path $backup 'user-data\build'));
        NewFiles=@(Get-FileRecords $compiled); PreviousProcesses=$previous; State='backed-up' }
    $metadata | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $backup 'deployment.json') -Encoding UTF8
    Validate-Backup $metadata $backup
    $changed = $true
    foreach ($file in $metadata.NewFiles) {
        $target = Join-Path $userBuild $file.Name
        Copy-Item -LiteralPath (Join-Path $compiled $file.Name) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.Sha256) { throw 'Compiled cache hash mismatch' }
    }
    # Only compiled resources are replaced. Learning directories are never restored over live data.
    $live = @(Start-And-Probe (Join-Path $backup 'deployment-probe.log'))
    if ([IO.File]::ReadAllText($oldCompiledSchema) -notmatch $versionPattern) { throw 'Post-start schema version mismatch' }
    $metadata.State='deployed'; $metadata.LiveProcesses=$live
    $metadata | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $backup 'deployment.json') -Encoding UTF8
    "DEPLOYMENT_BACKUP=$backup"
    'DEPLOYMENT_VERIFIED'
} catch {
    $failure = $_
    if ($enteredStop) {
        try {
            Stop-ScopedServices
            if ($changed) { Restore-Cache $metadata $backup }
            else { Copy-Item -LiteralPath $previousSchema -Destination $sourceSchema -Force }
            [void](Start-And-Probe (Join-Path $backup 'deployment-recovery-probe.log'))
            Write-Warning 'Deployment failed; prior resources and service readiness recovered.'
        } catch { Write-Warning "Recovery also failed: $_. Backup preserved at $backup" }
    }
    throw $failure
}
