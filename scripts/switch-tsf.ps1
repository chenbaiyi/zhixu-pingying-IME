[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DllPath,
    [switch]$IncludeExistingMachineRegistration
)

# Switch only the existing ZhixuIME COM class. Do not stop applications or
# unregister/recreate TSF profiles. Machine registration is explicitly opt-in.
$ErrorActionPreference = 'Stop'
$newDll = (Resolve-Path -LiteralPath $DllPath).ProviderPath
# Packaged desktop hosts can redirect LocalAppData into their own LocalCache.
# COM registration is shared by other applications, so store the physical path.
if (-not ('ZhixuTsfRegistrationPath' -as [type])) {
    Add-Type @"
using System.Text;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ZhixuTsfRegistrationPath {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder path, uint length, uint flags);
}
"@
}
$stream = [IO.File]::Open($newDll, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    $physicalPath = [Text.StringBuilder]::new(32768)
    $length = [ZhixuTsfRegistrationPath]::GetFinalPathNameByHandleW($stream.SafeFileHandle, $physicalPath, 32768, 0)
    if ($length -eq 0 -or $length -ge 32768) { throw 'Unable to resolve physical TSF DLL path' }
    $newDll = $physicalPath.ToString()
    if ($newDll.StartsWith('\\?\UNC\')) { $newDll = '\\' + $newDll.Substring(8) }
    elseif ($newDll.StartsWith('\\?\')) { $newDll = $newDll.Substring(4) }
} finally { $stream.Dispose() }
if ([IO.Path]::GetExtension($newDll) -ne '.dll') { throw 'Expected a TSF DLL' }
$image = [IO.File]::ReadAllBytes($newDll)
if ($image.Length -lt 64 -or [BitConverter]::ToUInt16($image, 0) -ne 0x5a4d) {
    throw 'Expected a Windows PE image'
}
$peOffset = [BitConverter]::ToInt32($image, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $image.Length -or
    [BitConverter]::ToUInt32($image, $peOffset) -ne 0x4550 -or
    [BitConverter]::ToUInt16($image, $peOffset + 4) -ne 0x8664) {
    throw 'ZhixuIME requires an x64 DLL'
}

$subkey = 'Software\Classes\CLSID\{60F059D2-C31E-408C-BF6D-8CE1B46EA420}\InprocServer32'
$targets = @([pscustomobject]@{ Hive = [Microsoft.Win32.RegistryHive]::CurrentUser; Name = 'HKCU' })
if ($IncludeExistingMachineRegistration) {
    $targets += [pscustomobject]@{ Hive = [Microsoft.Win32.RegistryHive]::LocalMachine; Name = 'HKLM' }
}
$opened = @()
try {
    # Acquire every required write handle before changing either registry view.
    foreach ($target in $targets) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($target.Hive, [Microsoft.Win32.RegistryView]::Registry64)
        try { $key = $base.OpenSubKey($subkey, $true) } finally { $base.Dispose() }
        # Update only registrations that already exist. Some installations are
        # machine-registered only, while others carry a current-user override.
        # Never create either registration implicitly.
        if ($null -eq $key) { continue }
        $opened += [pscustomobject]@{
            Name = $target.Name; Key = $key
            PreviousDll = $key.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            PreviousDllKind = $key.GetValueKind('')
            PreviousThreadingModel = $key.GetValue('ThreadingModel')
        }
    }
    if ($opened.Count -eq 0) { throw 'Existing ZhixuIME registration is required' }
    $backup = Join-Path (Split-Path -Parent $newDll) ('tsf-registration-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    if (Test-Path -LiteralPath $backup) { throw 'Backup directory already exists' }
    New-Item -ItemType Directory -Path $backup | Out-Null
    foreach ($target in $opened) {
        & "$env:SystemRoot\System32\reg.exe" export ($target.Name + '\' + $subkey) (Join-Path $backup ($target.Name + '.reg')) /y
        if ($LASTEXITCODE -ne 0) { throw "Could not back up $($target.Name) registration" }
    }
    try {
        foreach ($target in $opened) {
            $target.Key.SetValue('', $newDll, [Microsoft.Win32.RegistryValueKind]::String)
            $target.Key.SetValue('ThreadingModel', 'Apartment', [Microsoft.Win32.RegistryValueKind]::String)
            if ($target.Key.GetValue('') -ne $newDll -or $target.Key.GetValue('ThreadingModel') -ne 'Apartment') {
                throw "Registration verification failed: $($target.Name)"
            }
        }
    } catch {
        foreach ($target in $opened) {
            $target.Key.SetValue('', $target.PreviousDll, $target.PreviousDllKind)
            if ($null -eq $target.PreviousThreadingModel) { $target.Key.DeleteValue('ThreadingModel', $false) }
            else { $target.Key.SetValue('ThreadingModel', $target.PreviousThreadingModel, [Microsoft.Win32.RegistryValueKind]::String) }
        }
        throw
    }
    [pscustomobject]@{ Dll = $newDll; UpdatedRegistrations = $opened.Name -join ', '; Backup = $backup }
} finally {
    foreach ($target in $opened) { $target.Key.Dispose() }
}
