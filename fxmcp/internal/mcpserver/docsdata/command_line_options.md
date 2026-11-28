# FxSound Command Line Options

FxSound accepts a set of command line options that let you configure the app on launch, or push new settings to an already-running instance.

For example, run `fxsound.exe --preset="Bass Booster"` at any time to switch the running instance's preset without restarting it or affecting any other setting.

Each option's value must be attached directly to the option with `=` — e.g. `--power=1`. A space between the option and its value is **not** supported for these options; `--power 1` is parsed as two separate, unrelated arguments and the value is silently ignored.

Options with numeric values that fall outside their valid range are silently reset to their default — no error is shown.

`--balance` and `--master_gain` are additionally rounded to the nearest whole number, and `--filter_q` and `--volume_leveling` to the nearest `0.5`, matching the step size of their equivalent sliders in the Settings dialog.

Values containing spaces (preset names, device names) must be wrapped in double quotes immediately after the `=`, e.g. `--preset="Bass Booster"`.

| Option | Value | Description |
|---|---|---|
| `--power=<0\|1>` | `0` = off, `1` = on | Turns FxSound's audio processing (DFX) on or off. |
| `--preset=<name>` | Preset name (case-sensitive) | Selects the active preset. Only takes effect if power is currently on. Exclusive with the other preset commands below - see [Preset commands](#preset-commands). |
| `--save_preset=<name>` | New preset name | Saves the current (modified) settings as a new user preset and selects it. `name` is sanitized first - see [Preset commands](#preset-commands). No-op if power is off, the current preset isn't modified, or `name` is empty after sanitizing. Exclusive with the other preset commands below. |
| `--overwrite_preset` | *(flag, no value)* | Overwrites the currently selected user preset with its modified settings. No-op if power is off, the preset isn't modified, or the selected preset is a built-in preset. Exclusive with the other preset commands below. |
| `--undo_preset` | *(flag, no value)* | Reverts the currently selected preset's unsaved changes. No-op if power is off or the preset isn't modified. Exclusive with the other preset commands below. |
| `--rename_preset=<name>` | New preset name | Renames the currently selected user preset. `name` is sanitized first - see [Preset commands](#preset-commands). No-op if power is off, the preset has unsaved changes, the selected preset is a built-in preset, or `name` is empty after sanitizing. Exclusive with the other preset commands below. |
| `--delete_preset` | *(flag, no value)* | Deletes the currently selected user preset. No-op if power is off or the selected preset is a built-in preset. Exclusive with the other preset commands below. |
| `--output=<device>` | Output device's friendly name, or its endpoint id | Enables only this output and disables every other one. Must match a connected device's name returned by Windows exactly, or its `id` in `status.json`; ignored if no connected device matches. |
| `--enable-output=<device>` | Output device's friendly name, or its endpoint id | Enables this output, leaving the others as they are. Running instance only. The device must be connected. |
| `--disable-output=<device>` | Output device's friendly name, or its endpoint id | Disables this output, leaving the others as they are. Running instance only. With every output disabled, FxSound plays silence. |
| `--output-volume=<device>:<0-100>` | Friendly name or endpoint id, then `:`, then an integer `0`–`100` | Sets this output's volume trim in percent, on top of the Windows master volume (which applies to every output). Running instance only. |
| `--output-delay=<device>:<0-500>` | Friendly name or endpoint id, then `:`, then an integer `0`–`500` | Sets this output's delay in milliseconds. Running instance only. |
| `--view=<1\|2>` | `1` = Lite, `2` = Full | Switches the UI between Lite and Full view. |
| `--language=<code>` | Language code, e.g. `en`, `fr`, `fi` | Sets the display language. Falls back to the saved language, then the system display language, if omitted. |
| `--num_bands=<n>` | Integer, one of `5`, `10`, `15`, `20`, `31` | Sets the number of equalizer bands. Default: `10`. |
| `--balance=<n>` | Decimal, `-20.0`–`+20.0` | Sets the left/right stereo balance in dB. Default: `0`. |
| `--filter_q=<n>` | Decimal, `1.0`–`3.0` | Sets the EQ filter Q factor (controls the bandwidth of each band). Default: `1.0`. |
| `--master_gain=<n>` | Decimal, `-20.0`–`+20.0` | Sets the master gain in dB. Default: `0`. |
| `--volume_leveling=<n>` | Decimal, `0.0`–`4.0` | Sets the volume leveling amount in dB. Default: `0`. |
| `--set_band_freq=<band:freq[,band:freq...]>` | `band` = 0-based band index, `freq` = frequency in Hz | Sets one or more equalizer band center frequencies on a running instance. `band` must be less than the current number of bands; `freq` must fall within that band's allowed range or the pair is ignored. Running instance only. |
| `--set_band_gain=<band:gain[,band:gain...]>` | `band` = 0-based band index, `gain` = decimal, `-12.0`–`+12.0` | Sets one or more equalizer band boost/cut values in dB on a running instance. `band` must be less than the current number of bands, or the pair is ignored. Running instance only. |
| `--set_effect=<name:value[,name:value...]>` | `name` = one of `fidelity`/`clarity`, `ambience`, `surround`, `dynamicboost`/`dynamic_boost`, `bass`/`bassboost`/`bass_boost`; `value` = decimal, `0.0`–`10.0` | Sets one or more effect levels on a running instance. Out-of-range values are ignored (left unchanged) rather than reset to a default. Running instance only. |
| `--status` | *(flag, no value)* | Writes the running instance's current state (power, presets, output devices, per-output state, equalizer, effects) as JSON to `%APPDATA%\FxSound\status.json`; any other options passed alongside `--status` are ignored. |
| `--run_minimized` | *(flag, no value)* | Starts FxSound minimized to the system tray without showing the main window. On a running instance, only affects visibility for that launch (hides the main window instead of bringing it to the front). |

## Output commands

Outputs are a flat list: each one can be enabled or disabled, and with none enabled FxSound plays silence. There is no main output. The Windows master volume applies to every enabled output; `--output-volume` is a per-output trim on top of it.

`--output`, `--enable-output`, `--disable-output`, `--output-volume` and `--output-delay` match outputs by friendly name, exactly, or by endpoint id (the `id` of each entry in the `outputs` array of `status.json`). An endpoint id picks exactly that output. Several devices can share a name (two identical monitors or DACs, or a device that came back under a new endpoint id next to its old entry): a name then picks every output with that name. `--output` and `--enable-output` need the device connected, so they only act on the connected ones; `--disable-output`, `--output-volume` and `--output-delay` act on the disconnected ones too, so disabling a name never leaves a same-named output enabled. To act on one of several same-named outputs, pass its endpoint id. For `--output-volume` and `--output-delay` the name is split from the number at the **last** colon, so names that contain `:` work. Quote the whole value when it has spaces: `--output-volume="Speakers (Realtek):50"` (quotes around the name only, `--output-volume="Speakers (Realtek)":50`, also work). Values out of range, or a name that matches no output, are ignored. Each output's state (`enabled`, `present`, `volume`, `delay_ms`, `channel_mode`) is reported in the `outputs` array of `status.json`; `selected_output` there is the first enabled output.

## Preset commands

`--preset`, `--save_preset`, `--overwrite_preset`, `--undo_preset`, `--rename_preset`, and `--delete_preset` are **mutually exclusive**: only one is processed per invocation, in that priority order, regardless of how many are passed together (any others are silently ignored). Each mirrors the equivalent item in the app's menu (Save New Preset / Overwrite Existing Preset / Undo Preset Changes / Rename Preset / Delete Preset) and the same enabled/disabled conditions apply, as described in the table above.

`--save_preset` also checks the user preset count against the max user preset limit before saving, and is a no-op once the limit is reached - matching the "Save New Preset" menu item, which disables itself at the same point.

### Name sanitizing

Before `--save_preset` or `--rename_preset` acts on `name`, it's sanitized the same way the app's menu text entry sanitizes/validates preset names:

1. The filesystem-reserved characters `< > : " / \ | ? *` are stripped out.
2. The result is truncated to 64 characters if longer.
3. The final name is checked (case-insensitively) against existing preset names; if it matches one, the command is a no-op.

Each step runs in that order on the *result* of the previous one, so a name that only collides with an existing preset after character-stripping or truncation is still rejected - e.g. `--save_preset="Mu:sic"` is a no-op if a preset named "Music" already exists, since stripping the `:` produces a match.

## Examples

Save the current modified settings as a new preset on a running instance:

```
fxsound.exe --save_preset="My Preset"
```

Overwrite the currently selected user preset with its unsaved changes:

```
fxsound.exe --overwrite_preset
```

Discard unsaved changes to the currently selected preset:

```
fxsound.exe --undo_preset
```

Rename the currently selected user preset:

```
fxsound.exe --rename_preset="New Name"
```

Delete the currently selected user preset:

```
fxsound.exe --delete_preset
```

Note: the preset name in `--save_preset` and `--rename_preset` is sanitized before use - see [Name sanitizing](#name-sanitizing).

Start FxSound minimized, with a specific preset and output device:

```
fxsound.exe --preset="Bass Booster" --output="Speakers (Realtek High Definition Audio)" --run_minimized
```

Switch a running instance to Pro view and turn power on:

```
fxsound.exe --view=2 --power=1
```

Tune the equalizer on a running instance:

```
fxsound.exe --num_bands=15 --filter_q=2.0 --master_gain=3.0 --volume_leveling=2.0
```

Set individual equalizer band frequencies and gains on a running instance:

```
fxsound.exe --set_band_freq="0:60,1:150" --set_band_gain="0:3.0,1:-2.5"
```

Play on the headphones only, then also add the speakers at half volume with a 40 ms delay:

```
fxsound.exe --output="Headphones (USB Audio)"
fxsound.exe --enable-output="Speakers (Realtek High Definition Audio)" --output-volume="Speakers (Realtek High Definition Audio):50" --output-delay="Speakers (Realtek High Definition Audio):40"
```

Set effect levels on a running instance:

```
fxsound.exe --set_effect="bass:7.5,ambience:4.0"
```

Change the display language:

```
fxsound.exe --language=fr
```

Print the running instance's current status as JSON:

```
fxsound.exe --status
```
