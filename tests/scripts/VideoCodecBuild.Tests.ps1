Describe 'Video codec build plan' {
    It 'prints the pinned shared decoder build plan' {
        $plan = & "$PSScriptRoot\..\..\scripts\build-video-codecs.ps1" -PlanOnly

        $plan | Should -Contain 'X264_COMMIT=0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee'
        $plan | Should -Contain 'X265_COMMIT=b81f650e21e8aacbe6a9ad04ce14aefc05b932c0'
        $plan | Should -Contain 'FFMPEG_COMMIT=f88b741dbf242864e0c47b58e59dcbd67207c74b'
        $plan | Should -Contain 'X264_MODE=shared-win64-low-latency'
        $plan | Should -Contain 'X265_MODE=shared-win64-main8'
        $plan | Should -Contain 'FFMPEG_MODE=shared-decode-h264-hevc-d3d11va'
    }
}

Describe 'Video codec verifier fixtures' {
    BeforeAll {
        $projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        $verifierSource = Join-Path $projectRoot 'scripts\verify-video-codecs.ps1'
        $runtimeDll = Get-ChildItem (Join-Path $projectRoot 'work\build\video-codecs\install\bin') -Filter 'libx264*.dll' | Select-Object -First 1
        $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

        function Get-TestSha256([string]$Path) {
            $stream = [System.IO.File]::OpenRead($Path)
            try { return ([System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($stream))).Replace('-', '') }
            finally { $stream.Dispose() }
        }

        function New-VerifierFixture([string]$X264Commit) {
            $root = Join-Path $TestDrive ([Guid]::NewGuid().ToString('N'))
            $bin = Join-Path $root 'work\build\video-codecs\install\bin'
            New-Item -ItemType Directory -Force -Path (Join-Path $root 'scripts'), (Join-Path $root 'third_party'), $bin, (Join-Path $root 'work\build\video-codecs\install\licenses'), (Join-Path $root 'fakebin') | Out-Null
            Copy-Item -LiteralPath $verifierSource -Destination (Join-Path $root 'scripts\verify-video-codecs.ps1')
            Copy-Item -LiteralPath $runtimeDll.FullName -Destination (Join-Path $bin $runtimeDll.Name)
            @{
                x264 = @{ repository = 'fixture'; commit = '0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee' }
                x265 = @{ repository = 'fixture'; commit = 'b81f650e21e8aacbe6a9ad04ce14aefc05b932c0' }
                ffmpeg = @{ repository = 'fixture'; commit = 'f88b741dbf242864e0c47b58e59dcbd67207c74b' }
            } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'third_party\video-codecs.json') -Encoding utf8
            foreach ($name in @('x264-COPYING', 'x265-COPYING', 'ffmpeg-COPYING.LGPLv2.1')) { Set-Content -LiteralPath (Join-Path $root "work\build\video-codecs\install\licenses\$name") -Value 'fixture' -Encoding utf8 }
            $metadata = @{
                commits = @{ x264 = $X264Commit; x265 = 'b81f650e21e8aacbe6a9ad04ce14aefc05b932c0'; ffmpeg = 'f88b741dbf242864e0c47b58e59dcbd67207c74b' }
                configureFlags = @{
                    x264 = @('--enable-shared', '--disable-cli', '--enable-win32thread', '--bit-depth=8')
                    x265 = @('-DENABLE_SHARED=ON', '-DENABLE_CLI=OFF', '-DENABLE_ASSEMBLY=ON')
                    ffmpeg = @('--enable-shared', '--disable-static', '--disable-gpl', '--disable-programs', '--disable-doc', '--disable-network', '--disable-devices', '--disable-encoders', '--disable-muxers', '--disable-demuxers', '--disable-filters', '--disable-avformat', '--disable-avdevice', '--disable-avfilter', '--disable-swresample', '--disable-everything', '--enable-avcodec', '--enable-avutil', '--enable-swscale', '--enable-decoder=h264,hevc', '--enable-parser=h264,hevc', '--enable-hwaccel=h264_d3d11va,hevc_d3d11va', '--enable-d3d11va')
                }
                files = @{ ("bin/$($runtimeDll.Name)") = Get-TestSha256 (Join-Path $bin $runtimeDll.Name) }
            }
            $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'work\build\video-codecs\install\codec-build.json') -Encoding utf8
            $source = 'using System; class Program { static void Main() { Console.WriteLine("Image has the following dependencies:"); Console.WriteLine("    KERNEL32.dll"); Console.WriteLine("  Summary"); Console.WriteLine("Image has the following delay load dependencies:"); Console.WriteLine("    evil-delay.dll"); Console.WriteLine("  Summary"); } }'
            $sourcePath = Join-Path $root 'fakebin\dumpbin.cs'
            Set-Content -LiteralPath $sourcePath -Value $source -Encoding utf8
            & $csc /nologo ("/out:{0}" -f (Join-Path $root 'fakebin\dumpbin.exe')) $sourcePath
            if ($LASTEXITCODE -ne 0) { throw 'Could not compile the verifier fixture dumpbin.exe.' }
            return $root
        }

        function Invoke-FixtureVerifier([string]$Root) {
            $originalPath = $env:Path
            try {
                $env:Path = "$(Join-Path $Root 'fakebin');$originalPath"
                $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'scripts\verify-video-codecs.ps1') 2>&1
                if ($LASTEXITCODE -eq 0) { throw 'Verifier unexpectedly passed.' }
                throw ($output | Out-String)
            }
            finally { $env:Path = $originalPath }
        }
    }

    It 'rejects an unpackaged delay-load dependency' {
        $fixture = New-VerifierFixture '0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee'
        { Invoke-FixtureVerifier $fixture } | Should -Throw -ExpectedMessage '*evil-delay.dll*'
    }

    It 'rejects metadata with a mismatched manifest commit' {
        $fixture = New-VerifierFixture 'not-the-manifest-commit'
        { Invoke-FixtureVerifier $fixture } | Should -Throw -ExpectedMessage '*Metadata commit mismatch for x264*'
    }
}
