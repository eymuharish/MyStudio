param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$EditionRoot = (Join-Path $PSScriptRoot '..\..'),
    [string]$NativeBuild = 'build\native-opal\Release',
    [string]$ManagedPublish = 'build\managed\publish',
    [string]$RuntimeDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$edition = [IO.Path]::GetFullPath($EditionRoot)
$package = if ([IO.Path]::IsPathRooted($OutputDirectory)) { [IO.Path]::GetFullPath($OutputDirectory) } else { [IO.Path]::GetFullPath((Join-Path $edition $OutputDirectory)) }
if (Test-Path -LiteralPath $package) { throw 'Use a new output directory; existing packages are preserved.' }
$native = Join-Path $edition $NativeBuild
$managed = Join-Path $edition $ManagedPublish
$payload = Join-Path $package 'appfiles'
$launcher = Join-Path $native 'MyStudioPortableLauncher.exe'
if (!(Test-Path -LiteralPath $launcher -PathType Leaf)) { throw 'Missing statically linked portable launcher.' }
foreach ($name in @('coreclr.dll','hostfxr.dll','hostpolicy.dll','Microsoft.UI.Xaml.dll','Microsoft.WindowsAppRuntime.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $managed $name) -PathType Leaf)) { throw "Missing self-contained runtime: $name" }
}
$runtimeConfig = Get-Content -LiteralPath (Join-Path $managed 'MyStudio.runtimeconfig.json') -Raw | ConvertFrom-Json
if (!$runtimeConfig.runtimeOptions.includedFrameworks -or $runtimeConfig.runtimeOptions.PSObject.Properties.Name -contains 'frameworks' -or $runtimeConfig.runtimeOptions.PSObject.Properties.Name -contains 'framework') {
    throw 'Publish must include .NET frameworks locally, without an installed-runtime requirement.'
}
if (!$RuntimeDirectory) {
    throw 'Supply the x64 Microsoft Visual C++ runtime directory with -RuntimeDirectory.'
}
$required = @(
    (Join-Path $managed 'MyStudio.exe'),
    (Join-Path $managed 'MyStudio.pri'),
    (Join-Path $managed 'App.xbf'),
    (Join-Path $native 'opal_console.exe'),
    (Join-Path $native 'EssaStudio.CaptureCatalog.dll'),
    (Join-Path $edition 'dependencies\opal\lib\opal64.dll'),
    (Join-Path $edition 'dependencies\ptlib\lib\ptlib64.dll'),
    (Join-Path $native '..\aac\Release\mystudio_opal_aac.dll')
)
foreach ($file in $required) { if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing build artifact: $file" } }
$unexpectedManaged = @(Get-ChildItem -LiteralPath $managed -Filter '*.exe' -File -Recurse | Where-Object {
    [IO.Path]::GetRelativePath($managed, $_.FullName).Replace('\','/') -notin @('MyStudio.exe','createdump.exe','RestartAgent.exe')
})
if ($unexpectedManaged.Count) { throw ('Unexpected executable in managed publish: ' + ($unexpectedManaged.Name -join ', ')) }
$restartAgent = Join-Path $managed 'RestartAgent.exe'
if (Test-Path -LiteralPath $restartAgent) {
    $signature = Get-AuthenticodeSignature -LiteralPath $restartAgent
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:\s*,|$)') {
        throw 'Windows App SDK RestartAgent must have a valid Microsoft signature.'
    }
}
$runtimeRequired = @()
foreach ($name in @('avcodec-63.dll','avutil-61.dll','swscale-10.dll','libx264-165.dll','libx265.dll')) { $runtimeRequired += Join-Path $edition ('dependencies\media\video-codecs\bin\'+$name) }
foreach ($name in @('libcrypto-3-x64.dll','libssl-3-x64.dll')) { $runtimeRequired += Join-Path $edition ('dependencies\openssl\bin\'+$name) }
foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','vccorlib140.dll')) { $runtimeRequired += Join-Path $RuntimeDirectory $name }
foreach ($file in $runtimeRequired) { if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing critical runtime: $file" } }
$g722 = Join-Path $edition 'dependencies\opal\bin\plugins\x64\Release\G722_ptplugin.dll'
if (!(Test-Path -LiteralPath $g722 -PathType Leaf)) { throw 'Missing Release G.722 plugin.' }
New-Item -ItemType Directory -Path $package | Out-Null
New-Item -ItemType Directory -Path $payload | Out-Null
Copy-Item -LiteralPath $launcher -Destination (Join-Path $package 'MyStudio.exe')
Get-ChildItem -LiteralPath $managed | Where-Object Name -notin @('OPALData','EssaStudio.exe','EssaStudio.pdb') | Copy-Item -Destination $payload -Recurse
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File | Copy-Item -Destination $payload
$engine = Join-Path $payload 'Engines\Opal\NativeEngines'
$plugins = Join-Path $engine 'plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
foreach ($file in @($required[3],$required[4],$required[5],$required[6])) { Copy-Item -LiteralPath $file -Destination $engine }
Copy-Item -LiteralPath $required[7] -Destination $plugins
Copy-Item -LiteralPath $g722 -Destination (Join-Path $plugins 'g722_audio.dll')
Get-ChildItem -LiteralPath (Join-Path $edition 'dependencies\media\video-codecs\bin') -Filter '*.dll' -File |
    Where-Object Name -match '^(avcodec|avutil|swscale|libx264|libx265)' | Copy-Item -Destination $engine
Get-ChildItem -LiteralPath (Join-Path $edition 'dependencies\openssl\bin') -Filter '*.dll' -File | Copy-Item -Destination $engine
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File |
    Copy-Item -Destination $engine
$licenses = Join-Path $payload 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\ptlib\mpl-1.0.htm') -Destination (Join-Path $licenses 'PTLib-OPAL-MPL-1.0.htm')
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\openssl-source\LICENSE.txt') -Destination (Join-Path $licenses 'OpenSSL-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\media\aac-ld\licenses\fdk-aac-NOTICE') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\media\video-codecs\licenses') -Destination (Join-Path $licenses 'video-codecs') -Recurse
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\media\audio-dsp\licenses') -Destination (Join-Path $licenses 'audio-dsp') -Recurse
Copy-Item -LiteralPath (Join-Path $edition 'dependencies\media\libyuv\licenses') -Destination (Join-Path $licenses 'libyuv') -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\docs\opal-dependencies.json') -Destination (Join-Path $payload 'DEPENDENCIES.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\docs\opal-portable-readme.md') -Destination (Join-Path $payload 'README.md')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\docs\opal-validation.md') -Destination (Join-Path $payload 'VALIDATION.md')
$source = Join-Path $edition 'source'
$revision = & git -C $source rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot identify packaged application source.' }
$changes = & git -C $source status --porcelain
if ($LASTEXITCODE) { throw 'Cannot verify packaged source state.' }
[ordered]@{
    product = 'MyStudio'
    version = '2.2.0'
    applicationCommit = $revision
    sourceClean = ![bool]$changes
    builtArtifacts = @(foreach ($file in @('MyStudio.exe','appfiles\MyStudio.dll','appfiles\EssaStudio.dll','appfiles\Engines\Opal\NativeEngines\opal_console.exe','appfiles\Engines\Opal\NativeEngines\opal64.dll','appfiles\Engines\Opal\NativeEngines\plugins\mystudio_opal_aac.dll')) {
        [ordered]@{ path=$file.Replace('\','/'); sha256=(Get-FileHash -LiteralPath (Join-Path $package $file)).Hash.ToLowerInvariant() }
    })
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $payload 'BUILD-IDENTITY.json') -Encoding utf8
if (Get-ChildItem -LiteralPath $package -Filter '*.exe' -File -Recurse | Where-Object {
    [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\','/') -notin @('MyStudio.exe','appfiles/MyStudio.exe','appfiles/createdump.exe','appfiles/RestartAgent.exe','appfiles/Engines/Opal/NativeEngines/opal_console.exe')
}) { throw 'Unexpected executable in OPAL package.' }
& python (Join-Path $PSScriptRoot 'Verify-NativeDependencies.py') $package --portable --output (Join-Path $payload 'RUNTIME-CHECKS.json')
if ($LASTEXITCODE) { throw 'Portable runtime closure failed.' }
$manifest = Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=[IO.Path]::GetRelativePath($package,$_.FullName).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $payload 'FILES.json') -Encoding utf8
Write-Output $package
