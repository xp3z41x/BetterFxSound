# Installers

## BetterFxSound (`BetterFxSound.iss`, Inno Setup)

The BetterFxSound installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or later by
`.github/workflows/release.yml`. It produces `BetterFxSound-<version>-x64-setup.exe`.

**Why Inno Setup.** The upstream projects (`fxsound*.aip`) are Advanced Installer *Professional* projects
(`Modules="professional"`). They use the auto-updater, an EXE bootstrapper, DLL custom actions and Authenticode
signing, which the free Advanced Installer edition can't build. Inno Setup is free, scriptable, and installs on
`windows-latest` with `choco install innosetup`.

What it does, compared with `fxsound.aip`:

| | `fxsound.aip` (official) | `BetterFxSound.iss` |
|---|---|---|
| Identity | FxSound, FxSound LLC, UpgradeCode `{1CA2081B-…}` | BetterFxSound, AppId `{D55BD9F9-D5A8-4ADF-8A83-B49AF7CC30D9}` |
| Install folder | `Program Files\FxSound LLC\FxSound` | `Program Files\BetterFxSound` (exe still `FxSound.exe`) |
| Files | FxSound.exe, fxdiag.exe, fxmcp.exe, fxmcp.mcpb, `Factsoft\*.fac`, fonts, `Apps\DfxSetupDrv.exe`, `DfxInstall.dll`, `Drivers\win7\|win10\<arch>\` | Same, x64 and Windows 10+ only. No `fxmcp.mcpb` (its manifest hard-codes the official path), no `updater.exe`, no `DfxInstall.dll` (its logic is ported to Pascal) |
| Driver | `DfxInstall.dll!InstallDFXDriver`: `DfxSetupDrv check` (removes the legacy DFX driver), `fxdevcon64 install fxvad.inf`, enable the endpoint, `DfxSetupDrv getguid` → `HKLM\Software\DFX\23\devices\dfx_guid`, `setname`, `defaultbuffersize`, `powercfg -REQUESTSOVERRIDE` | Same steps. Reuses an existing `Root\FXVAD` device and refreshes its package with `pnputil`, instead of creating a second device |
| Driver folder | `win10\<arch>` on Windows 10/11, `win7\<arch>` otherwise (WMI OS version) | `win10\x64` only (`MinVersion=10.0`) |
| Shortcuts | Start menu, desktop, Startup folder, "Check for FxSound updates" | Start menu, desktop and Startup (tasks, checked by default) |
| Uninstall | stops FxSound, `fxdevcon remove`, deletes the update task | stops FxSound, `fxdevcon64 remove` |
| User data | not touched | not touched (`%APPDATA%\FxSound`, `HKCU\SOFTWARE\DFX`) |

Before installing, it looks for an official FxSound uninstall entry (DisplayName `FxSound`). If it finds one, it
closes `FxSound.exe` and runs `msiexec /x <ProductCode> /qn`. That uninstall also removes the driver, so the driver
step installs it again.

Command-line switch `/SKIPDRIVER` (setup and uninstaller) skips the driver step.

Local build (Windows, Inno Setup installed):

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=1.0.1 /DFxSoundExe=<path>\FxSound.exe /DFxDiagExe=<path>\fxdiag.exe /DFxMcpExe=<path>\fxmcp.exe Installer\BetterFxSound.iss
```

## Releasing

1. Set the version in `fxsound/FxSound.jucer` and the generated files (`JuceLibraryCode/JuceHeader.h`,
   `Project/resources.rc`, `JUCE_APP_VERSION` in `Project/FxSound_App.vcxproj`).
2. Add `Installer/release-notes/v<version>.md`.
3. Push the tag `v<version>`. The workflow builds, smoke-tests and publishes a **pre-release**.
   `workflow_dispatch` builds and smoke-tests only.
