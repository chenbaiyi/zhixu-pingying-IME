[CmdletBinding()]
param([switch]$AsObject)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2026 discovery tool not found' }
$install = & $vswhere -version '[18.0,19.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $install) { throw 'Visual Studio 2026 MSVC x64 toolset not found' }
$install = $install.Trim()
$toolVersion = (Get-Content -LiteralPath (Join-Path $install 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Windows 10/11 SDK headers not found' }
$resolved = [ordered]@{
    VisualStudio = $install
    VcVars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    Compiler = Join-Path $install "VC\Tools\MSVC\$toolVersion\bin\Hostx64\x64\cl.exe"
    MSBuild = Join-Path $install 'MSBuild\Current\Bin\MSBuild.exe'
    CMake = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    CTest = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
    Ninja = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    Vcpkg = Join-Path $install 'VC\vcpkg\vcpkg.exe'
    Sdk = $sdk.FullName
    SignTool = Join-Path $sdkRoot "bin\$($sdk.Name)\x64\signtool.exe"
    ResourceCompiler = Join-Path $sdkRoot "bin\$($sdk.Name)\x64\rc.exe"
    SdkLibrary = Join-Path $sdkRoot "Lib\$($sdk.Name)\um\x64\kernel32.lib"
}
foreach ($entry in $resolved.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value)) { throw "Required native tool missing: $($entry.Key): $($entry.Value)" }
}
if ($AsObject) { return [pscustomobject]$resolved }
$resolved.GetEnumerator() | ForEach-Object { '{0}: {1}' -f $_.Key, $_.Value }
& $resolved.CMake --version
if ($LASTEXITCODE -ne 0) { throw 'CMake failed' }
& $resolved.Vcpkg version
if ($LASTEXITCODE -ne 0) { throw 'vcpkg failed' }
Write-Output 'protoc: resolved from the pinned vcpkg host protobuf package during configure'
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { Write-Output 'Managed prerequisites: .NET 8 SDK / WiX 4 still required before Task 6' }
