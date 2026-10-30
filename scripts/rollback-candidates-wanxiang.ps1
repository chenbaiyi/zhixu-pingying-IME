[CmdletBinding()]
param([Parameter(Mandatory)][string]$DeploymentPath, [switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
$deployment = (Resolve-Path -LiteralPath $DeploymentPath).ProviderPath
if ((Split-Path $deployment -Leaf) -notmatch '^candidates-wanxiang-[0-9-]+-[a-f0-9]{8}$') { throw 'Expected this upgrade deployment directory' }
$metadata = Get-Content -LiteralPath (Join-Path $deployment 'deployment.json') -Raw | ConvertFrom-Json
$build = (Resolve-Path -LiteralPath $metadata.Build).ProviderPath
$backup = Join-Path $deployment 'backup'
$installed = Join-Path $build 'InputHost.exe'
if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne $metadata.InputHostHash) { throw 'InputHost changed since deployment; refusing overwrite' }
$classKey = 'Software\Classes\CLSID\{60F059D2-C31E-408C-BF6D-8CE1B46EA420}\InprocServer32'
$keys = @()
try {
    foreach ($registration in $metadata.PreviousRegistrations) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]$registration.Hive, [Microsoft.Win32.RegistryView]::Registry64)
        try { $key = $base.OpenSubKey($classKey, $true) } finally { $base.Dispose() }
        if (-not $key -and [Microsoft.Win32.RegistryHive]$registration.Hive -eq [Microsoft.Win32.RegistryHive]::CurrentUser) { continue } # Do not recreate a missing user override.
        if (-not $key -or $key.GetValue('') -ne $metadata.Dll) { throw 'Registration changed since deployment; refusing overwrite' }
        $keys += [pscustomobject]@{Key=$key; Previous=$registration}
    }
    foreach ($name in @('InputHost.exe','rime-previous\zhixu_pinyin.dict.yaml','user-data\build\zhixu_pinyin.table.bin')) {
        if (-not (Test-Path -LiteralPath (Join-Path $backup $name))) { throw "Rollback backup missing: $name" }
    }
    if ($ValidateOnly) { 'ROLLBACK_BACKUP_AND_REGISTRATION_VALIDATED'; return }
    $paths = @((Join-Path $build 'ZhixuImeBroker.exe'),$installed,(Join-Path $build 'PredictService.exe'))
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    foreach ($path in $paths) {
        foreach ($process in Get-CimInstance Win32_Process -Filter "Name='$([IO.Path]::GetFileName($path))'" | Where-Object SessionId -eq $session) {
            if ($process.ExecutablePath -ine $path) { throw 'Unexpected process path' }
            $current = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.ProcessId)"
            if ($current -and $current.ExecutablePath -ieq $path -and $current.CreationDate -eq $process.CreationDate) {
                Stop-Process -Id $current.ProcessId
                $running = Get-Process -Id $current.ProcessId -ErrorAction SilentlyContinue
                if ($running -and -not $running.WaitForExit(5000)) { throw 'Service did not exit' }
            }
        }
    }
    Copy-Item -LiteralPath (Join-Path $backup 'InputHost.exe') -Destination $installed -Force
    foreach ($name in @('ZhixuImeBroker.exe','PredictService.exe')) {
        $previousBinary = Join-Path $backup $name
        if (Test-Path -LiteralPath $previousBinary) { Copy-Item -LiteralPath $previousBinary -Destination (Join-Path $build $name) -Force }
    }
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $backup 'rime-previous') -File) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $build ('rime\' + $file.Name)) -Force
    }
    $oldBuild = Join-Path $backup 'user-data\build'
    if (Test-Path -LiteralPath $oldBuild) {
        foreach ($file in Get-ChildItem -LiteralPath $oldBuild -File) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $metadata.UserDataDirectory ('build\' + $file.Name)) -Force
        }
    }
    # Preserve learning performed after the upgrade; restore compiled resources only.
    if (Test-Path -LiteralPath $metadata.SettingsFile) {
        $settings = Get-Content -LiteralPath $metadata.SettingsFile -Raw | ConvertFrom-Json -AsHashtable
        $previousCount = 5
        if (Test-Path -LiteralPath (Join-Path $backup 'settings.json')) {
            $oldSettings = Get-Content -LiteralPath (Join-Path $backup 'settings.json') -Raw | ConvertFrom-Json -AsHashtable
            if ($oldSettings.ContainsKey('candidate_count')) { $previousCount = $oldSettings.candidate_count }
        }
        $settings.candidate_count = $previousCount
        [IO.File]::WriteAllText($metadata.SettingsFile, ($settings | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    }
    foreach ($item in $keys) {
        $item.Key.SetValue('', $item.Previous.Dll, [Microsoft.Win32.RegistryValueKind]$item.Previous.DllKind)
        if ($null -eq $item.Previous.ThreadingModel) { $item.Key.DeleteValue('ThreadingModel', $false) }
        else { $item.Key.SetValue('ThreadingModel', $item.Previous.ThreadingModel, [Microsoft.Win32.RegistryValueKind]::String) }
    }
    Start-Process -FilePath (Join-Path $build 'ZhixuImeBroker.exe') -WindowStyle Hidden
    'ROLLBACK_COMPLETED'
} finally { foreach ($item in $keys) { $item.Key.Dispose() } }
