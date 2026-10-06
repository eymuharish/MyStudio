param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$BuildSdk,
    [string]$RuntimeDirectory,
    [string]$CMake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [string]$EditionRoot = (Join-Path $PSScriptRoot '..\..')
)
$ErrorActionPreference = 'Stop'
$edition = [IO.Path]::GetFullPath($EditionRoot)
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($BuildSdk) { & (Join-Path $PSScriptRoot 'Build-OpalDependencies.ps1') -EditionRoot $edition }
& $CMake -S (Join-Path $source 'native\opal') -B (Join-Path $edition 'build\native-opal') -G 'Visual Studio 17 2022' -A x64 `
    "-DOPAL_SOURCE_DIR=$edition/dependencies/opal" "-DPTLIB_SOURCE_DIR=$edition/dependencies/ptlib" `
    "-DOPAL_LIBRARY=$edition/dependencies/opal/lib/opal64.lib" "-DPTLIB_LIBRARY=$edition/dependencies/ptlib/lib/ptlib64.lib" `
    "-DTELESTUDIO_VIDEO_CODECS_DIR=$edition/dependencies/media/video-codecs" "-DTELESTUDIO_AUDIO_DSP_INSTALL=$edition/dependencies/media/audio-dsp" `
    "-DLIBYUV_INCLUDE_DIR=$edition/dependencies/media/libyuv/include" "-DLIBYUV_LIBRARY=$edition/dependencies/media/libyuv/lib/libyuv.lib" `
    "-DTELESTUDIO_AAC_LD_INSTALL=$edition/dependencies/media/aac-ld"
if ($LASTEXITCODE) { throw 'OPAL native configure failed.' }
& $CMake --build (Join-Path $edition 'build\native-opal') --config Release --parallel 4
if ($LASTEXITCODE) { throw 'OPAL native build failed.' }
# A fresh publish directory prevents retired assemblies surviving a rebuild.
$managedBuildId = [Guid]::NewGuid().ToString('N')
$managedOutput = Join-Path $edition ('build\managed\publish-' + $managedBuildId)
$managedArtifacts = Join-Path $edition ('build\managed\artifacts-' + $managedBuildId)
& dotnet publish (Join-Path $source 'MyStudio.WinUI\MyStudio.WinUI.csproj') -c Release -r win-x64 --self-contained true -p:Platform=x64 --artifacts-path $managedArtifacts -o $managedOutput --verbosity quiet
if ($LASTEXITCODE) { throw 'OPAL managed publish failed.' }
& (Join-Path $PSScriptRoot 'Update-OpalProvenance.ps1') -EditionRoot $edition
& (Join-Path $PSScriptRoot 'Package-OpalPortable.ps1') -OutputDirectory $OutputDirectory -EditionRoot $edition -RuntimeDirectory $RuntimeDirectory -ManagedPublish $managedOutput
# A package is a build output. Run native/core, loopback, UI and physical tests
# before describing it as validated; record their evidence in VALIDATION.md.
