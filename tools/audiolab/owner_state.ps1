<#
Snapshot and restore the owner's FxSound state around live tests (docs/DEBUG_LAB.md).

  .\tools\audiolab\owner_state.ps1 snapshot <dir>   # FxSound running: copies settings, presets, HKCU\SOFTWARE\DFX, --status
  .\tools\audiolab\owner_state.ps1 restore <dir>    # closes FxSound gracefully, puts everything back, starts the installed app
  .\tools\audiolab\owner_state.ps1 close            # graceful close only (Restart Manager, like an installer would)

Take the audio lab lock first.
#>
param(
    [Parameter(Mandatory)][ValidateSet('snapshot', 'restore', 'close')][string]$Action,
    [string]$Dir
)
$ErrorActionPreference = 'Stop'
$appData = Join-Path $env:APPDATA 'FxSound'
$installDir = 'C:\Program Files\BetterFxSound'
$exe = Join-Path $installDir 'FxSound.exe'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Close-FxSound {
    if (-not (Get-Process FxSound -ErrorAction SilentlyContinue)) { Write-Host 'FxSound is not running'; return }
    python (Join-Path $here 'rm_close.py') $exe
    if (Get-Process FxSound -ErrorAction SilentlyContinue) { throw 'FxSound is still running after the graceful close' }
}

switch ($Action) {
    'snapshot' {
        New-Item -ItemType Directory -Force $Dir | Out-Null
        if (Test-Path "$Dir\FxSound.settings") { throw "$Dir already holds a snapshot" }
        Copy-Item "$appData\FxSound.settings" $Dir
        foreach ($d in 'Presets', 'AutoSave') { if (Test-Path "$appData\$d") { Copy-Item -Recurse "$appData\$d" "$Dir\$d" } }
        reg export 'HKCU\SOFTWARE\DFX' "$Dir\dfx.reg" /y | Out-Null
        python (Join-Path $here 'fxcli.py') status | Out-File -Encoding utf8 "$Dir\outputs.txt"
        Copy-Item "$appData\status.json" "$Dir\status.json"
        Get-Content "$Dir\outputs.txt"
        Write-Host "snapshot saved in $Dir"
    }
    'close' { Close-FxSound }
    'restore' {
        if (-not (Test-Path "$Dir\FxSound.settings")) { throw "no snapshot in $Dir" }
        Close-FxSound
        Copy-Item "$Dir\FxSound.settings" "$appData\FxSound.settings" -Force
        foreach ($d in 'Presets', 'AutoSave') {
            if (Test-Path "$Dir\$d") {
                if (Test-Path "$appData\$d") { Remove-Item -Recurse -Force "$appData\$d" }
                Copy-Item -Recurse "$Dir\$d" "$appData\$d"
            }
        }
        reg delete 'HKCU\SOFTWARE\DFX' /f | Out-Null
        reg import "$Dir\dfx.reg" 2>&1 | Out-Null
        Start-Process -FilePath $exe -WorkingDirectory $installDir
        Start-Sleep 6
        python (Join-Path $here 'fxcli.py') status
        Write-Host '--- snapshot outputs ---'
        Get-Content "$Dir\outputs.txt"
    }
}
