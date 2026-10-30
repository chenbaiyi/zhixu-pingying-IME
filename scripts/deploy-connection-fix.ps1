[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDirectory,
      [ValidatePattern('^InputHost-[a-z0-9-]+\.exe$')][string]$InputHostSourceName = 'InputHost-connectionfix.exe',
      [ValidatePattern('^ImeTsf-[a-z0-9-]+\.dll$')][string]$DllSourceName = 'ImeTsf-connectionfix.dll',
      [ValidatePattern('^[a-z0-9-]+$')][string]$ReleaseName = 'connection')

# Scoped, reversible deployment. Never close user applications or edit Rime data.
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
$runtime = Join-Path $env:LOCALAPPDATA 'ZhixuIME\runtime'
$classKey = 'Software\Classes\CLSID\{60F059D2-C31E-408C-BF6D-8CE1B46EA420}\InprocServer32'
$registrations = @()
foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    try { $key = $base.OpenSubKey($classKey, $true) } finally { $base.Dispose() }
    if (-not $key) { continue }
    try {
        $oldDll = [string]$key.GetValue('')
        if (-not $oldDll.StartsWith($runtime + '\', [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $oldDll -PathType Leaf)) { throw "Unexpected registration: $hive $oldDll" }
        $registrations += [pscustomobject]@{ Hive = $hive; Dll = $oldDll; DllKind = $key.GetValueKind(''); ThreadingModel = $key.GetValue('ThreadingModel') }
    } finally { $key.Dispose() }
}
if ($registrations.Count -eq 0) { throw 'Existing ZhixuIME registration is required' }
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
$hostChanged = $false
$registryChanged = $false
try {
    Stop-ScopedServices
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
    $runningBroker = Start-Process -FilePath $broker -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 200
        $live = @(Get-ScopedServices)
        $hosts = @($live | Where-Object { $_.Name -eq 'InputHost.exe' -and $_.ParentProcessId -eq $runningBroker.Id })
        $predictions = @($live | Where-Object { $_.Name -eq 'PredictService.exe' -and $_.ParentProcessId -eq $runningBroker.Id })
    } while (($hosts.Count -ne 1 -or $predictions.Count -ne 1) -and (Get-Date) -lt $deadline)
    if ($hosts.Count -ne 1 -or $predictions.Count -ne 1) { throw 'New Broker children did not become ready' }
    $comObject = [Activator]::CreateInstance([Type]::GetTypeFromCLSID([guid]'60F059D2-C31E-408C-BF6D-8CE1B46EA420'))
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($comObject)
    $loaded = @([Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object { $_.FileName -ieq $fixedDll })
    if ($loaded.Count -ne 1) { throw 'COM activation did not load staged DLL' }
    $live | Select-Object Name, ProcessId, ParentProcessId, ExecutablePath | Format-List
    "DEPLOYMENT=$deployment"
    & (Join-Path $PSScriptRoot 'enable-user-profile.ps1')
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
    if ($hostChanged) { Copy-Item -LiteralPath (Join-Path $backup 'InputHost.exe') -Destination $installedHost -Force }
    if ($previousProcesses.Count -gt 0) { Start-Process -FilePath $broker -WindowStyle Hidden }
    throw $failure
}
