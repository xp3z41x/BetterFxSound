# Outputs rework: contract between the work sessions

Branch: `feature/outputs-rework` (from `main`). Each session works on its own branch made from
this one, and the orchestrator merges them. This file is the source of truth. If a session
needs to change the contract, it asks the orchestrator first.

## Decisions made by the owner

1. **Windows master volume applies to every output.** The level and mute of the FxSound
   virtual device become a software gain applied by the app to every output:
   `gain = master_scalar * (mute ? 0 : 1) * trim(volume_percent)`. The app no longer copies
   the master volume to a real device's endpoint volume, in either direction. The app no
   longer changes the volume of real devices.
2. **No "main output" in the UI or in the model.** Every output has a checkbox and can be
   disabled. Internally the engine still has a **carrier**: one enabled output on the original
   low-latency path (`SND_DEVICES_TARGETED_REAL_PLAYBACK`). The other enabled outputs go
   through `MultiOutputRouter`. When the carrier is disabled or disconnected, the app chooses
   the next carrier among the enabled outputs that are present.
3. **Zero enabled outputs is allowed and means silence.** The UI shows a short notice.
4. **Scope of this round:** Pro window grid, Lite window popup, tray menu, CLI and
   `status.json` and fxmcp, the "next output" hotkey, and HDMI failover.
5. The Settings dialog's "Outputs" pane is removed. Everything it did moves to the main
   window.

## Engine API (audiopassthru) — implemented by the "Backend – motor" session

These are added to `AudioPassthru` (`audiopassthru/include/AudioPassthru.h`):

```cpp
struct OutputConfig {
    std::wstring device_id;      // SoundDevice::pwszID
    bool enabled = false;
    int delay_ms = 0;            // 0..500
    int volume_percent = 100;    // 0..100, trim on top of the master volume
    OutputChannelMode channel_mode = OutputChannelMode::Both;
};
// The whole desired set of outputs, carrier included. The carrier is still chosen with
// setAsPlaybackDevice(). If the carrier is missing from the list or is disabled, the carrier
// path plays silence; no reinit is needed. Every other enabled entry that is present plays
// through MultiOutputRouter. The FxSound virtual device is always ignored.
void setOutputs(const std::vector<OutputConfig>& outputs);
```

- The master level and mute are read inside the engine, from the virtual device's
  `IAudioEndpointVolume` and its change callback. The value is published to the render paths
  lock-free, for example as an atomic float. Changes ramp smoothly to avoid clicks.
- Delay calibration measures **only the enabled outputs that are present**, carrier included
  only if it is enabled. `DelayCalibrationResult` keeps one entry per output, keyed by
  `device_id`. Nothing may assume that `results.front()` is the carrier. Calibration with
  zero enabled outputs fails with a clear error.
- `setAdditionalOutputs`, `setMainOutputDelay` and `setMainOutputChannelMode` stay as thin
  wrappers until the "Backend – controller" session no longer uses them. Then they are
  removed.

## Controller API used by the UI — declared in `FxController.h`

```cpp
struct OutputState { String id, name; bool enabled, present; int volume_percent, delay_ms;
                     OutputChannelMode channel_mode; };
std::vector<OutputState> getOutputs();
void setOutputEnabled(const String& id, bool enabled);
void setOutputVolume(const String& id, int volume_percent);
void setOutputDelay(const String& id, int delay_ms);
void setOutputChannelMode(const String& id, OutputChannelMode channel_mode);
int  getEnabledOutputCount();
// unchanged: startDelayCalibration, cancelDelayCalibration, getDelayCalibrationState,
//            getDelayCalibrationError, applyDelayCalibrationResults
```

- `getOutputs()` returns every real playback device that is present, in a stable order (the
  Windows device order). After them come the enabled outputs whose device is not connected
  (`present = false`). The order does not depend on which output is the carrier.
- Notifications: after any change to the device list, to which outputs are enabled, or to
  the settings caused by calibration, the controller sends `FxModel::Event::OutputListUpdated`.
  Volume, delay and channel changes coming from the UI do not notify. `OutputSelected` is no
  longer used by the UI.
- `applyDelayCalibrationResults()` aligns the measured outputs to the slowest one. Outputs
  that could not be measured keep their delay, and the summary for the user mentions them.
- The stubs at the end of `FxController.cpp` (marked `CONTRACT STUBS`) belong to the
  "Backend – controller" session. The Frontend session may change them on its own branch
  only to get test data. At merge time, the controller session's version wins.

## Settings (persistence) — "Backend – controller" session

- New key `outputs`: a JSON array of
  `{id, name, enabled, delay_ms, volume, channel_mode}`, holding enabled outputs and outputs
  that have been configured.
- One-time migration from `output_device_name` + `main_output_delay_ms` +
  `main_output_channel_mode` + `additional_outputs`. The old main output becomes an enabled
  entry, and the old additional outputs keep their enabled state and settings. The old keys
  are removed after the migration.
- Device preference and priority (`DeviceConfig`, "prioritize new devices") remain the
  policy for **choosing the carrier**. They no longer decide which outputs are enabled.

## Integrations — "Integrações" session

- `status.json`: add `outputs: [{id, name, enabled, present, volume, delay_ms, channel_mode}]`.
  Keep `output_devices`. `selected_output` is kept for compatibility and becomes the first
  enabled output.
- CLI: `--output=<friendly name>` becomes "enable only this output". Add
  `--enable-output=<name>`, `--disable-output=<name>`,
  `--output-volume=<name>:<0-100>` and `--output-delay=<name>:<0-500>`. Update
  `docs/COMMAND_LINE_OPTIONS.md`, run `go generate ./...` in `fxmcp/`, and update fxmcp
  (`status.go` plus write tools) and its unit tests.

## Who owns which files (to avoid conflicts)

| Session | Files |
|---|---|
| Backend – motor | `audiopassthru/**` |
| Backend – controller | `FxController.*` (except the UI-facing API declaration, which is fixed), `FxModel.*`, `Source/Utils/Settings/**` |
| Frontend | `FxView.*`, `FxProView.*`, `FxLiteView.*`, `FxSettingsDialog.*`, `FxSystemTrayView.*`, new widget files, `FxSound.jucer`/vcxproj when adding files |
| Integrações | `FxController::applyConfig` / status.json writing (after the controller session), `docs/COMMAND_LINE_OPTIONS.md`, `fxmcp/**` |

Rule of the repository (CLAUDE.md): changes in `audiopassthru/` need human review before
merge.
