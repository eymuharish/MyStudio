[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:Passed = 0
$script:Failed = 0
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$builder = Join-Path $projectRoot 'scripts\build-audio-dsp.ps1'
$verifier = Join-Path $projectRoot 'scripts\verify-audio-dsp.ps1'

function Assert-Equal {
    param($Actual, $Expected, [string]$Name)
    if ($Actual -ne $Expected) { throw "$Name expected '$Expected', got '$Actual'." }
}

function Assert-Sequence {
    param([object[]]$Actual, [object[]]$Expected, [string]$Name)
    Assert-Equal $Actual.Count $Expected.Count "$Name count"
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        Assert-Equal $Actual[$index] $Expected[$index] "$Name[$index]"
    }
}

function Assert-Fails {
    param([scriptblock]$Action, [string]$Pattern, [string]$Name)
    try { & $Action | Out-Null }
    catch {
        if ($_.Exception.Message -notlike $Pattern) {
            throw "$Name failed with the wrong message: $($_.Exception.Message)"
        }
        return
    }
    throw "$Name unexpectedly passed."
}

function Invoke-Test {
    param([string]$Name, [scriptblock]$Action)
    try {
        & $Action
        $script:Passed++
        Write-Output "PASS $Name"
    }
    catch {
        $script:Failed++
        Write-Error "FAIL $Name`: $($_.Exception.Message)" -ErrorAction Continue
    }
}

function Get-TestSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function New-AudioDspFixture {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("essastudio-audio-dsp-test-" + [Guid]::NewGuid().ToString('N'))
    $install = Join-Path $root 'work\build\audio-dsp\install'
    $source = Join-Path $root 'work\build\audio-dsp\src\speexdsp'
    $generated = Join-Path $root 'work\build\audio-dsp\generated'
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'third_party'), (Join-Path $root 'scripts'), (Join-Path $install 'include\speex'), (Join-Path $install 'lib'), (Join-Path $install 'licenses'), $source, $generated | Out-Null

    $sources = @(
        'libspeexdsp/preprocess.c', 'libspeexdsp/filterbank.c',
        'libspeexdsp/fftwrap.c', 'libspeexdsp/kiss_fft.c',
        'libspeexdsp/kiss_fftr.c', 'libspeexdsp/mdf.c',
        'libspeexdsp/resample.c'
    )
    foreach ($relative in $sources) {
        $path = Join-Path $source $relative.Replace('/', '\')
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [System.IO.File]::WriteAllText($path, "fixture $relative", [System.Text.UTF8Encoding]::new($false))
    }
    [System.IO.File]::WriteAllText((Join-Path $source 'COPYING'), 'BSD-3-Clause fixture', [System.Text.UTF8Encoding]::new($false))
    $config = @'
#define inline __inline
#if _M_IX86_FP >= 1 || defined(_M_X64)
#define USE_SSE
#endif
#if _M_IX86_FP >= 2 || defined(_M_X64)
#define USE_SSE2
#endif
#ifndef USE_SSE
#define USE_ALLOCA
#endif
#define FLOATING_POINT
#define USE_KISS_FFT
#define EXPORT
'@
    [System.IO.File]::WriteAllText((Join-Path $generated 'config.h'), $config, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $install 'include\speex\speex_preprocess.h'), 'fixture header', [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $install 'include\speex\speex_resampler.h'), 'fixture header', [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $install 'lib\speexdsp-private.lib'), 'fixture static archive', [System.Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath (Join-Path $source 'COPYING') -Destination (Join-Path $install 'licenses\speexdsp-COPYING')

    $manifest = [ordered]@{
        schemaVersion = 1
        speexdsp = [ordered]@{
            repository = 'https://github.com/xiph/speexdsp.git'
            tag = 'SpeexDSP-1.2.1'
            tagObject = 'e762196693c487249d7061081915c8eaf7511f4f'
            peeledCommit = '1b28a0f61bc31162979e1f26f3981fc3637095c8'
            license = 'BSD-3-Clause'
            licenseSource = 'COPYING'
            sources = $sources
        }
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'third_party\audio-dsp.json') -Encoding UTF8

    $metadata = [ordered]@{
        schemaVersion = 1
        repository = $manifest.speexdsp.repository
        tag = $manifest.speexdsp.tag
        tagObject = $manifest.speexdsp.tagObject
        peeledCommit = $manifest.speexdsp.peeledCommit
        sourceCheckout = [ordered]@{ relativePath = 'work/build/audio-dsp/src/speexdsp'; clean = $true; head = $manifest.speexdsp.peeledCommit; tagObject = $manifest.speexdsp.tagObject; peeledCommit = $manifest.speexdsp.peeledCommit }
        sources = @($sources | ForEach-Object { [ordered]@{ path = $_; sha256 = Get-TestSha256 (Join-Path $source $_.Replace('/', '\')) } })
        license = [ordered]@{
            sourcePath = 'COPYING'; sourceSha256 = Get-TestSha256 (Join-Path $source 'COPYING')
            stagedPath = 'licenses/speexdsp-COPYING'; stagedSha256 = Get-TestSha256 (Join-Path $install 'licenses\speexdsp-COPYING')
        }
        generatedConfig = [ordered]@{ path = 'work/build/audio-dsp/generated/config.h'; sha256 = Get-TestSha256 (Join-Path $generated 'config.h') }
        toolchain = [ordered]@{ msvcVersion = '19.50.35723.0'; cmakeVersion = '4.1.1-msvc1' }
        architecture = 'x64'; configuration = 'Release'; language = 'C'; runtime = '/MD'
        compileDefinitions = @('HAVE_CONFIG_H=1', 'WIN32', '_LIB', 'NDEBUG')
        forbiddenDefinitionsPresent = @()
        headers = @(
            [ordered]@{ path = 'include/speex/speex_preprocess.h'; sha256 = Get-TestSha256 (Join-Path $install 'include\speex\speex_preprocess.h') },
            [ordered]@{ path = 'include/speex/speex_resampler.h'; sha256 = Get-TestSha256 (Join-Path $install 'include\speex\speex_resampler.h') }
        )
        library = [ordered]@{ path = 'lib/speexdsp-private.lib'; sha256 = Get-TestSha256 (Join-Path $install 'lib\speexdsp-private.lib'); classification = 'static-archive' }
    }
    $metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $install 'audio-dsp-build.json') -Encoding UTF8
    return [pscustomobject]@{ Root = $root; Install = $install; Source = $source; Generated = $generated; Manifest = $manifest; Metadata = $metadata; Sources = $sources }
}

function Save-FixtureMetadata($Fixture) {
    $Fixture.Metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Fixture.Install 'audio-dsp-build.json') -Encoding UTF8
}

Invoke-Test 'PlanOnly prints exact immutable dependency and build policy' {
    if (-not (Test-Path -LiteralPath $builder)) { throw "Missing builder: $builder" }
    $plan = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $builder -PlanOnly 2>&1)
    if ($LASTEXITCODE -ne 0) { throw ($plan | Out-String) }
    foreach ($line in @(
        'SPEEXDSP_REPOSITORY=https://github.com/xiph/speexdsp.git',
        'SPEEXDSP_TAG=SpeexDSP-1.2.1',
        'SPEEXDSP_TAG_OBJECT=e762196693c487249d7061081915c8eaf7511f4f',
        'SPEEXDSP_PEELED_COMMIT=1b28a0f61bc31162979e1f26f3981fc3637095c8',
        'SPEEXDSP_ARCH=x64', 'SPEEXDSP_CONFIGURATION=Release',
        'SPEEXDSP_LANGUAGE=C', 'SPEEXDSP_RUNTIME=/MD',
        'SPEEXDSP_LINKAGE=static', 'SPEEXDSP_ARITHMETIC=FLOATING_POINT',
        'SPEEXDSP_FFT=USE_KISS_FFT'
    )) { if ($plan -notcontains $line) { throw "Plan is missing: $line" } }
    foreach ($sourcePath in @('preprocess.c','filterbank.c','fftwrap.c','kiss_fft.c','kiss_fftr.c','mdf.c','resample.c')) {
        if (-not ($plan -match [regex]::Escape($sourcePath))) { throw "Plan is missing source $sourcePath" }
    }
}

Invoke-Test 'Verifier rejects every trust and static-link fixture mutation' {
    if (-not (Test-Path -LiteralPath $verifier)) { throw "Missing verifier: $verifier" }
    . $verifier

    $cases = @(
        @{ Name='wrong tag object'; Pattern='*tag object*'; Mutate={ param($f) $f.Metadata.tagObject='0'*40 } },
        @{ Name='wrong peeled commit'; Pattern='*peeled commit*'; Mutate={ param($f) $f.Metadata.peeledCommit='1'*40 } },
        @{ Name='dirty source'; Pattern='*dirty*'; Mutate={ param($f) $f.Metadata.sourceCheckout.clean=$false } },
        @{ Name='missing license'; Pattern='*license*missing*'; Mutate={ param($f) Remove-Item -LiteralPath (Join-Path $f.Install 'licenses\speexdsp-COPYING') } },
        @{ Name='changed license'; Pattern='*license*SHA-256*'; Mutate={ param($f) Add-Content -LiteralPath (Join-Path $f.Install 'licenses\speexdsp-COPYING') -Value 'changed' } },
        @{ Name='missing source'; Pattern='*source list*'; Mutate={ param($f) $f.Metadata.sources=@($f.Metadata.sources | Select-Object -Skip 1) } },
        @{ Name='extra source'; Pattern='*source list*'; Mutate={ param($f) $f.Metadata.sources += [ordered]@{path='libspeexdsp/buffer.c';sha256='0'*64} } },
        @{ Name='wrong source hash'; Pattern='*source*SHA-256*'; Mutate={ param($f) $f.Metadata.sources[0].sha256='0'*64 } },
        @{ Name='wrong config hash'; Pattern='*config*SHA-256*'; Mutate={ param($f) $f.Metadata.generatedConfig.sha256='0'*64 } },
        @{ Name='missing required config define'; Pattern='*missing required definition*'; Mutate={ param($f) (Get-Content -LiteralPath (Join-Path $f.Generated 'config.h') -Raw).Replace('#define USE_KISS_FFT','') | Set-Content -LiteralPath (Join-Path $f.Generated 'config.h') -Encoding UTF8; $f.Metadata.generatedConfig.sha256=Get-TestSha256 (Join-Path $f.Generated 'config.h') } },
        @{ Name='forbidden config define'; Pattern='*forbidden definition*'; Mutate={ param($f) Add-Content -LiteralPath (Join-Path $f.Generated 'config.h') -Value "`r`n#define FIXED_POINT"; $f.Metadata.generatedConfig.sha256=Get-TestSha256 (Join-Path $f.Generated 'config.h') } },
        @{ Name='forbidden define'; Pattern='*forbidden definition*'; Mutate={ param($f) $f.Metadata.forbiddenDefinitionsPresent=@('FIXED_POINT') } },
        @{ Name='wrong architecture'; Pattern='*architecture*'; Mutate={ param($f) $f.Metadata.architecture='Win32' } },
        @{ Name='wrong configuration'; Pattern='*configuration*'; Mutate={ param($f) $f.Metadata.configuration='Debug' } },
        @{ Name='wrong language'; Pattern='*language*'; Mutate={ param($f) $f.Metadata.language='CXX' } },
        @{ Name='wrong runtime'; Pattern='*runtime*'; Mutate={ param($f) $f.Metadata.runtime='/MT' } },
        @{ Name='missing MSVC version'; Pattern='*MSVC version*'; Mutate={ param($f) $f.Metadata.toolchain.msvcVersion='' } },
        @{ Name='missing CMake version'; Pattern='*CMake version*'; Mutate={ param($f) $f.Metadata.toolchain.cmakeVersion='' } },
        @{ Name='missing definition'; Pattern='*compile definitions*'; Mutate={ param($f) $f.Metadata.compileDefinitions=@('WIN32','_LIB','NDEBUG') } },
        @{ Name='wrong library hash'; Pattern='*library*SHA-256*'; Mutate={ param($f) $f.Metadata.library.sha256='0'*64 } },
        @{ Name='import library'; Pattern='*static archive*'; Mutate={ param($f) $f.Metadata.library.classification='import-library' } },
        @{ Name='missing staged header'; Pattern='*header*'; Mutate={ param($f) Remove-Item -LiteralPath (Join-Path $f.Install 'include\speex\speex_preprocess.h') } },
        @{ Name='changed staged header'; Pattern='*header*SHA-256*'; Mutate={ param($f) Add-Content -LiteralPath (Join-Path $f.Install 'include\speex\speex_preprocess.h') -Value 'changed' } },
        @{ Name='Speex DLL'; Pattern='*Speex DLL*'; Mutate={ param($f) [System.IO.File]::WriteAllText((Join-Path $f.Install 'speexdsp.dll'),'fixture') } }
    )
    foreach ($case in $cases) {
        $fixture = New-AudioDspFixture
        try {
            & $case.Mutate $fixture
            Save-FixtureMetadata $fixture
            Assert-Fails { Test-AudioDspArtifacts -ProjectRoot $fixture.Root -SkipSourceGitCheck } $case.Pattern $case.Name
        }
        finally { Remove-Item -LiteralPath $fixture.Root -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

Invoke-Test 'Verifier rejects a Speex DLL reported by consumer dependency inspection' {
    if (-not (Test-Path -LiteralPath $verifier)) { throw "Missing verifier: $verifier" }
    . $verifier
    Assert-Fails { Assert-AudioDspNoSpeexDllDependency @('KERNEL32.dll', 'speexdsp.dll') } '*Speex DLL dependency*' 'consumer Speex DLL dependency'
}

Invoke-Test 'Verifier rejects static CRT archive directives' {
    if (-not (Test-Path -LiteralPath $verifier)) { throw "Missing verifier: $verifier" }
    . $verifier
    Assert-Fails { Assert-AudioDspDynamicCrtDirectives @('/DEFAULTLIB:LIBCMT', '/DEFAULTLIB:OLDNAMES') } '*dynamic CRT /MD*' 'static CRT directive'
}

Invoke-Test 'Pinned install passes full verifier including native consumer proof when present' {
    $metadata = Join-Path $projectRoot 'work\build\audio-dsp\install\audio-dsp-build.json'
    if (Test-Path -LiteralPath $metadata) {
        $savedPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier 2>&1
            $verifierExit = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $savedPreference }
        if ($verifierExit -ne 0) { throw ($output | Out-String) }
        if ($output -notcontains 'AUDIO_DSP_CONSUMER_PROOF=PASS') { throw 'Real consumer proof was not reported.' }
    }
}

Write-Output "AUDIO_DSP_TESTS_PASSED=$script:Passed"
Write-Output "AUDIO_DSP_TESTS_FAILED=$script:Failed"
if ($script:Failed -ne 0) { exit 1 }
