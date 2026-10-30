[CmdletBinding()]
param(
    [string]$DataDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\rime\full_pinyin'),
    [string]$SupplementPath
)
$ErrorActionPreference = 'Stop'
if (-not $SupplementPath) { $SupplementPath = Join-Path $DataDirectory 'zhixu_daily.dict.yaml' }
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$basePath = Join-Path $DataDirectory 'pinyin_simp.dict.yaml'
$baseLines = $utf8.GetString([IO.File]::ReadAllBytes($basePath)) -split '\r?\n'
$baseWords = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$syllables = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$baseRecords = 0
foreach ($line in $baseLines) {
    if ($line.StartsWith('#') -or -not $line.Contains("`t")) { continue }
    $fields = $line -split "`t"
    [void]$baseWords.Add($fields[0])
    foreach ($syllable in ($fields[1] -split ' ')) { [void]$syllables.Add($syllable) }
    ++$baseRecords
}
$lines = $utf8.GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $SupplementPath))) -split '\r?\n'
$entriesStarted = $false
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$supplementRecords = 0
$newWords = 0
$lineNumber = 0
foreach ($line in $lines) {
    ++$lineNumber
    if ($line -eq '...') { $entriesStarted = $true; continue }
    if (-not $entriesStarted -or $line.Length -eq 0 -or $line.StartsWith('#')) { continue }
    $fields = $line -split "`t"
    if ($fields.Count -ne 3 -or $fields[0] -notmatch '^[\p{IsCJKUnifiedIdeographs}]+$' -or
        $fields[1] -notmatch '^[a-z]+( [a-z]+)*$' -or $fields[2] -notmatch '^[1-9][0-9]*$') {
        throw "Invalid supplement columns, text, Pinyin or positive weight at line $lineNumber"
    }
    $reading = $fields[1] -split ' '
    if ($reading.Count -ne $fields[0].Length) { throw "Character/Pinyin count mismatch at line $lineNumber" }
    foreach ($syllable in $reading) {
        if (-not $syllables.Contains($syllable)) { throw "Unknown Pinyin syllable at line $lineNumber" }
    }
    if (-not $seen.Add($fields[0] + "`t" + $fields[1])) { throw "Duplicate supplement entry at line $lineNumber" }
    ++$supplementRecords
    if (-not $baseWords.Contains($fields[0])) { ++$newWords }
}
if (-not $entriesStarted -or $supplementRecords -eq 0) { throw 'Supplement contains no dictionary records' }
[pscustomobject]@{
    BaseRecords = $baseRecords
    SupplementRecords = $supplementRecords
    NewTerms = $newWords
    ExistingTermRankingHints = $supplementRecords - $newWords
    Validation = 'PASS'
}
