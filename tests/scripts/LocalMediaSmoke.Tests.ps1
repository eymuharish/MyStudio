[CmdletBinding()]
param()

Describe 'test-local-media all-controls smoke contract' {
    BeforeAll {
        $scriptPath = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'scripts\test-local-media.ps1'
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
    }

    It 'accepts only the current all-controls switches' {
        $parameters = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })

        $parameters | Should -Contain 'PackageRoot'
        $parameters | Should -Contain 'EvidenceRoot'
        $parameters | Should -Contain 'AllControls'
        $parameters | Should -Contain 'NoRetainedMedia'
        $parameters | Should -Not -Contain 'SystemAudioOnly'
    }

    It 'keeps an owned temp recording root and bounded event polling' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        $source | Should -Match 'EssaStudio-task13-smoke-'
        $source | Should -Match 'paired-recordings'
        $source | Should -Match 'function script:Wait-PairEvent'
        $source | Should -Match 'event-poll-timeout'
        $source | Should -Match 'Receive-PairOutput \$Peer'
        $source | Should -Match 'paired-engine-shutdown-timeout:\$\(\$Engine\.Name\):pid='
        $source | Should -Not -Match 'SystemAudioOnly'
    }

    It 'uses the native test-only recording output contract for both engines' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        $source | Should -Match 'ESSASTUDIO_TEST_MODE'
        $source | Should -Match 'ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR'
        $source | Should -Match 'record start '
        $source | Should -Match 'record stop'
        $source | Should -Match 'recording-state'
        $source | Should -Match 'exactly-one-recording-artifact'
    }

    It 'does not claim UI clicks when package UIA prerequisites are absent' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        $source | Should -Match 'UIA_CLICK_PROOF=NOT_EXECUTED'
        $source | Should -Match 'MANAGED_BUTTON_GATE='
        $source | Should -Not -Match 'UIA_CLICK_PROOF=PASS'
    }

    It 'checks the protocol-specific microphone events in paired calls' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        $source | Should -Match '\$sipText\.Contains\(''"type":"mute-changed"''\)'
        $source | Should -Match '\$h323Text\.Contains\(''"type":"mute-changed"''\)'
        $source | Should -Match '"type":"microphone-mute-changed"'
    }

    It 'requires confirmed payload values for every all-controls action' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        foreach ($required in @(
            '"source":"desktop"', '"microphone-mute-changed".*"muted":true',
            '"microphone-mute-changed".*"muted":false', '"system-audio-changed".*"enabled":true',
            '"system-audio-changed".*"enabled":false', '"room-audio-mute-changed".*"muted":true',
            '"room-audio-mute-changed".*"muted":false',
            '"masterPercent":100,"bassDb":0,"voiceDb":0,"trebleDb":0')) {
            $source | Should -Match ([regex]::Escape($required))
        }
    }

    It 'requires a fresh camera confirmation after stopping desktop sharing' {
        $source = Get-Content -LiteralPath $scriptPath -Raw -Encoding utf8

        $source | Should -Match 'function script:Wait-PairEventAfter'
        $source | Should -Match '\$sourceCameraCursor = \$first\.Transcript\.Count'
        $source | Should -Match 'Send-PairCommand \$first ''source camera'''
        $source | Should -Match 'Wait-PairEventAfter \$first.*source.*camera.*\$sourceCameraCursor'
    }
}
