# Debug lab: rules shared by the debug sessions

Scope: only the fork's extra features: multiple outputs, channel mode (L/R/Mono), per-output
volume and delay, master volume on every output, giving each device back its own volume, the
hidden carrier and how it is elected, microphone delay sync, the outputs UI, the new CLI
options and status.json, fxmcp, and the installer. Don't debug FxSound's original DSP or UI.

The design spec is `docs/OUTPUTS_REWORK.md`.

## The machine

- BetterFxSound 1.0.0 is installed in `C:\Program Files\BetterFxSound`, with the FxSound
  virtual driver. The owner's settings are in `%APPDATA%\FxSound\FxSound.settings`. The app
  log is `%APPDATA%\FxSound\fxsound.log`, and a crash dump `fxsound.dmp` is there too.
  The engine's runtime state of the outputs (carrier in use, each other output's stream state,
  last error and underflow/realign counters) is in the `engine` object of `status.json`
  (`FxSound.exe --status`, or fxmcp's `fxsound_get_status`).
- There is no local C++ compiler. CI builds every pushed branch:
  - `.github/workflows/windows-build.yml` produces the `BetterFxSound-x64-<sha>` artifact
    with `FxSound.exe`.
  - `release.yml`, run by `workflow_dispatch`, also builds the installer.
  
  Download artifacts with `gh run download`. These are our own builds, so they are trusted.
- Python 3.14 is available. Go is at `X:\ZumbIA\tools\go\bin`.
- The owner allows live tests on this machine: test sounds, switching outputs and channel
  modes, WASAPI loopback capture of each output device (for example with `pyaudiowpatch` or
  `soundcard` from PyPI), and running CI-built exes.

## Audio lab lock: only one session uses the audio at a time

- Take the lock before anything that plays sound, changes outputs or settings, or
  starts/stops FxSound: `mkdir X:\BetterFxSound\.claude\audio-lab.lock`. `mkdir` is atomic,
  so if it fails, someone else holds the lock.
- Write your session name and the start time into `owner.txt` inside the lock folder.
- Hold the lock for 20 minutes at most per stretch. Release it with `rmdir /s` when done,
  even on failure.
- You may break a lock that is older than 40 minutes. Say so in your report.
- Before your first live test, snapshot the owner's state: copy `FxSound.settings`, export
  `HKCU\SOFTWARE\DFX`, and run `FxSound.exe --status` and save the result. Restore it before
  you release the lock.
- At the end, the installed `C:\Program Files\BetterFxSound\FxSound.exe` must be running
  again with the owner's settings.
- Running a CI build:
  1. Close the installed app.
  2. Copy the artifact's `FxSound.exe` into a scratch folder together with the install
     folder's `Factsoft`, `Apps`, the fonts and the other files.
  3. Run it with that folder as the working directory.
- Never uninstall the driver, never change it, never run the installer silently over the
  owner's install, and never change the owner's Windows default device permanently.

## Tools

- Put reusable test tools in `tools/audiolab/`, for example a loopback capture that reports
  L/R energy for each device, or a chirp detector.
- Before writing your own, check whether another session already pushed one:
  `git fetch` and `git log --all -- tools/audiolab`.

## Ownership (to keep merges clean)

| Session | Area |
|---|---|
| Debug: channel selector | Everything about `OutputChannelMode`. On the carrier path: channel mode, then delay, then gain, in `AudioPassthruPrivate`. In `SecondaryOutput`: mix format and channel mask, then channel mapping in `render`. The fast path of `configure()` for channel changes. `FxController::setOutputChannelMode`. |
| Debug: sync beeps | `DelayCalibration.*`, solo handling, `startDelayCalibration`, `applyDelayCalibrationResults`, the sync button in the UI. |
| Debug: engine and controller | Everything else in `MultiOutputRouter` / `SecondaryOutput`: rings, drift resampler, idle stop, reconnect. Master volume and gain ramps, the devices' own volume while in use and when given back, carrier election, device changes, HDMI, the hotkey, settings migration, power on/off. |
| Debug: UI, CLI, installer | `FxOutputsGrid`, the Pro/Lite/tray views, CLI and status.json, `fxmcp/`, `Installer/`, the workflows. |

If you find a bug in another session's area, don't fix it. Send it to the orchestrator with
evidence.

## Conventions

- Branch from `main`.
- Diagnose first, with evidence: logs, measurements, reasoning with file:line. Then fix as
  narrowly as you can.
- Add diagnostic logging only if it's cheap and safe for real time: never allocate or block
  on audio threads. Remove it, or keep it behind a flag, before you finish.
- Commit with lowercase conventional commits (`fix: ...`), using
  `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`, and end the message
  with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Push and keep CI green. Don't open a PR and don't merge: the orchestrator merges.
- Changes to `audiopassthru/` are high risk (they affect all system audio). Explain every one
  in your report.
