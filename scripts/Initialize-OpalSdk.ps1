param([string]$EditionRoot = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference = 'Stop'
$edition = [IO.Path]::GetFullPath($EditionRoot)
$pins = @(
    @{name='opal'; url='https://git.code.sf.net/p/opalvoip/opal'; commit='aeda428ced0b1d8c96d4290bea97362bceb7faf2'},
    @{name='ptlib'; url='https://git.code.sf.net/p/opalvoip/ptlib'; commit='c2e9dea2434871baf3f9039db1aadb151bbd7c8b'},
    @{name='openssl-source'; url='https://github.com/openssl/openssl.git'; commit='45e844fa2a14ec92d146bd8f5778ac130b6625fb'}
)
New-Item -ItemType Directory -Path (Join-Path $edition 'dependencies') -Force | Out-Null
foreach ($pin in $pins) {
    $directory = Join-Path $edition ('dependencies\' + $pin.name)
    if (!(Test-Path -LiteralPath $directory)) {
        & git clone --no-checkout $pin.url $directory
        if ($LASTEXITCODE) { throw "Clone failed for $($pin.name)." }
        & git -C $directory checkout --detach $pin.commit
        if ($LASTEXITCODE) { throw "Pinned checkout failed for $($pin.name)." }
    }
    $head = & git -C $directory rev-parse HEAD
    if ($LASTEXITCODE -or $head -ne $pin.commit) { throw "Existing $($pin.name) is not the approved pinned commit; preserve it and use a new edition directory." }
}
