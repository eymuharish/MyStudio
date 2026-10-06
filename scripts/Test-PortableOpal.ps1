param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [int]$SipPort = 25142,
    [int]$H323Port = 21762,
    [int]$RtpPort = 53004
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $evidence) { throw 'Use a new evidence directory; prior tests are preserved.' }
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$relocated = Join-Path $evidence 'مسار عربي مع مسافات 漢字\MyStudio'
[IO.Directory]::CreateDirectory($relocated) | Out-Null
Get-ChildItem -LiteralPath $package | Copy-Item -Destination $relocated -Recurse
$reportFolder = Join-Path $evidence 'ui'
[IO.Directory]::CreateDirectory($reportFolder) | Out-Null
$arguments = @('--sip-port', "$SipPort", '--h323-port', "$H323Port", '--rtp-port', "$RtpPort",
    '--ui-smoke', ('"' + $reportFolder + '"'))
$environment = @{
    PATH = (Join-Path $env:SystemRoot 'System32')
    DOTNET_ROOT = (Join-Path $evidence 'no-installed-dotnet')
    DOTNET_ROOT_X64 = (Join-Path $evidence 'no-installed-dotnet')
    DOTNET_MULTILEVEL_LOOKUP = '0'
    HTTP_PROXY = 'http://127.0.0.1:1'; HTTPS_PROXY = 'http://127.0.0.1:1'
}
$process = Start-Process -FilePath (Join-Path $relocated 'MyStudio.exe') -ArgumentList $arguments -Environment $environment -WorkingDirectory $env:SystemRoot -WindowStyle Hidden -PassThru
$modules = @{}
$deadline = [DateTimeOffset]::UtcNow.AddSeconds(60)
while (!$process.HasExited -and [DateTimeOffset]::UtcNow -lt $deadline) {
    foreach ($candidate in @(Get-Process -Name MyStudio,opal_console -ErrorAction SilentlyContinue)) {
        try {
            if (!$candidate.Path -or !$candidate.Path.StartsWith($relocated + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
            foreach ($module in $candidate.Modules) {
                if ($module.ModuleName -match '^(coreclr|hostfxr|hostpolicy|Microsoft\.UI\.Xaml|msvcp\d|vcruntime\d|vccorlib\d|concrt\d)') {
                    $modules[$module.FileName] = [ordered]@{ process=$candidate.ProcessName; name=$module.ModuleName; path=$module.FileName }
                }
            }
        } catch { continue } # A sampled process can exit between module queries.
    }
    Start-Sleep -Milliseconds 100
    $process.Refresh()
}
if (!$process.HasExited) { throw 'Portable UI fixture did not finish within 60 seconds; evidence is retained.' }
$process.WaitForExit()
if ($process.ExitCode) { throw "Portable launcher returned exit code $($process.ExitCode)" }
$ui = Get-Content -LiteralPath (Join-Path $reportFolder 'ui-smoke.json') -Raw | ConvertFrom-Json
if (!$ui.engineReady -or !$ui.captureAffinityVerified -or $ui.snapshots.Count -ne 18 -or $ui.studioPolicyChecks -ne 13) { throw 'Portable UI/engine checks failed.' }
if (!(Test-Path -LiteralPath (Join-Path $relocated 'appfiles\OPALData') -PathType Container)) { throw 'Default portable data root missing.' }
foreach ($name in @('coreclr.dll','Microsoft.UI.Xaml.dll')) {
    if (!(@($modules.Values).name -contains $name)) { throw "Did not observe local runtime module: $name" }
}
foreach ($module in $modules.Values) {
    if (!$module.path.StartsWith($relocated + '\appfiles\', [StringComparison]::OrdinalIgnoreCase)) { throw "External runtime loaded: $($module.path)" }
}
$names=@(Get-ChildItem -LiteralPath $relocated).Name
if ($names.Count -ne 2 -or 'appfiles' -notin $names -or 'MyStudio.exe' -notin $names) { throw 'Unexpected portable root files after startup.' }
& python (Join-Path $PSScriptRoot 'Test-PortableWorker.py') --worker (Join-Path $relocated 'appfiles\Engines\Opal\NativeEngines\opal_console.exe') --output (Join-Path $evidence 'worker-codecs.json') --sip-port $SipPort --h323-port $H323Port --rtp-port $RtpPort
if ($LASTEXITCODE) { throw 'Relocated native codec/code-page check failed.' }
$barFolder = Join-Path $evidence 'call-bar'
$barData = Join-Path $evidence 'call-bar-data'
[IO.Directory]::CreateDirectory($barFolder) | Out-Null
$barArguments = @('--sip-port', "$SipPort", '--h323-port', "$H323Port", '--rtp-port', "$RtpPort",
    '--data-root', ('"' + $barData + '"'), '--call-bar-smoke', ('"' + $barFolder + '"'))
$barProcess = Start-Process -FilePath (Join-Path $relocated 'MyStudio.exe') -ArgumentList $barArguments -Environment $environment -WorkingDirectory $env:SystemRoot -WindowStyle Hidden -PassThru
if (!$barProcess.WaitForExit(60000)) { throw 'Portable call-bar fixture did not finish; evidence is retained.' }
if ($barProcess.ExitCode) { throw "Portable call-bar fixture returned exit code $($barProcess.ExitCode)" }
$bar = Get-Content -LiteralPath (Join-Path $barFolder 'call-bar-smoke.json') -Raw | ConvertFrom-Json
if ($bar.result -ne 'PASS' -or !$bar.callHoldControlsAbsent -or !$bar.topmostStyle -or !$bar.survivesMainMinimize -or
    !$bar.duplicateAudioRequestsPrevented -or !$bar.recordingPauseDoesNotPauseCall -or $bar.realDeviceCalls -ne 0 -or
    $bar.logicalWidth -ne 504 -or $bar.logicalHeight -ne 48) { throw 'Portable call-bar controls/layout checks failed.' }
[ordered]@{
    result='PASS'; relocatedRoot=$relocated; unicodeAndSpacesPath=$true
    defaultDataRootIsInsideAppfiles=$true
    installedDotnetLookupDisabled=$true; pathContainsOnlyWindowsSystem32=$true
    downloadProxiesBlockedForThisProcess=$true; freshOperatingSystemTested=$false
    captureAffinityVerified=$ui.captureAffinityVerified; engineReady=$ui.engineReady; layouts=$ui.snapshots.Count; studioPolicyChecks=$ui.studioPolicyChecks
    callHoldControlsAbsent=$bar.callHoldControlsAbsent; compactCallBarVerified=$true
    observedRuntimeModules=@($modules.Values)
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'portable-startup.json') -Encoding utf8
Write-Output 'PORTABLE_ARABIC_PATH_AND_LOCAL_RUNTIMES=PASS'
