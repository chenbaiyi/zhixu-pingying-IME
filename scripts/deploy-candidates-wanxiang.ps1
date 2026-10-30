[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDirectory,
      [ValidatePattern('^InputHost-[a-z0-9-]+\.exe$')][string]$InputHostSourceName = 'InputHost-connectionfix.exe',
      [ValidatePattern('^ImeTsf-[a-z0-9-]+\.dll$')][string]$DllSourceName = 'ImeTsf-connectionfix.dll',
      [ValidatePattern('^[a-z0-9-]+$')][string]$ReleaseName = 'candidates-wanxiang',
      [Parameter(Mandatory)][string]$RuntimeDirectory,
      [Parameter(Mandatory)][string]$CompiledProfileDirectory)

# Scoped paired upgrade. Back up binaries, Rime assets, settings and user profile; preserve learned data.
$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator context required for existing HKLM registration' }
if (-not [Environment]::Is64BitProcess) { throw '64-bit PowerShell required' }
$build = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$newHost = Join-Path $build $InputHostSourceName
$sourceDll = Join-Path $build $DllSourceName
$installedHost = Join-Path $build 'InputHost.exe'
$broker = Join-Path $build 'ZhixuImeBroker.exe'
foreach ($file in @($newHost, $sourceDll, $installedHost, $broker, (Join-Path $build 'PredictService.exe'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing deployment file: $file" }
}
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).ProviderPath
$compiledProfile = (Resolve-Path -LiteralPath $CompiledProfileDirectory).ProviderPath
$compiledBuild = Join-Path $compiledProfile 'build'
$compiledSchema = Join-Path $compiledBuild 'zhixu_pinyin.schema.yaml'
if (-not (Test-Path -LiteralPath $compiledSchema)) { throw 'Compiled schema missing' }
$schemaText = [IO.File]::ReadAllText($compiledSchema)
if ($schemaText -notmatch 'page_size: 8' -or $schemaText -notmatch '2.0.0') { throw 'Expected verified Wanxiang schema with 8 candidates' }
$userData = Join-Path $env:LOCALAPPDATA 'ZhixuIME\user-data'
$settingsFile = Join-Path $env:LOCALAPPDATA 'ZhixuIME\settings.json'
$legacySchema = Join-Path $userData 'build\zhixu_pinyin.schema.yaml'
if (-not (Test-Path -LiteralPath $legacySchema) -or [IO.File]::ReadAllText($legacySchema) -notmatch 'page_size: 5') {
    throw 'This script is the first 5-to-8 Wanxiang upgrade; previous schema must be backed up separately for later upgrades'
}
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$rimeDirectory = Join-Path $build 'rime'
$lock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'third_party.lock.json') -Raw | ConvertFrom-Json
$asset = $lock.assets | Where-Object name -eq 'rime-wanxiang'
foreach ($file in $asset.files | Where-Object { $_.path.StartsWith('assets/rime/full_pinyin/') }) {
    $candidateFile = Join-Path $rimeDirectory ([IO.Path]::GetFileName($file.path))
    if ((Get-FileHash -LiteralPath $candidateFile -Algorithm SHA256).Hash -ne $file.sha256) { throw "Rime staging hash mismatch: $candidateFile" }
}
$classKey = 'Software\Classes\CLSID\{60F059D2-C31E-408C-BF6D-8CE1B46EA420}\InprocServer32'
$registrations = @()
foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    try { $key = $base.OpenSubKey($classKey, $true) } finally { $base.Dispose() }
    if (-not $key) { throw "Existing registration missing: $hive" }
    try {
        $oldDll = [string]$key.GetValue('')
        if (-not $oldDll.StartsWith($runtime + '\', [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $oldDll -PathType Leaf)) { throw "Unexpected registration: $hive $oldDll" }
        $registrations += [pscustomobject]@{ Hive = $hive; Dll = $oldDll; DllKind = $key.GetValueKind(''); ThreadingModel = $key.GetValue('ThreadingModel') }
    } finally { $key.Dispose() }
}
$serviceNames = "Name='ZhixuImeBroker.exe' OR Name='InputHost.exe' OR Name='PredictService.exe'"
$servicePaths = @($broker, $installedHost, (Join-Path $build 'PredictService.exe'))
$currentSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
if (-not ('ZhixuDeploymentProcessQuery' -as [type])) {
    Add-Type 'using System; using System.Text; using System.Runtime.InteropServices; public static class ZhixuDeploymentProcessQuery { [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,bool inherit,uint pid); [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref uint size); [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle); }'
}
function Get-NativeProcessPath([uint32]$ProcessId) {
    $handle = [ZhixuDeploymentProcessQuery]::OpenProcess(0x1000, $false, $ProcessId)
    if ($handle -eq [IntPtr]::Zero) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -eq 87) { return $null }  # Process already exited.
        throw "Unable to verify process image: PID=$ProcessId error=$errorCode"
    }
    try {
        $path = New-Object Text.StringBuilder(32768)
        [uint32]$size = 32768
        if (-not [ZhixuDeploymentProcessQuery]::QueryFullProcessImageName($handle, 0, $path, [ref]$size)) { throw "Native image query failed: PID=$ProcessId" }
        return $path.ToString()
    } finally { [void][ZhixuDeploymentProcessQuery]::CloseHandle($handle) }
}
function Get-ScopedServices {
    $processes = @(Get-CimInstance Win32_Process -Filter $serviceNames | Where-Object { $_.SessionId -eq $currentSession })
    $verified = @()
    foreach ($process in $processes) {
        if (-not $process.ExecutablePath) {
            $imagePath = Get-NativeProcessPath $process.ProcessId
            if (-not $imagePath) { continue }
            $process.ExecutablePath = $imagePath
        }
        if ($process.ExecutablePath -notin $servicePaths) { throw "Unexpected ZhixuIME process path: $($process.ExecutablePath)" }
        $verified += $process
    }
    return $verified
}
function Stop-ScopedServices {
    foreach ($name in @('ZhixuImeBroker.exe', 'InputHost.exe', 'PredictService.exe')) {
        foreach ($process in @(Get-ScopedServices | Where-Object { $_.Name -eq $name })) {
            $current = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.ProcessId)"
            if (-not $current) { continue }
            if (-not $current.ExecutablePath) { $current.ExecutablePath = Get-NativeProcessPath $current.ProcessId }
            if (-not $current.ExecutablePath) { continue }
            if ($current.ExecutablePath -ne $process.ExecutablePath -or $current.CreationDate -ne $process.CreationDate) { throw 'Process identity changed; refusing to stop it' }
            Stop-Process -Id $process.ProcessId
            $waiting = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
            if ($waiting -and -not $waiting.WaitForExit(5000)) { throw "Service did not exit: $name" }
        }
    }
}
$previousProcesses = @(Get-ScopedServices)
$deployment = Join-Path $runtime ($ReleaseName + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$backup = Join-Path $deployment 'backup'
New-Item -ItemType Directory -Path $backup | Out-Null
$fixedDll = Join-Path $deployment 'ImeTsf.dll'
Copy-Item -LiteralPath $sourceDll -Destination $fixedDll
Copy-Item -LiteralPath $installedHost -Destination (Join-Path $backup 'InputHost.exe')
$oldHostHash = (Get-FileHash -LiteralPath (Join-Path $backup 'InputHost.exe') -Algorithm SHA256).Hash
$newHostHash = (Get-FileHash -LiteralPath $newHost -Algorithm SHA256).Hash
$newDllHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $fixedDll -Algorithm SHA256).Hash -ne $newDllHash) { throw 'Staged DLL hash mismatch' }
$processBackup = @($previousProcesses | Select-Object Name, ProcessId, ParentProcessId, SessionId, CreationDate, ExecutablePath)
$metadata = [ordered]@{ Build = $build; Dll = $fixedDll; InputHostHash = $newHostHash; DllHash = $newDllHash; PreviousInputHostHash = $oldHostHash; PreviousRegistrations = $registrations; PreviousProcesses = $processBackup }
[IO.File]::WriteAllText((Join-Path $deployment 'deployment.json'), ($metadata | ConvertTo-Json -Depth 6))
# Reconstruct the exact prior runtime dictionary configuration from preserved
# source assets. The build's staging directory was refreshed during validation.
$previousRime = Join-Path $backup 'rime-previous'
New-Item -ItemType Directory -Path $previousRime | Out-Null
foreach ($name in @('AUTHORS','LICENSE','README.md','pinyin_simp.dict.yaml','default.yaml','zhixu_pinyin.dict.yaml','zhixu_pinyin.schema.yaml')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot ('assets\rime\full_pinyin\' + $name)) -Destination (Join-Path $previousRime $name)
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'assets\rime\wanxiang_source\zhixu_daily.original.dict.yaml') -Destination (Join-Path $previousRime 'zhixu_daily.dict.yaml')
$previousReadme = Join-Path $repositoryRoot 'docs\development\2026-10-09-candidates-wanxiang\README.previous.md'
if (Test-Path -LiteralPath $previousReadme) { Copy-Item -LiteralPath $previousReadme -Destination (Join-Path $previousRime 'README.md') -Force }
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($name in @('default.yaml','zhixu_pinyin.schema.yaml','zhixu_pinyin.dict.yaml')) {
    $path = Join-Path $previousRime $name
    $text = [IO.File]::ReadAllText($path).Replace('page_size: 8','page_size: 5').Replace('  - wanxiang_plain','  - pinyin_simp')
    if ($name -eq 'zhixu_pinyin.schema.yaml') { $text = $text.Replace('version: "2.0.0"','version: "1.2.0"') }
    if ($name -eq 'zhixu_pinyin.dict.yaml') { $text = $text.Replace('version: "2.0.0"','version: "1.1.0"') }
    [IO.File]::WriteAllText($path, $text, $utf8)
}
Copy-Item -LiteralPath $rimeDirectory -Destination (Join-Path $deployment 'rime') -Recurse
$metadata.UserDataDirectory = $userData
$metadata.SettingsFile = $settingsFile
$metadata.CompiledProfileDirectory = $compiledProfile
[IO.File]::WriteAllText((Join-Path $deployment 'deployment.json'), ($metadata | ConvertTo-Json -Depth 6))
$hostChanged = $false
$profileChanged = $false
$settingsChanged = $false
$registryChanged = $false
try {
    Stop-ScopedServices
    if (Test-Path -LiteralPath $userData) { Copy-Item -LiteralPath $userData -Destination (Join-Path $backup 'user-data') -Recurse }
    if (Test-Path -LiteralPath $settingsFile) {
        Copy-Item -LiteralPath $settingsFile -Destination (Join-Path $backup 'settings.json')
        $settings = [IO.File]::ReadAllText($settingsFile) | ConvertFrom-Json -AsHashtable
        $settings.candidate_count = 8
        [IO.File]::WriteAllText($settingsFile, ($settings | ConvertTo-Json -Depth 12), $utf8)
        $settingsChanged = $true
    }
    if (-not (Test-Path -LiteralPath $settingsFile)) {
        [IO.File]::WriteAllText($settingsFile, '{"version":1,"candidate_count":8}', $utf8)
        $settingsChanged = $true
    }
    $userBuild = Join-Path $userData 'build'
    New-Item -ItemType Directory -Force -Path $userBuild | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $compiledBuild -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $userBuild $file.Name) -Force }
    $profileChanged = $true
    Copy-Item -LiteralPath $newHost -Destination $installedHost -Force
    $hostChanged = $true
    if ((Get-FileHash -LiteralPath $installedHost -Algorithm SHA256).Hash -ne $newHostHash) { throw 'Installed InputHost hash mismatch' }
    $switch = & (Join-Path $PSScriptRoot 'switch-tsf.ps1') -DllPath $fixedDll -IncludeExistingMachineRegistration
    $fixedDll = $switch.Dll
    $metadata.Dll = $fixedDll
    [IO.File]::WriteAllText((Join-Path $deployment 'deployment.json'), ($metadata | ConvertTo-Json -Depth 6))
    $registryChanged = $true
    foreach ($registration in $registrations) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($registration.Hive, [Microsoft.Win32.RegistryView]::Registry64)
        try { $key = $base.OpenSubKey($classKey) } finally { $base.Dispose() }
        try { if ($key.GetValue('') -ne $fixedDll) { throw 'Registry post-check failed' } } finally { $key.Dispose() }
    }
    if (-not ('ZhixuCandidatePipeReadiness' -as [type])) {
        Add-Type 'using System.Runtime.InteropServices; public static class ZhixuCandidatePipeReadiness { [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool WaitNamedPipeW(string name,uint timeout); }'
    }
    $pipeName = '\\.\pipe\ZhixuIME.tsf-input.' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $runningBroker = Start-Process -FilePath $broker -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddSeconds(180)
    do {
        Start-Sleep -Milliseconds 200
        $live = @(Get-ScopedServices)
        $hosts = @($live | Where-Object { $_.Name -eq 'InputHost.exe' -and $_.ParentProcessId -eq $runningBroker.Id })
        $pipeReady = [ZhixuCandidatePipeReadiness]::WaitNamedPipeW($pipeName, 1)
        if (-not $pipeReady) { $pipeReady = [Runtime.InteropServices.Marshal]::GetLastWin32Error() -eq 121 }
        $predictions = @($live | Where-Object { $_.Name -eq 'PredictService.exe' -and $_.ParentProcessId -eq $runningBroker.Id })
    } while (($hosts.Count -ne 1 -or $predictions.Count -ne 1 -or -not $pipeReady) -and (Get-Date) -lt $deadline)
    if ($hosts.Count -ne 1 -or $predictions.Count -ne 1 -or -not $pipeReady) { throw 'New Broker children did not become ready' }
    $comObject = [Activator]::CreateInstance([Type]::GetTypeFromCLSID([guid]'60F059D2-C31E-408C-BF6D-8CE1B46EA420'))
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($comObject)
    $loaded = @([Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object { $_.FileName -ieq $fixedDll })
    if ($loaded.Count -ne 1) { throw 'COM activation did not load staged DLL' }
    $live | Select-Object Name, ProcessId, ParentProcessId, ExecutablePath | Format-List
    "DEPLOYMENT=$deployment"
    # Existing TSF profile remains enabled; no registration recreation.
    $metadata.PipeReady = $pipeReady
    $metadata.SettingsChanged = $settingsChanged
    $metadata.ProfileChanged = $profileChanged
    [IO.File]::WriteAllText((Join-Path $deployment 'deployment.json'), ($metadata | ConvertTo-Json -Depth 6))
    'DEPLOYMENT_VERIFIED'
} catch {
    $failure = $_
    Stop-ScopedServices
    if ($registryChanged) {
        foreach ($registration in $registrations) {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($registration.Hive, [Microsoft.Win32.RegistryView]::Registry64)
            try { $key = $base.OpenSubKey($classKey, $true) } finally { $base.Dispose() }
            try {
                if ($key.GetValue('') -ne $fixedDll) { throw 'Registration changed externally; refusing rollback overwrite' }
                $key.SetValue('', $registration.Dll, $registration.DllKind)
                if ($null -eq $registration.ThreadingModel) { $key.DeleteValue('ThreadingModel', $false) }
                else { $key.SetValue('ThreadingModel', $registration.ThreadingModel, [Microsoft.Win32.RegistryValueKind]::String) }
            } finally { $key.Dispose() }
        }
    }
    if ($profileChanged -and (Test-Path -LiteralPath (Join-Path $backup 'user-data\build'))) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $backup 'user-data\build') -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $userData ('build\' + $file.Name)) -Force }
    }
    if ($settingsChanged) {
        if (Test-Path -LiteralPath (Join-Path $backup 'settings.json')) { Copy-Item -LiteralPath (Join-Path $backup 'settings.json') -Destination $settingsFile -Force }
        else { [IO.File]::WriteAllText($settingsFile, '{"version":1,"candidate_count":5}', $utf8) }
    }
    foreach ($file in Get-ChildItem -LiteralPath $previousRime -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $rimeDirectory $file.Name) -Force }
    if ($hostChanged) { Copy-Item -LiteralPath (Join-Path $backup 'InputHost.exe') -Destination $installedHost -Force }
    if ($previousProcesses.Count -gt 0) { Start-Process -FilePath $broker -WindowStyle Hidden }
    throw $failure
}
