[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [switch]$Clean,
      [string]$Target)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$toolchain = & (Join-Path $PSScriptRoot 'bootstrap.ps1') -AsObject
# Import the toolchain environment; no machine-wide PATH or installation changes.
$environmentLines = & $env:ComSpec /d /c "call `"$($toolchain.VcVars)`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$env:VCPKG_ROOT = Split-Path $toolchain.Vcpkg -Parent
$cacheId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($root))).Substring(0,16)
$cacheRoot = Join-Path ([IO.Path]::GetTempPath()) "ZhixuIME-native\$cacheId"
if ($cacheRoot -match '[^\x00-\x7F]') { throw 'Native dependency cache requires an ASCII TEMP path (vcpkg tool extraction limitation)' }
$env:VCPKG_DOWNLOADS = Join-Path $cacheRoot 'downloads'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $cacheRoot 'binary'
$env:ZHIXU_BUILD_ROOT = Join-Path $cacheRoot 'build'
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS,$env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
$sourceRoot = $root
if ($root -match '[^\x00-\x7F]') {
    $sourceRoot = Join-Path ([IO.Path]::GetTempPath()) "ZhixuIME-source-$cacheId"
    if (-not (Test-Path -LiteralPath $sourceRoot)) {
        New-Item -ItemType Junction -Path $sourceRoot -Target $root | Out-Null
    }
    $sourceItem = Get-Item -LiteralPath $sourceRoot
    if ($sourceItem.LinkType -ne 'Junction' -or $sourceItem.Target -notcontains $root) {
        throw "ASCII source junction does not point to this worktree: $sourceRoot"
    }
}
$preset = 'x64-' + $Configuration.ToLowerInvariant()
New-Item -ItemType Directory -Force -Path (Join-Path $env:ZHIXU_BUILD_ROOT $preset) | Out-Null
Push-Location $sourceRoot
try {
    # Pin cl.exe explicitly. CMake 4.2's compiler discovery crashes while
    # traversing a Unicode source path on this host, although MSVC itself is valid.
    & $toolchain.CMake --preset $preset "-DCMAKE_MAKE_PROGRAM=$($toolchain.Ninja)" "-DCMAKE_CXX_COMPILER=$($toolchain.Compiler)" "-DVCPKG_INSTALLED_DIR=$cacheRoot/installed"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
$buildArgs = @('--build','--preset',$preset,'--parallel','4')
if ($Target) { $buildArgs += '--target',$Target }
if ($Clean) { $buildArgs += '--clean-first' }
    & $toolchain.CMake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
