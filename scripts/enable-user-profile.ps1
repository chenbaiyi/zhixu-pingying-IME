[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$tip = '0804:{60F059D2-C31E-408C-BF6D-8CE1B46EA420}{AEFCD1B0-B3A6-45C4-AAD0-BCC298AD01F8}'
$languages = Get-WinUserLanguageList
$before = @($(foreach ($language in $languages) {
    [pscustomobject]@{ LanguageTag = $language.LanguageTag; InputMethodTips = @($language.InputMethodTips) }
}))
if (@($languages | ForEach-Object { $_.InputMethodTips }) -contains $tip) {
    'USER_PROFILE_ALREADY_ENABLED=True'
    return
}
$backup = Join-Path $env:LOCALAPPDATA ('ZhixuIME\runtime\user-profile-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
$before | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'languages-before.json') -Encoding utf8
if (-not ('ZhixuUserProfileInstallation' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ZhixuUserProfileInstallation {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet=CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public delegate bool InstallLayoutOrTip([MarshalAs(UnmanagedType.LPWStr)] string tip, uint flags);
}
'@
}
$module = [ZhixuUserProfileInstallation]::LoadLibraryExW((Join-Path $env:SystemRoot 'System32\input.dll'), [IntPtr]::Zero, 0x800)
if ($module -eq [IntPtr]::Zero) { throw 'Unable to load system input.dll' }
try {
    $entry = [ZhixuUserProfileInstallation]::GetProcAddress($module, 'InstallLayoutOrTip')
    if ($entry -eq [IntPtr]::Zero) { throw 'System InstallLayoutOrTip export missing' }
    $install = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($entry, [ZhixuUserProfileInstallation+InstallLayoutOrTip])
    # Enable only Zhixu. Do not set a default or replace other input methods.
    if (-not $install.Invoke($tip, 0)) { throw 'InstallLayoutOrTip failed' }
} finally { [void][ZhixuUserProfileInstallation]::FreeLibrary($module) }
"USER_PROFILE_INSTALL=True"
"BACKUP=$backup"
$after = Get-WinUserLanguageList
@($(foreach ($language in $after) {
    [pscustomobject]@{ LanguageTag = $language.LanguageTag; InputMethodTips = @($language.InputMethodTips) }
})) | ConvertTo-Json -Depth 4
