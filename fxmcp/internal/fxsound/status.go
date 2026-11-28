package fxsound

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

const (
	statusTimeout   = 5 * time.Second
	statusPollTotal = 2 * time.Second
	statusPollTick  = 100 * time.Millisecond
)

// EqBand is a single equalizer band, as reported by --status.
type EqBand struct {
	Index     int     `json:"index"`
	Frequency float64 `json:"frequency"`
	Gain      float64 `json:"gain"`
}

// PresetInfo is a single preset entry, as reported by --status.
type PresetInfo struct {
	Name     string `json:"name"`
	Modified bool   `json:"modified"`
}

// Presets groups the built-in and user-defined preset lists reported by
// --status.
type Presets struct {
	BuiltIn     []PresetInfo `json:"built_in"`
	UserDefined []PresetInfo `json:"user_defined"`
}

// Equalizer is the equalizer section of --status's output.
type Equalizer struct {
	NumBands       int      `json:"num_bands"`
	MasterGain     float64  `json:"master_gain"`
	VolumeLeveling float64  `json:"volume_leveling"`
	FilterQ        float64  `json:"filter_q"`
	Balance        float64  `json:"balance"`
	Bands          []EqBand `json:"bands"`
}

// Effects is the effects section of --status's output. Values are on a
// 0-10 scale (matching --set_effect's input range), not the raw 0-1 scale
// FxController stores internally.
type Effects struct {
	Clarity      float64 `json:"clarity"`
	Ambience     float64 `json:"ambience"`
	Surround     float64 `json:"surround"`
	DynamicBoost float64 `json:"dynamicboost"`
	Bass         float64 `json:"bass"`
}

// OutputInfo is one audio output, as reported by --status. Every output can
// be enabled or disabled independently; with none enabled FxSound plays
// silence. Present is false for an enabled output whose device is not
// connected right now. Volume is a 0-100 trim on top of the Windows master
// volume, DelayMs is 0-500, ChannelMode is one of "both", "left", "right",
// "mono".
type OutputInfo struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Enabled     bool   `json:"enabled"`
	Present     bool   `json:"present"`
	Volume      int    `json:"volume"`
	DelayMs     int    `json:"delay_ms"`
	ChannelMode string `json:"channel_mode"`
}

// EngineOutput is the audio engine's runtime state of one output played
// next to the carrier (the "outputs" of status.json's "engine" object).
// LastError is the HRESULT of the last failed open or render, as "0x" and 8
// hex digits ("0x00000000" once a stream opened). The counters count since
// the output was added and wrap around.
type EngineOutput struct {
	ID             string `json:"id"`
	Name           string `json:"name"`
	Started        bool   `json:"started"`
	Open           bool   `json:"open"`
	Streaming      bool   `json:"streaming"`
	Primed         bool   `json:"primed"`
	Failed         bool   `json:"failed"`
	LastError      string `json:"last_error"`
	VolumeTaken    bool   `json:"volume_taken"`
	DeviceRate     uint32 `json:"device_rate"`
	QueueMs        int    `json:"queue_ms"`
	TargetMs       int    `json:"target_ms"`
	Opens          uint32 `json:"opens"`
	OpenFailures   uint32 `json:"open_failures"`
	StreamFailures uint32 `json:"stream_failures"`
	Underflows     uint32 `json:"underflows"`
	Reprimes       uint32 `json:"reprimes"`
	Realigns       uint32 `json:"realigns"`
	StallJumps     uint32 `json:"stall_jumps"`
	Overflows      uint32 `json:"overflows"`
}

// Engine is what the audio engine actually does with the outputs
// (status.json's "engine" object), as opposed to their settings (Outputs):
// the carrier in use on the low-latency path, and every other output it
// plays. Absent while the engine is not initialized.
type Engine struct {
	Processing       bool           `json:"processing"`
	Muted            bool           `json:"muted"`
	Parked           bool           `json:"parked"`
	HandedBack       bool           `json:"handed_back"`
	Driven           bool           `json:"driven"`
	CarrierID        string         `json:"carrier_id"`
	CarrierName      string         `json:"carrier_name"`
	CarrierEnabled   bool           `json:"carrier_enabled"`
	PendingCarrierID string         `json:"pending_carrier_id"`
	FailedCarrierID  string         `json:"failed_carrier_id"`
	Outputs          []EngineOutput `json:"outputs"`
}

// Status is the shape of %APPDATA%\FxSound\status.json, written by
// FxController::printStatus (the --status command-line option).
type Status struct {
	Version        string       `json:"version"`
	Power          bool         `json:"power"`
	Presets        Presets      `json:"presets"`
	SelectedPreset string       `json:"selected_preset"`
	OutputDevices  []string     `json:"output_devices"`
	Outputs        []OutputInfo `json:"outputs"`
	// SelectedOutput is the first enabled output, empty if none is enabled.
	// Kept for compatibility; use Outputs.
	SelectedOutput string    `json:"selected_output"`
	Equalizer      Equalizer `json:"equalizer"`
	Effects        Effects   `json:"effects"`
	// Engine is the audio engine's runtime state of the outputs; nil when
	// status.json has none (engine not initialized, or an older FxSound).
	Engine *Engine `json:"engine,omitempty"`
}

// parseStatus decodes the contents of status.json.
func parseStatus(data []byte) (*Status, error) {
	var s Status
	if err := json.Unmarshal(data, &s); err != nil {
		return nil, fmt.Errorf("parse status.json: %w", err)
	}
	return &s, nil
}

func statusFilePath() (string, error) {
	appData := os.Getenv("APPDATA")
	if appData == "" {
		return "", newError(ErrKindUnknown, nil, "%%APPDATA%% not set")
	}
	return filepath.Join(appData, "FxSound", "status.json"), nil
}

// ReadStatus spawns "FxSound.exe --status" and reads back the resulting
// %APPDATA%\FxSound\status.json.
//
// --status requires an already-running instance (see IsFxSoundRunning); a
// cold-start invocation silently no-ops, in which case this may read a
// stale or absent file. There is no synchronous completion signal for the
// write, so this polls for the file's mtime to advance past the moment of
// invocation, bounded by statusPollTotal.
func ReadStatus(ctx context.Context, paths *Paths) (*Status, error) {
	path, err := statusFilePath()
	if err != nil {
		return nil, err
	}

	// Without a running instance, "FxSound.exe --status" would itself become
	// the new instance (it never exits, so Run would kill it at its timeout,
	// midway through its startup). Report that the app is not running instead.
	running, err := fxSoundRunning()
	if err != nil {
		return nil, fmt.Errorf("check running state: %w", err)
	}
	if !running {
		return nil, newError(ErrKindRequiresRunningInstance, nil, "FxSound is not running (start it first, e.g. with fxsound_set_power)")
	}

	var before time.Time
	if info, statErr := os.Stat(path); statErr == nil {
		before = info.ModTime()
	}

	if _, err := Run(ctx, statusTimeout, paths.FxSoundExe, "--status"); err != nil {
		return nil, err
	}

	deadline := time.Now().Add(statusPollTotal)
	var lastErr error
	for time.Now().Before(deadline) {
		info, statErr := os.Stat(path)
		if statErr == nil && info.ModTime().After(before) {
			data, readErr := os.ReadFile(path)
			if readErr != nil {
				lastErr = readErr
			} else {
				return parseStatus(data)
			}
		} else if statErr != nil {
			lastErr = statErr
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(statusPollTick):
		}
	}
	if lastErr != nil {
		return nil, newError(ErrKindRequiresRunningInstance, lastErr, "status.json not updated after --status within %s", statusPollTotal)
	}
	return nil, newError(ErrKindRequiresRunningInstance, nil, "status.json not updated after --status within %s (is FxSound running?)", statusPollTotal)
}
