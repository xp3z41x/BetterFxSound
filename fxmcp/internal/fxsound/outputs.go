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

// FindOutput returns the output with this exact friendly name, or nil.
func (s *Status) FindOutput(name string) *OutputInfo {
	for i := range s.Outputs {
		if s.Outputs[i].Name == name {
			return &s.Outputs[i]
		}
	}
	return nil
}

// ResolveOutput finds the output with this exact friendly name. If none
// matches (or needPresent is set and the match is not connected), it returns
// an error listing the outputs that are available.
func (s *Status) ResolveOutput(name string, needPresent bool) (*OutputInfo, error) {
	o := s.FindOutput(name)
	if o != nil && (o.Present || !needPresent) {
		return o, nil
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
	if o != nil {
		return nil, newError(ErrKindValueRejected, nil, "output %q is not connected; available outputs: %s", name, avail)
	}
	return nil, newError(ErrKindValueRejected, nil, "no output named %q; available outputs: %s", name, avail)
}
