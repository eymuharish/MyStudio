param([string]$EditionRoot = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference = 'Stop'
$edition = [IO.Path]::GetFullPath($EditionRoot)
$media = Join-Path $edition 'dependencies\media'
$actualHeads = @{}
foreach ($name in @('opal','ptlib','openssl-source')) {
    $actualHeads[$name] = & git -C (Join-Path $edition ('dependencies\'+$name)) rev-parse HEAD
    if ($LASTEXITCODE) { throw "Cannot read dependency provenance: $name" }
}
$expectedHeads = @{opal='aeda428ced0b1d8c96d4290bea97362bceb7faf2';ptlib='c2e9dea2434871baf3f9039db1aadb151bbd7c8b';'openssl-source'='45e844fa2a14ec92d146bd8f5778ac130b6625fb'}
foreach ($name in $expectedHeads.Keys) { if ($actualHeads[$name] -ne $expectedHeads[$name]) { throw "Dependency pin mismatch: $name" } }
$data = [ordered]@{
    schemaVersion = 1
    product = 'MyStudio'
    version = '2.2.0'
    baselineCommit = 'd50caf6e14c9c56954e0584aa129243cfdbf1550'
    platform = 'Windows x64'
    verifiedHeads = $actualHeads
    buildScriptsSha256 = @{sdk=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Build-OpalDependencies.ps1')).Hash;openssl=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '..\native\opal\aac\BuildOpenSsl.ps1')).Hash;bfcp=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-floor-control.patch')).Hash}
    opal = @{ repository='https://git.code.sf.net/p/opalvoip/opal'; commit='aeda428ced0b1d8c96d4290bea97362bceb7faf2'; version='4.0.0'; patch='native/opal/patches/bfcp-floor-control.patch' }
    sdkPatchHashes = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '..\native\opal\patches') -Filter '*.patch' -File | ForEach-Object { @{path=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash} })
    ptlib = @{ repository='https://git.code.sf.net/p/opalvoip/ptlib'; commit='c2e9dea2434871baf3f9039db1aadb151bbd7c8b'; version='4.0.0'; patches='scripts/Build-OpalDependencies.ps1' }
    openssl = @{ repository='https://github.com/openssl/openssl.git'; commit='45e844fa2a14ec92d146bd8f5778ac130b6625fb'; tag='openssl-3.5.9'; configuration='VC-WIN64A shared no-tests no-asm no-makedepend'; runtime='/MD' }
    aac = (Get-Content -LiteralPath (Join-Path $media 'aac-ld\aac-ld-build.json') -Raw | ConvertFrom-Json)
    video = (Get-Content -LiteralPath (Join-Path $media 'video-codecs\codec-build.json') -Raw | ConvertFrom-Json)
    audioDsp = (Get-Content -LiteralPath (Join-Path $media 'audio-dsp\audio-dsp-build.json') -Raw | ConvertFrom-Json)
    libyuv = @{ source='Existing MyStudio PJSIP bundled libyuv headers/archive'; sha256=(Get-FileHash -LiteralPath (Join-Path $media 'libyuv\lib\libyuv.lib')).Hash; upstreamLicense='https://chromium.googlesource.com/libyuv/libyuv/+/refs/heads/main/LICENSE'; commit='Not recorded by original artifact' }
}
$data | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $PSScriptRoot '..\docs\opal-dependencies.json') -Encoding utf8
