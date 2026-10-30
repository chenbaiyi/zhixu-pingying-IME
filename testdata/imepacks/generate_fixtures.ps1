[CmdletBinding()]
param(
    [string]$OutputDirectory = $PSScriptRoot,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression.FileSystem

$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$packagePath = Join-Path $outputRoot 'acceptance.imepack'
if (Test-Path -LiteralPath $packagePath) {
    if (-not $Force) {
        throw "Refusing to overwrite existing fixture: $packagePath. Use -Force to replace it."
    }
    Remove-Item -LiteralPath $packagePath -Force
}

$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("zhixu-imepack-" + [guid]::NewGuid())
try {
    [System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    $entries = @(
        '{"id":"acceptance.phrase","type":"phrase","text":"道韵","pinyin":"daoyun","weight":1.0,"tags":["acceptance"]}',
        '{"id":"acceptance.daoyun","type":"continuation","trigger":"道韵","completions":[{"text":"法则","weight":1.0}],"tags":["acceptance"]}',
        '{"id":"acceptance.quotation","type":"quotation","segments":["道可道","非常道"],"work":"道德经","author":"老子","tags":["acceptance"]}'
    )
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $entriesPath = Join-Path $stagingRoot 'entries.jsonl'
    [System.IO.File]::WriteAllText($entriesPath, (($entries -join "`n") + "`n"), $utf8NoBom)
    $entriesHash = (Get-FileHash -LiteralPath $entriesPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        schema_version = '1.0'
        id = 'com.zhixu.acceptance'
        name = 'Zhixu acceptance package'
        version = '1.0.0'
        locale = 'zh-CN'
        content_types = @('phrase', 'continuation', 'quotation')
        source = 'local-test'
        license = 'MIT'
        generated_at = '2026-09-24T00:00:00Z'
        entries = [ordered]@{
            path = 'entries.jsonl'
            sha256 = $entriesHash
        }
    }
    [System.IO.File]::WriteAllText((Join-Path $stagingRoot 'manifest.json'),
        ($manifest | ConvertTo-Json -Compress), $utf8NoBom)
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stagingRoot, $packagePath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Write-Output $packagePath
}
finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
