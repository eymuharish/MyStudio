param(
    [Parameter(Mandatory = $true)][string]$PerlExe,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../../../..'),
    [string]$VsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
    [string]$VcVersion = '14.44'
)
$ErrorActionPreference = 'Stop'
$outputDirectory = [IO.Path]::GetFullPath($OutputRoot)
$sourceDirectory = Join-Path $outputDirectory 'dependencies/openssl-source'
$installDirectory = Join-Path $outputDirectory 'dependencies/openssl'
$buildDirectory = Join-Path $outputDirectory 'build/openssl/native'
$evidenceDirectory = Join-Path $outputDirectory 'evidence'
$perlExecutable = (Resolve-Path -LiteralPath $PerlExe).Path
$vsEnvironmentScript = (Resolve-Path -LiteralPath $VsDevCmd).Path
foreach ($path in @($outputDirectory, $perlExecutable, $vsEnvironmentScript, $VcVersion)) {
    if ($path -match '["\r\n%!&|<>]') { throw 'Build paths and toolchain version must be safe literal Windows paths' }
}
foreach ($target in @($sourceDirectory, $installDirectory, $buildDirectory, $evidenceDirectory)) {
    $absolute = [IO.Path]::GetFullPath($target)
    if (-not $absolute.StartsWith($outputDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Dependency build path escaped its output directory'
    }
}
$expectedCommit = '45e844fa2a14ec92d146bd8f5778ac130b6625fb'
if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory '.git'))) {
    New-Item -ItemType Directory -Path (Split-Path $sourceDirectory) -Force | Out-Null
    & git clone --depth 1 --branch openssl-3.5.9 https://github.com/openssl/openssl.git $sourceDirectory
    if ($LASTEXITCODE -ne 0) { throw 'OpenSSL release clone failed' }
}
$commit = (& git -C $sourceDirectory rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -ne $expectedCommit) { throw 'OpenSSL source is not the verified 3.5.9 release commit' }
$perlPlatform = (& $perlExecutable -e 'print $^O' 2>$null)
if ($LASTEXITCODE -ne 0 -or $perlPlatform -ne 'MSWin32') { throw 'A native Windows Perl is required, such as official portable Strawberry Perl' }
New-Item -ItemType Directory -Path $buildDirectory, $evidenceDirectory -Force | Out-Null
$driver = Join-Path (Split-Path $buildDirectory) 'build-openssl.cmd'
$driverText = @"
@echo off
setlocal
call "$vsEnvironmentScript" -arch=x64 -host_arch=x64 -vcvars_ver=$VcVersion >nul
if errorlevel 1 exit /b %errorlevel%
set LANG=C
set LC_ALL=C
set LC_CTYPE=C
cd /d "$buildDirectory"
if /i "%~1"=="configure" goto configure
if /i "%~1"=="build" goto build
if /i "%~1"=="install" goto install
exit /b 2
:configure
"$perlExecutable" "$sourceDirectory\Configure" VC-WIN64A shared no-tests no-asm no-makedepend "--prefix=$installDirectory" "--openssldir=$installDirectory\ssl"
exit /b %errorlevel%
:build
nmake /NOLOGO
exit /b %errorlevel%
:install
nmake /NOLOGO install_sw
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($driver, $driverText, [Text.UTF8Encoding]::new($false))
function Invoke-BuildStep([string]$Step) {
    & cmd.exe /d /c ('"' + $driver + '" ' + $Step) *> (Join-Path $evidenceDirectory "openssl-$Step.log")
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL $Step failed; see evidence/openssl-$Step.log" }
}
Invoke-BuildStep 'configure'
$makefile = Join-Path $buildDirectory 'makefile'
$makeText = [IO.File]::ReadAllText($makefile)
if ([regex]::Matches($makeText, '(?m)^(LIB|DSO|BIN)_CFLAGS=/Zi /Fd[^\r\n]+\r?$').Count -ne 3) { throw 'Unexpected OpenSSL Windows compiler flags' }
$original = Join-Path (Split-Path $buildDirectory) 'makefile-Zi-original'
if (-not (Test-Path -LiteralPath $original)) { Copy-Item -LiteralPath $makefile -Destination $original }
$makeText = $makeText.Replace('/Zi /Fdossl_static.pdb', '/Z7').Replace('/Zi /Fddso.pdb', '/Z7').Replace('/Zi /Fdapp.pdb', '/Z7')
[IO.File]::WriteAllText($makefile, $makeText, [Text.UTF8Encoding]::new($false))
Invoke-BuildStep 'build'
Invoke-BuildStep 'install'
$vcLibraryDirectory = Join-Path $installDirectory 'lib/VC/x64/MD'
New-Item -ItemType Directory -Path $vcLibraryDirectory -Force | Out-Null
foreach ($library in @('libcrypto.lib', 'libssl.lib')) {
    Copy-Item -LiteralPath (Join-Path $installDirectory "lib/$library") -Destination (Join-Path $vcLibraryDirectory $library)
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'LICENSE.txt') -Destination (Join-Path $installDirectory 'LICENSE.txt')
& (Join-Path $installDirectory 'bin/openssl.exe') version -a *> (Join-Path $evidenceDirectory 'openssl-version.log')
if ($LASTEXITCODE -ne 0) { throw 'Installed OpenSSL did not start' }
Write-Output "OpenSSL 3.5.9 x64 installed: $installDirectory"
