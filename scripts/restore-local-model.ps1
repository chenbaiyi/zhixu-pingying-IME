[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root '.local-assets/models/wanxiang-lts-zh-hans.gram'
$target = Join-Path $root 'assets/rime/full_pinyin/wanxiang-lts-zh-hans.gram'
$metadata = Get-Content -LiteralPath (Join-Path $root 'assets/rime/full_pinyin/GRAMMAR-SOURCES.json') -Raw | ConvertFrom-Json
$expected = $metadata.model.sha256 -replace '^sha256:', ''
if (Test-Path -LiteralPath $target) {
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected -or (Get-Item -LiteralPath $target).Length -ne $metadata.model.bytes) { throw 'Existing model differs; preserve it and inspect manually.' }
    Write-Output 'Pinned context model already present and verified.'
    return
}
if (-not (Test-Path -LiteralPath $source)) { throw 'Local model absent. Use python scripts/fetch-context-model.py --fetch' }
if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $expected -or (Get-Item -LiteralPath $source).Length -ne $metadata.model.bytes) { throw 'Local model validation failed.' }
Copy-Item -LiteralPath $source -Destination $target -ErrorAction Stop
Write-Output 'Pinned context model restored and verified.'
