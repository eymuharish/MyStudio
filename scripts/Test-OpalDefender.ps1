param(
    [Parameter(Mandatory)][string]$ScanPath,
    [string]$ReportFile
)
$ErrorActionPreference = 'Stop'
$scan = (Resolve-Path -LiteralPath $ScanPath).Path
$platform = Get-ChildItem -LiteralPath "$env:ProgramData\Microsoft\Windows Defender\Platform" -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$scanner = if ($platform) { Join-Path $platform.FullName 'MpCmdRun.exe' } else { "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" }
if (!(Test-Path -LiteralPath $scanner -PathType Leaf)) { throw 'Microsoft Defender scanner is unavailable; no clean verdict can be issued.' }
$started = [DateTimeOffset]::UtcNow
$fileHashBefore = if (Test-Path -LiteralPath $scan -PathType Leaf) { (Get-FileHash -LiteralPath $scan -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
# This scan-only option ignores exclusions and scans archives. It does not change
# Defender settings or restore files. A detected file must not be released.
$output = @(& $scanner -Scan -ScanType 3 -File $scan -DisableRemediation 2>&1) | ForEach-Object { $_.ToString() }
$exitCode = $LASTEXITCODE
$status = Get-MpComputerStatus
$fileHashAfter = if ($fileHashBefore) { (Get-FileHash -LiteralPath $scan -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
$unchanged = !$fileHashBefore -or $fileHashBefore -eq $fileHashAfter
$clean = $exitCode -eq 0 -and ($output -join "`n") -match 'found no threats' -and $unchanged
$report = [ordered]@{
    result = if ($clean) { 'NO_THREATS_DETECTED' } else { 'NOT_VERIFIED' }
    scanPath = $scan; startedUtc = $started.ToString('O'); completedUtc = [DateTimeOffset]::UtcNow.ToString('O')
    exitCode = $exitCode; exclusionsIgnored = $true; remediationDisabledForThisScan = $true
    signatureVersion = $status.AntivirusSignatureVersion
    signatureUpdated = $status.AntivirusSignatureLastUpdated.ToString('O')
    realTimeProtectionEnabled = $status.RealTimeProtectionEnabled
    sha256 = $fileHashAfter; sha256Before = $fileHashBefore; fileUnchangedDuringScan = $unchanged
    output = $output
}
if ($ReportFile) {
    $reportPath = [IO.Path]::GetFullPath($ReportFile)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($reportPath)) | Out-Null
    $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $reportPath -Encoding utf8
}
$output
if (!$clean) { throw 'Defender did not return an explicit no-threats result. Review the scan output before release.' }
