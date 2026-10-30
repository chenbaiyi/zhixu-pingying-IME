[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [string]$Filter = '')
$ErrorActionPreference = 'Stop'
$tools = & (Join-Path $PSScriptRoot 'bootstrap.ps1') -AsObject
$root = Split-Path $PSScriptRoot -Parent
$cacheId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($root))).Substring(0,16)
$env:ZHIXU_BUILD_ROOT = Join-Path ([IO.Path]::GetTempPath()) "ZhixuIME-native\$cacheId\build"
Push-Location $root
try {
    $arguments = @('--preset',('x64-' + $Configuration.ToLowerInvariant()),'--output-on-failure','--no-tests=error')
    if ($Filter) { $arguments += @('-R',$Filter) }
    & $tools.CTest @arguments
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
} finally { Pop-Location }
