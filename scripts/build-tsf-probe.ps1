[CmdletBinding()]
param([Parameter(Mandatory)][ValidatePattern('^ImeTsf-[a-z0-9-]+$')][string]$OutputName,
      [ValidateSet('Tsf', 'InputHostTests', 'InputHost', 'CommonTests', 'All', 'AllTests')][string]$Component = 'Tsf',
      [ValidatePattern('^InputHost-[a-z0-9-]+$')][string]$InputHostOutputName,
      [string]$TestFilter = '*',
      [switch]$UseInstalledDependencies)

# Development-only build helper. Never changes COM registration or processes.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$toolchain = & (Join-Path $PSScriptRoot 'bootstrap.ps1') -AsObject
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $digest = $hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($root))
    $cacheId = ([BitConverter]::ToString($digest)).Replace('-', '').Substring(0, 16)
} finally { $hasher.Dispose() }
$cacheRoot = Join-Path ([IO.Path]::GetTempPath()) "ZhixuIME-native\$cacheId"
$build = Join-Path $cacheRoot 'build\x64-debug-librime'
$sourceRoot = Join-Path ([IO.Path]::GetTempPath()) "ZhixuIME-source-$cacheId"
if (-not (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) {
    throw "Existing librime development build not found: $build"
}
$sourceItem = Get-Item -LiteralPath $sourceRoot
if ($sourceItem.LinkType -ne 'Junction' -or $sourceItem.Target -notcontains $root) {
    throw "ASCII source junction does not point to this worktree: $sourceRoot"
}
$alternateCMake = Join-Path $cacheRoot 'downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $alternateCMake)) { throw 'Cached CMake 3.31.10 not found' }
$gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
$gitDirectory = if ($gitCommand) { Split-Path $gitCommand.Source -Parent } else { 'D:\Program Files\Git\cmd' }
$powershellDirectory = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell'
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$gitDirectory;$powershellDirectory"
Remove-Item Env:INCLUDE,Env:EXTERNAL_INCLUDE,Env:LIB,Env:LIBPATH,Env:VSCMD_VER,Env:__VSCMD_PREINIT_PATH -ErrorAction SilentlyContinue
$env:ZHIXU_VCVARS = $toolchain.VcVars
$environmentLines = & $env:ComSpec /d /c 'call "%ZHIXU_VCVARS%" >nul && set'
if ($LASTEXITCODE -ne 0) { throw "vcvars64 failed ($LASTEXITCODE)" }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$env:VCPKG_DOWNLOADS = Join-Path $cacheRoot 'downloads'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $cacheRoot 'binary'
$env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
Push-Location $build
try {
    if ($Component -eq 'Tsf' -or $InputHostOutputName -or $UseInstalledDependencies) {
        $configureArgs = @('-S', $sourceRoot, '-B', $build,
            "-DCMAKE_CXX_COMPILER=$($toolchain.Compiler)", "-DCMAKE_MAKE_PROGRAM=$($toolchain.Ninja)",
            "-DZHIXU_TSF_OUTPUT_NAME=$OutputName")
        if ($InputHostOutputName) { $configureArgs += "-DZHIXU_INPUT_HOST_OUTPUT_NAME=$InputHostOutputName" }
        if ($UseInstalledDependencies) { $configureArgs += '-DVCPKG_MANIFEST_INSTALL=OFF' }
        & $alternateCMake @configureArgs
        if ($LASTEXITCODE -ne 0) { throw "CMAKE_EXIT=$LASTEXITCODE" }
    }
    $buildTargets = @(if ($Component -eq 'All') { 'all' } elseif ($Component -eq 'AllTests') { 'zhixu_common_tests'; 'zhixu_input_host_tests'; 'zhixu_broker_tests'; 'zhixu_tsf_tests'; 'zhixu_predict_tests'; 'InputHost'; 'ImeTsf' } elseif ($Component -eq 'Tsf') { 'zhixu_tsf_tests' } elseif ($Component -eq 'CommonTests') { 'zhixu_common_tests' } elseif ($Component -eq 'InputHostTests') { 'zhixu_input_host_tests' } else { 'InputHost'; 'zhixu_input_host_tests' })
    & $toolchain.Ninja -C $build -j 4 @buildTargets
    if ($LASTEXITCODE -ne 0) { throw "BUILD_EXIT=$LASTEXITCODE" }
    $testName = if ($Component -eq 'Tsf') { 'zhixu_tsf_tests.exe' } elseif ($Component -eq 'CommonTests') { 'zhixu_common_tests.exe' } else { 'zhixu_input_host_tests.exe' }
    $testLog = Join-Path $cacheRoot ("validation-$Component-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '.log')
    if ($Component -in @('All','AllTests')) {
        & $toolchain.CTest --test-dir $build --output-on-failure --no-tests=error | Tee-Object -FilePath $testLog
    } else {
        & (Join-Path $build $testName) "--gtest_filter=$TestFilter" | Tee-Object -FilePath $testLog
    }
    $testExit = $LASTEXITCODE
    "TEST_EXIT=$testExit"
    "TEST_LOG=$testLog"
    if ($testExit -ne 0) { throw 'Tests failed; do not deploy this build' }
    if ($Component -eq 'Tsf') { "PROBE_DLL=$(Join-Path $build "$OutputName.dll")" }
    if ($Component -eq 'InputHost' -and $InputHostOutputName) { "PROBE_INPUT_HOST=$(Join-Path $build "$InputHostOutputName.exe")" }
} finally { Pop-Location }
