package fxsound

import (
	"fmt"
	"strings"
)

// Valid ranges of the per-output options (--output-volume, --output-delay).
const (
	MaxOutputVolume  = 100
	MaxOutputDelayMs = 500
)

// FormatOutputValue renders the "<device name>:<number>" value of
// --output-volume and --output-delay. FxSound splits it on the last colon,
// so a device name containing ':' is fine.
func FormatOutputValue(deviceName string, value int) string {
	return fmt.Sprintf("%s:%d", deviceName, value)
}

// ValidateOutputName rejects an empty or blank device name.
func ValidateOutputName(deviceName string) error {
	if strings.TrimSpace(deviceName) == "" {
		return newError(ErrKindValueRejected, nil, "device_name must not be empty")
	}
	return nil
}

// ValidateOutputVolume checks a per-output volume trim, in percent.
func ValidateOutputVolume(v int) error {
	if v < 0 || v > MaxOutputVolume {
		return newError(ErrKindValueRejected, nil, "volume %d is out of range (0 to %d)", v, MaxOutputVolume)
	}
	return nil
}

// ValidateOutputDelay checks a per-output delay, in milliseconds.
func ValidateOutputDelay(ms int) error {
	if ms < 0 || ms > MaxOutputDelayMs {
		return newError(ErrKindValueRejected, nil, "delay_ms %d is out of range (0 to %d)", ms, MaxOutputDelayMs)
	}
	return nil
}

// MatchOutputs returns the outputs a per-output command-line option acts on,
// as FxSound picks them: an endpoint id (an output's ID) picks exactly that
// output; otherwise every output with this exact friendly name is picked.
// connectedOnly (enabling) keeps only the connected ones; disabling, volume
// and delay act on the disconnected same-named outputs too.
func (s *Status) MatchOutputs(key string, connectedOnly bool) []OutputInfo {
	for _, o := range s.Outputs {
		if o.ID != "" && o.ID == key {
			if connectedOnly && !o.Present {
				return nil
			}
			return []OutputInfo{o}
		}
	}
	var matches []OutputInfo
	for _, o := range s.Outputs {
		if o.Name == key && (o.Present || !connectedOnly) {
			matches = append(matches, o)
		}
	}
	return matches
}

// FindOutput returns the output with this endpoint id, else the first one
// with this exact friendly name, or nil.
func (s *Status) FindOutput(key string) *OutputInfo {
	for i := range s.Outputs {
		if s.Outputs[i].ID != "" && s.Outputs[i].ID == key {
			return &s.Outputs[i]
		}
	}
	for i := range s.Outputs {
		if s.Outputs[i].Name == key {
			return &s.Outputs[i]
		}
	}
	return nil
}

// ResolveOutput finds the outputs an option naming key acts on (see
// MatchOutputs). If none matches (or needPresent is set and none of the
// matches is connected), it returns an error listing the outputs that are
// available.
func (s *Status) ResolveOutput(key string, needPresent bool) ([]OutputInfo, error) {
	if matches := s.MatchOutputs(key, needPresent); len(matches) > 0 {
		return matches, nil
	}
	var names []string
	for _, x := range s.Outputs {
		if x.Present || !needPresent {
			names = append(names, fmt.Sprintf("%q", x.Name))
		}
	}
	avail := "none"
	if len(names) > 0 {
		avail = strings.Join(names, ", ")
	}
	if s.FindOutput(key) != nil {
		return nil, newError(ErrKindValueRejected, nil, "output %q is not connected; available outputs: %s", key, avail)
	}
	return nil, newError(ErrKindValueRejected, nil, "no output named %q; available outputs: %s", key, avail)
}
