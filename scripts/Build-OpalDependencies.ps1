param(
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
    [string]$EditionRoot = (Join-Path $PSScriptRoot '..\..')
)
$ErrorActionPreference = 'Stop'
$edition = [IO.Path]::GetFullPath($EditionRoot)
$ptlib = Join-Path $edition 'dependencies\ptlib'
$opal = Join-Path $edition 'dependencies\opal'
$evidence = Join-Path $edition 'evidence'
$openssl = Join-Path $edition 'dependencies\openssl'
foreach ($pin in @(@($ptlib,'c2e9dea2434871baf3f9039db1aadb151bbd7c8b'),@($opal,'aeda428ced0b1d8c96d4290bea97362bceb7faf2'),@((Join-Path $edition 'dependencies\openssl-source'),'45e844fa2a14ec92d146bd8f5778ac130b6625fb'))) {
    $head = & git -C $pin[0] rev-parse HEAD
    if ($LASTEXITCODE -or $head -ne $pin[1]) { throw "Dependency checkout differs from the approved pin: $($pin[0])" }
}
if (!(Test-Path -LiteralPath (Join-Path $openssl 'lib\VC\x64\MD\libcrypto.lib'))) { throw 'Build the pinned OpenSSL x64 dependency before PTLib.' }
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$options = '--disable-ffmpeg32 --disable-ffmpeg64 --disable-ffmpeg32wb --disable-ffmpeg64wb --disable-vxml --disable-dshow --disable-sapi --disable-sasl --disable-openldap --disable-lua32 --disable-lua64 --disable-v8 --disable-gstreamer --disable-gstreamer1 --disable-pcap --disable-odbc --disable-openssl --disable-opensslvc32 --disable-opensslvc64 --disable-imagemagick --disable-libjpeg'
$configureArguments = @('--no-search') + ($options -split ' ') + @("--openssl64-dir=$openssl")
function Invoke-LoggedBuild([string]$project, [string]$platform, [string]$name) {
    & $MSBuild $project /t:Build /m:4 /p:Configuration=Release "/p:Platform=$platform" /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /p:PostBuildEventUseInBuild=false /v:minimal *> (Join-Path $evidence "$name.log")
    if ($LASTEXITCODE) { Get-Content (Join-Path $evidence "$name.log") -Tail 30; throw "$name failed: $LASTEXITCODE" }
}
function Copy-IfChanged([string]$from, [string]$to) {
    if (!(Test-Path -LiteralPath $to) -or (Get-FileHash -LiteralPath $from).Hash -ne (Get-FileHash -LiteralPath $to).Hash) {
        Copy-Item -LiteralPath $from -Destination $to
    }
}
$previousPtlib = $env:PTLIBDIR
$previousOptions = $env:PTLIB_CONFIGURE_OPTIONS
$previousPath = $env:PATH
$previousClOptions = $env:_CL_
try {
    $env:PTLIBDIR = $ptlib
    $env:PTLIB_CONFIGURE_OPTIONS = $options + ' --openssl64-dir="' + $openssl + '"'
    $env:PATH = (Join-Path $ptlib 'bin\MergeSym\Release') + ';' + (Join-Path $ptlib 'lib') + ';' + (Join-Path $openssl 'bin') + ';' + $env:PATH
    $env:_CL_ = ($previousClOptions + ' /FS').Trim()
    # The pinned Windows-version fallback has a pointer-to-pointer/ANSI mismatch.
    $win32 = Join-Path $ptlib 'src\ptlib\msos\win32.cxx'
    $original = [IO.File]::ReadAllText($win32)
    $fixed = $original.Replace('return GetVersionEx(&info);', 'return GetVersionExW(info);')
    if ($fixed -ne $original) { [IO.File]::WriteAllText($win32, $fixed) }
    # PAwsSigner remains compiled without AWS SDK and needs PProcess itself.
    $aws = Join-Path $ptlib 'src\ptclib\aws_sdk.cxx'
    $original = [IO.File]::ReadAllText($aws)
    if ($original.IndexOf('#include <ptlib/pprocess.h>') -gt $original.IndexOf('#if P_AWS_SDK')) {
        $fixed = $original.Replace('#include <ptclib/aws_sdk.h>', "#include <ptclib/aws_sdk.h>`r`n#include <ptlib/pprocess.h>")
        [IO.File]::WriteAllText($aws, $fixed)
    }
    # The SDK project lists Deepgram even when recognition is disabled.
    $deepgram = Join-Path $ptlib 'src\ptclib\speech_deep.cxx'
    $original = [IO.File]::ReadAllText($deepgram)
    if (!$original.Contains('#if P_SPEECH_RECOGNITION')) {
        $fixed = $original.Replace('#include <ptlib.h>', "#include <ptlib.h>`r`n#if P_SPEECH_RECOGNITION") + "`r`n#endif // P_SPEECH_RECOGNITION`r`n"
        [IO.File]::WriteAllText($deepgram, $fixed)
    }
    $mergeProject = Join-Path $ptlib 'tools\MergeSym\MergeSym_2022.vcxproj'
    $original = [IO.File]::ReadAllText($mergeProject)
    if (!$original.Contains('<LanguageStandard>')) {
        [IO.File]::WriteAllText($mergeProject, $original.Replace('<ClCompile>', "<ClCompile>`r`n      <LanguageStandard>stdcpp17</LanguageStandard>"))
    }
    # This DLL project hard-codes swresample although FFmpeg is disabled here.
    $opalDllProject = Join-Path $opal 'src\win32\opal_dll_2022.vcxproj'
    $original = [IO.File]::ReadAllText($opalDllProject)
    $fixed = $original.Replace('<AdditionalDependencies>swresample.lib;', '<AdditionalDependencies>')
    if ($fixed -ne $original) { [IO.File]::WriteAllText($opalDllProject, $fixed) }
    $bfcp = Join-Path $opal 'include\sdp\bfcpinterface.h'
    if (!([IO.File]::ReadAllText($bfcp).Contains('bool RequestFloor(bool release = false)'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-floor-control.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the reviewed BFCP patch to the pinned OPAL source.' }
    }
    $bfcpSource = Join-Path $opal 'src\sdp\bfcpinterface.cxx'
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('dedicated presentation floor'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-presentation-floor.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP presentation floor selection patch.' }
    }
    $rtpSource = Join-Path $opal 'src\rtp\rtp_session.cxx'
    if (!([IO.File]::ReadAllText($rtpSource).Contains('Compare the bound target so closed streams are removed'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\rtp-notifier-lifetime.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the RTP notifier lifetime patch.' }
    }
    $bfcpLogger = Join-Path $opal 'src\sdp\BFCP\libbfcp\bfcpsrvctl\bfcpsrv\bfcp_server.cpp'
    if ([IO.File]::ReadAllText($bfcpLogger).Contains('static BFCP_Server _p')) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-logger-lifetime.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP logger lifetime patch.' }
    }
    $sipConnectionSource = Join-Path $opal 'src\sip\sipcon.cxx'
    $callSource = Join-Path $opal 'src\opal\call.cxx'
    if (!([IO.File]::ReadAllText($callSource).Contains('ReferencedIterator(it).AssignTo(connection)'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\call-enumeration-reference-safety.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the removed call connection reference safety patch.' }
    }
    if (!([IO.File]::ReadAllText($sipConnectionSource).Contains('OpalTransportPtr SIPConnection::GetEstablishedDialogTransport() const'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\sip-dialog-bye-flow.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the established SIP dialog BYE flow patch.' }
    }
    $sipPduSource = Join-Path $opal 'src\sip\sippdu.cxx'
    if (!([IO.File]::ReadAllText($sipPduSource).Contains('SIP dialog transaction'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\sip-dialog-controls-flow.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the established SIP dialog controls flow patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('BFCP is a control protocol'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-wildcard-sdp.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP wildcard control SDP patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('Register negotiated remote and virtual-local users before transport callbacks'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-negotiated-presentation.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the negotiated BFCP presentation initialization patch.' }
    }
    $bfcpParticipant = Join-Path $opal 'src\sdp\BFCP\libbfcp\bfcpsrvctl\bfcpcli\bfcp_participant.cpp'
    if (!([IO.File]::ReadAllText($bfcpParticipant).Contains('This wrapper owns the request list on success and failure alike'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-request-list-ownership.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP request list ownership patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('void BFCPSession::LibParticipantWrapper::StopParticipant()'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-participant-lifetime.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP participant lifetime patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpLogger).Contains('sockfd == BFCP_INVALID_SOCKET || u->sockFd == BFCP_INVALID_SOCKET'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-windows-socket-sentinel.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the Windows BFCP socket sentinel patch.' }
    }
    $bfcpConnection = Join-Path $opal 'src\sdp\BFCP\libbfcp\BFCPconnection.cpp'
    if (!([IO.File]::ReadAllText($bfcpConnection).Contains('both actual Windows threads have exited'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-windows-thread-join.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the Windows BFCP thread completion patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('bool BFCPSession::OpenTCP('))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-tcp-role-port.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP negotiated TCP role and port patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('static bool IsPresentationFloor('))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-content-role-disconnect-log.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP content role and disconnect log patch.' }
    }
    $bfcpParser = Join-Path $opal 'src\sdp\BFCP\libbfcp\bfcpmsg\bfcp_messages_parse.c'
    if (!([IO.File]::ReadAllText($bfcpParser).Contains('Mandatory nested floor status is absent'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-grant-fsm-parser.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP grant state and bounded nested status patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpParser).Contains('MyStudio bounded optional overall status'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-overall-status-bounds.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP optional overall status bounds patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('Local control has no received wire status'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-local-fsm-event-safety.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP local FSM event safety patch.' }
    }
    $sdpEndpoint = Join-Path $opal 'src\sdp\sdpep.cxx'
    if (!([IO.File]::ReadAllText($sdpEndpoint).Contains('Offer construction has a signalling-host placeholder'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-offer-session-reuse.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the established BFCP offer session reuse patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpConnection).Contains('Failed connect still owns real thread handles'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-failed-connect-join.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP failed connect thread ownership patch.' }
    }
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('BFCP owns libbfcp sockets, not an OpalMediaTransport'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-session-open-state.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP actual socket state and session close patch.' }
    }
    $sipPdu = Join-Path $opal 'src\sip\sippdu.cxx'
    if (!([IO.File]::ReadAllText($bfcpSource).Contains('void BFCPDescription::SetFloorStreamMap('))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\bfcp-floor-map-synchronization.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the BFCP map-only synchronization and snapshot patch.' }
    }
    if (!([IO.File]::ReadAllText($sipPdu).Contains('An idle read timeout is not a lost flow'))) {
        & git -C $opal apply --recount (Join-Path $PSScriptRoot '..\native\opal\patches\sip-idle-read-timeout.patch')
        if ($LASTEXITCODE) { throw 'Cannot apply the reliable SIP pre-byte idle read patch.' }
    }
    Push-Location $ptlib
    try {
        & '.\configure.exe' @configureArguments *> (Join-Path $evidence 'ptlib-configure.log')
        if ($LASTEXITCODE) { throw 'PTLib configure failed' }
        # Legacy configure only searches Windows Kits 8; DNS is supplied by the installed Windows 10 SDK.
        $configuration = [IO.File]::ReadAllText((Join-Path $ptlib 'include\ptlib_config.h'))
        $configuration = $configuration.Replace('#undef P_DNS_RESOLVER', '#define P_DNS_RESOLVER 1')
        $configuration = $configuration -replace '(?m)^#undef P_HAS_IPV6\r?$', '#define P_HAS_IPV6 1'
        # OpenSSL 3.5 adds headers beyond this SDK's legacy include_alias list.
        $extraAliases = foreach ($header in Get-ChildItem -LiteralPath (Join-Path $openssl 'include\openssl') -Filter '*.h' -File) {
            $alias = '  #pragma P_SSL_INCLUDE(openssl/' + $header.Name + ')'
            if (!$configuration.Contains($alias.Trim())) { $alias }
        }
        if ($extraAliases) { $configuration = $configuration.Replace('  #pragma P_SSL_INCLUDE(openssl/ssl.h)', ($extraAliases -join "`r`n") + "`r`n  #pragma P_SSL_INCLUDE(openssl/ssl.h)") }
        [IO.File]::WriteAllText((Join-Path $ptlib 'include\ptlib_config.h'), $configuration)
        New-Item -ItemType Directory -Path 'lib\x64\include' -Force | Out-Null
        Copy-IfChanged 'include\ptlib_config.h' 'lib\x64\include\ptlib_config.h'
        Invoke-LoggedBuild 'tools\MergeSym\MergeSym_2022.vcxproj' 'x64' 'mergesym-build'
        Invoke-LoggedBuild 'src\ptlib\msos\PTLib_2022.vcxproj' 'x64' 'ptlib-build'
    } finally { Pop-Location }
    Push-Location $opal
    try {
        & '.\configure.exe' --no-search --disable-iax2 --disable-skinny --disable-lync --disable-fax --disable-ivr --disable-java --disable-csharp --disable-ruby --disable-presence --disable-msrp --disable-sipim --disable-rfc4103 --disable-h501 *> (Join-Path $evidence 'opal-configure.log')
        if ($LASTEXITCODE) { throw 'OPAL configure failed' }
        New-Item -ItemType Directory -Path 'lib\x64\include' -Force | Out-Null
        Copy-IfChanged 'include\opal_config.h' 'lib\x64\include\opal_config.h'
        Invoke-LoggedBuild 'src\win32\opal_dll_2022.vcxproj' 'x64' 'opal-build'
        Invoke-LoggedBuild 'plugins\audio\G722\G722_2022.vcxproj' 'x64' 'g722-build'
    } finally { Pop-Location }
    [ordered]@{ ptlib=(git -C $ptlib rev-parse HEAD); opal=(git -C $opal rev-parse HEAD); openssl=(git -C (Join-Path $edition 'dependencies\openssl-source') rev-parse HEAD); toolset='v143'; platform='x64'; tls=$true; patches=@('Windows GetVersionExW pointer fix','AWS signer PProcess include','Deepgram speech feature guard','Windows SDK DNS and IPv6 detection','OpenSSL3.5 header aliases','MergeSym C++17','Disable unused hardcoded swresample link','BFCP floor control, UDP initialization and presentation floor selection','RTP notifier target lifetime','BFCP logger lifetime','Reuse open reliable established SIP dialog flow for BYE and in-dialog controls','BFCP wildcard control SDP format','Initialize negotiated BFCP presentation floor and users; reuse its SDP session','Single-owner BFCP floor request list on success and failure','Stop BFCP participant timer and wire callbacks before destroying their owner','Windows BFCP unsigned socket sentinel','Join Windows BFCP wire and retransmission threads before deleting their owner','Honor negotiated BFCP TCP setup role and valid SDP port','Map BFCP floor to actual negotiated presentation content role; correct disconnect log arguments','BFCP request/release FSM and bounded mandatory nested floor status parsing','Bounded optional BFCP floor and overall status with nullable consumer guards','Initialized local BFCP FSM events and safe success/failure logging','Preserve open BFCP session when constructing re-INVITE offers','Join BFCP failed TCP connect threads and close each handle once','Track actual BFCP socket state and stop owned wrappers on session close','Retain active reliable SIP flows across pre-byte idle read timeouts') } | ConvertTo-Json | Set-Content (Join-Path $evidence 'dependency-build.json')
} finally { $env:PTLIBDIR=$previousPtlib; $env:PTLIB_CONFIGURE_OPTIONS=$previousOptions; $env:PATH=$previousPath; $env:_CL_=$previousClOptions }
