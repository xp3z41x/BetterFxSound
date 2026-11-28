package mcpserver

import (
	"context"
	"fmt"
	"strings"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"fxmcp/internal/fxsound"
)

// applyResult is the common output shape for every write tool in this
// package: a settings-change either applied and returned a short
// human-readable detail, or the call returned an error explaining why it
// didn't (a precondition FxController.cpp would otherwise have silently
// no-op'd on).
type applyResult struct {
	Applied bool   `json:"applied"`
	Detail  string `json:"detail,omitempty"`
}

func boolFlag(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

func (a *App) registerWriteTools(s *mcp.Server) {
	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_set_power",
		Description: "Turns FxSound's audio processing (DFX) on or off. Works whether or not FxSound is " +
			"currently running -- if it isn't, this launches it with power set accordingly.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSetPower)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_select_preset",
		Description: "Selects the active preset by name (case-sensitive, must match an existing preset " +
			"exactly -- see fxsound://presets for valid names). Only takes effect if power is on; works " +
			"whether or not FxSound is currently running.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSelectPreset)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_set_output_device",
		Description: "Enables only this output and disables every other one, by the output's exact Windows " +
			"friendly name (see the outputs list in fxsound://status, or fxsound://diagnostics's device list, " +
			"for valid names). The device must be connected. To change one output without touching the " +
			"others use fxsound_enable_output / fxsound_disable_output. Works whether or not FxSound is " +
			"currently running (when it is not, it is started with this output; an unknown name is then " +
			"ignored instead of reported).",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSetOutputDevice)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_enable_output",
		Description: "Enables one output (by exact Windows friendly name, or by its id from the outputs " +
			"list in fxsound://status) so FxSound also plays on it, leaving the other outputs as they are. " +
			"A name shared by several connected outputs enables all of them; pass the id to pick one. The " +
			"device must be connected. Requires FxSound to be running.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolEnableOutput)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_disable_output",
		Description: "Disables one output (by exact Windows friendly name, or by its id from the outputs " +
			"list in fxsound://status), leaving the other outputs as they are. A name shared by several " +
			"outputs disables all of them; pass the id to pick one. With every output disabled FxSound " +
			"plays silence. Requires FxSound to be running.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolDisableOutput)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_set_output_volume",
		Description: "Sets one output's volume trim, 0-100 percent, on top of the Windows master volume " +
			"(which applies to every output). Output is chosen by exact Windows friendly name (every output " +
			"with that name) or by its id from fxsound://status. Requires FxSound to be running.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSetOutputVolume)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_set_output_delay",
		Description: "Sets one output's delay, 0-500 milliseconds, e.g. to line up speakers with " +
			"Bluetooth headphones. Output is chosen by exact Windows friendly name (every output with that " +
			"name) or by its id from fxsound://status. Requires FxSound to be running.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSetOutputDelay)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_save_preset",
		Description: "Saves the current modified settings as a new user preset and selects it. Requires " +
			"FxSound to be running, power to be on, and the current preset to have unsaved changes (check " +
			"fxsound://presets). The name is sanitized (filesystem-reserved characters < > : \" / \\ | ? * " +
			"stripped, truncated to 64 characters) and rejected if that collides with an existing preset name.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolSavePreset)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_overwrite_preset",
		Description: "Overwrites the currently selected user preset with its unsaved changes. Requires " +
			"FxSound to be running, power to be on, the selected preset to be a user preset (not built-in), " +
			"and to have unsaved changes. This permanently discards the preset's previously saved content.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(true)},
	}, a.toolOverwritePreset)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_undo_preset",
		Description: "Reverts the currently selected preset's unsaved changes. Requires FxSound to be " +
			"running, power to be on, and the preset to have unsaved changes.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolUndoPreset)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_rename_preset",
		Description: "Renames the currently selected user preset. Requires FxSound to be running, power to " +
			"be on, the selected preset to be a user preset (not built-in) with no unsaved changes. The name " +
			"is sanitized and collision-checked the same way as fxsound_save_preset.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(false)},
	}, a.toolRenamePreset)

	mcp.AddTool(s, &mcp.Tool{
		Name: "fxsound_delete_preset",
		Description: "Deletes the currently selected user preset. Requires FxSound to be running, power to " +
			"be on, and the selected preset to be a user preset (not built-in). This is irreversible.",
		Annotations: &mcp.ToolAnnotations{DestructiveHint: boolPtr(true)},
	}, a.toolDeletePreset)
}

type setPowerInput struct {
	On bool `json:"on" jsonschema:"true to turn FxSound's audio processing on, false to turn it off"`
}

func (a *App) toolSetPower(ctx context.Context, _ *mcp.CallToolRequest, in setPowerInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"power": boolFlag(in.On)}, false); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("power set to %v", in.On)}, nil
}

type selectPresetInput struct {
	Name string `json:"name" jsonschema:"the preset name to select, case-sensitive"`
}

func (a *App) toolSelectPreset(ctx context.Context, _ *mcp.CallToolRequest, in selectPresetInput) (*mcp.CallToolResult, applyResult, error) {
	if in.Name == "" {
		return nil, applyResult{}, fmt.Errorf("name must not be empty")
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"preset": in.Name}, false); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("preset selected: %s", in.Name)}, nil
}

type setOutputDeviceInput struct {
	DeviceName string `json:"device_name" jsonschema:"the output device's exact Windows friendly name, or its id from fxsound://status"`
}

func (a *App) toolSetOutputDevice(ctx context.Context, _ *mcp.CallToolRequest, in setOutputDeviceInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.ValidateOutputName(in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	if err := a.checkOutputIfRunning(ctx, in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"output": in.DeviceName}, false); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("only output enabled: %s", in.DeviceName)}, nil
}

type outputNameInput struct {
	DeviceName string `json:"device_name" jsonschema:"the output's exact Windows friendly name, or its id from fxsound://status"`
}

func (a *App) toolEnableOutput(ctx context.Context, _ *mcp.CallToolRequest, in outputNameInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.ValidateOutputName(in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	targets, err := a.checkOutput(ctx, in.DeviceName, true)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"enable-output": in.DeviceName}, true); err != nil {
		return nil, applyResult{}, err
	}
	return a.confirmOutputs(ctx, targets, fmt.Sprintf("output enabled: %s", in.DeviceName),
		func(o *fxsound.OutputInfo) bool { return o != nil && o.Enabled })
}

func (a *App) toolDisableOutput(ctx context.Context, _ *mcp.CallToolRequest, in outputNameInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.ValidateOutputName(in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	targets, err := a.checkOutput(ctx, in.DeviceName, false)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"disable-output": in.DeviceName}, true); err != nil {
		return nil, applyResult{}, err
	}
	// A disabled output that is not connected leaves the outputs list.
	return a.confirmOutputs(ctx, targets, fmt.Sprintf("output disabled: %s", in.DeviceName),
		func(o *fxsound.OutputInfo) bool { return o == nil || !o.Enabled })
}

type setOutputVolumeInput struct {
	DeviceName string `json:"device_name" jsonschema:"the output's exact Windows friendly name, or its id from fxsound://status"`
	Volume     int    `json:"volume" jsonschema:"volume trim in percent, 0 to 100"`
}

func (a *App) toolSetOutputVolume(ctx context.Context, _ *mcp.CallToolRequest, in setOutputVolumeInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.ValidateOutputName(in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateOutputVolume(in.Volume); err != nil {
		return nil, applyResult{}, err
	}
	targets, err := a.checkOutput(ctx, in.DeviceName, false)
	if err != nil {
		return nil, applyResult{}, err
	}
	flags := map[string]string{"output-volume": fxsound.FormatOutputValue(in.DeviceName, in.Volume)}
	if err := fxsound.Apply(ctx, a.Paths, flags, true); err != nil {
		return nil, applyResult{}, err
	}
	return a.confirmOutputs(ctx, targets, fmt.Sprintf("output %s volume set to %d%%", in.DeviceName, in.Volume),
		func(o *fxsound.OutputInfo) bool { return o != nil && o.Volume == in.Volume })
}

type setOutputDelayInput struct {
	DeviceName string `json:"device_name" jsonschema:"the output's exact Windows friendly name, or its id from fxsound://status"`
	DelayMs    int    `json:"delay_ms" jsonschema:"delay in milliseconds, 0 to 500"`
}

func (a *App) toolSetOutputDelay(ctx context.Context, _ *mcp.CallToolRequest, in setOutputDelayInput) (*mcp.CallToolResult, applyResult, error) {
	if err := fxsound.ValidateOutputName(in.DeviceName); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateOutputDelay(in.DelayMs); err != nil {
		return nil, applyResult{}, err
	}
	targets, err := a.checkOutput(ctx, in.DeviceName, false)
	if err != nil {
		return nil, applyResult{}, err
	}
	flags := map[string]string{"output-delay": fxsound.FormatOutputValue(in.DeviceName, in.DelayMs)}
	if err := fxsound.Apply(ctx, a.Paths, flags, true); err != nil {
		return nil, applyResult{}, err
	}
	return a.confirmOutputs(ctx, targets, fmt.Sprintf("output %s delay set to %d ms", in.DeviceName, in.DelayMs),
		func(o *fxsound.OutputInfo) bool { return o != nil && o.DelayMs == in.DelayMs })
}

type presetNameInput struct {
	Name string `json:"name" jsonschema:"the preset name"`
}

func (a *App) toolSavePreset(ctx context.Context, _ *mcp.CallToolRequest, in presetNameInput) (*mcp.CallToolResult, applyResult, error) {
	status, err := readyStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateSavePreset(status); err != nil {
		return nil, applyResult{}, err
	}
	sanitized, err := fxsound.ValidatePresetName(in.Name, status)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"save_preset": sanitized}, true); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("saved and selected new preset: %s", sanitized)}, nil
}

func (a *App) toolOverwritePreset(ctx context.Context, _ *mcp.CallToolRequest, _ any) (*mcp.CallToolResult, applyResult, error) {
	status, err := readyStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateOverwritePreset(status); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"overwrite_preset": ""}, true); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("overwrote preset: %s", status.SelectedPreset)}, nil
}

func (a *App) toolUndoPreset(ctx context.Context, _ *mcp.CallToolRequest, _ any) (*mcp.CallToolResult, applyResult, error) {
	status, err := readyStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateUndoPreset(status); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"undo_preset": ""}, true); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("undid unsaved changes to preset: %s", status.SelectedPreset)}, nil
}

func (a *App) toolRenamePreset(ctx context.Context, _ *mcp.CallToolRequest, in presetNameInput) (*mcp.CallToolResult, applyResult, error) {
	status, err := readyStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateRenamePreset(status); err != nil {
		return nil, applyResult{}, err
	}
	sanitized, err := fxsound.ValidatePresetName(in.Name, status)
	if err != nil {
		return nil, applyResult{}, err
	}
	oldName := status.SelectedPreset
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"rename_preset": sanitized}, true); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("renamed preset %q to %q", oldName, sanitized)}, nil
}

func (a *App) toolDeletePreset(ctx context.Context, _ *mcp.CallToolRequest, _ any) (*mcp.CallToolResult, applyResult, error) {
	status, err := readyStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.ValidateDeletePreset(status); err != nil {
		return nil, applyResult{}, err
	}
	if err := fxsound.Apply(ctx, a.Paths, map[string]string{"delete_preset": ""}, true); err != nil {
		return nil, applyResult{}, err
	}
	return nil, applyResult{Applied: true, Detail: fmt.Sprintf("deleted preset: %s", status.SelectedPreset)}, nil
}

// readyStatus ensures a live FxSound instance is up (launching one if
// needed) and returns its current status, for the preset-management tools
// that need to validate preconditions (power, modified state, built-in
// vs. user preset) before building the real command.
func readyStatus(ctx context.Context, paths *fxsound.Paths) (*fxsound.Status, error) {
	if err := fxsound.EnsureRunning(ctx, paths); err != nil {
		return nil, err
	}
	status, err := fxsound.ReadStatus(ctx, paths)
	if err != nil {
		return nil, fmt.Errorf("read current state: %w", err)
	}
	return status, nil
}

// checkOutputIfRunning reports an unknown or disconnected output when FxSound
// is running (it would silently ignore --output for it). On a cold start the
// name is passed through unchecked: the outputs are only known once it runs.
func (a *App) checkOutputIfRunning(ctx context.Context, name string) error {
	running, err := fxsound.IsFxSoundRunning()
	if err != nil {
		return fmt.Errorf("check running state: %w", err)
	}
	if !running {
		return nil
	}
	_, err = a.checkOutput(ctx, name, true)
	return err
}

// checkOutput makes sure FxSound is running and knows an output with this
// exact name (or endpoint id), so a typo is reported (with the available
// outputs) instead of being silently ignored by FxSound. needPresent also
// requires the device to be connected, as enabling does. It returns the
// outputs the option will act on.
func (a *App) checkOutput(ctx context.Context, name string, needPresent bool) ([]fxsound.OutputInfo, error) {
	if err := fxsound.EnsureRunning(ctx, a.Paths); err != nil {
		return nil, err
	}
	status, err := fxsound.ReadStatus(ctx, a.Paths)
	if err != nil {
		return nil, err
	}
	return status.ResolveOutput(name, needPresent)
}

// confirmOutputs reads the state back after a per-output option was applied
// and reports Applied only if every targeted output (matched by id; nil when
// it is no longer listed) now satisfies done. FxSound handles the forwarded
// option before the --status that follows it, so the fresh status shows its
// effect, and a no-op is reported instead of claimed as applied.
func (a *App) confirmOutputs(ctx context.Context, targets []fxsound.OutputInfo, detail string, done func(o *fxsound.OutputInfo) bool) (*mcp.CallToolResult, applyResult, error) {
	status, err := fxsound.ReadStatus(ctx, a.Paths)
	if err != nil {
		return nil, applyResult{}, fmt.Errorf("read state after the change: %w", err)
	}
	var missed []string
	for _, t := range targets {
		var current *fxsound.OutputInfo
		for i := range status.Outputs {
			if status.Outputs[i].ID == t.ID {
				current = &status.Outputs[i]
				break
			}
		}
		if !done(current) {
			missed = append(missed, fmt.Sprintf("%q (id %s)", t.Name, t.ID))
		}
	}
	if len(missed) > 0 {
		return nil, applyResult{Applied: false, Detail: "FxSound did not apply the change to " + strings.Join(missed, ", ")}, nil
	}
	if len(targets) > 1 {
		detail += fmt.Sprintf(" (%d outputs with that name)", len(targets))
	}
	return nil, applyResult{Applied: true, Detail: detail}, nil
}
