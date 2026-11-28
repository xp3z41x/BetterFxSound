package fxsound

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestFormatOutputValue(t *testing.T) {
	flags := map[string]string{
		"output-volume": FormatOutputValue("Speakers (Realtek)", 50),
		"output-delay":  FormatOutputValue("Dock: Line Out", 120),
	}
	args, err := BuildCommandLine(flags)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{`--output-delay="Dock: Line Out:120"`, `--output-volume="Speakers (Realtek):50"`}
	if !reflect.DeepEqual(args, want) {
		t.Fatalf("got %v, want %v", args, want)
	}
}

func TestOutputFlagsRejectQuotes(t *testing.T) {
	_, err := BuildCommandLine(map[string]string{"enable-output": `X" --power=0 --preset="Y`})
	if err == nil {
		t.Fatal("expected a quote in the device name to be rejected")
	}
}

func TestValidateOutput(t *testing.T) {
	for _, n := range []string{"", "  "} {
		if ValidateOutputName(n) == nil {
			t.Errorf("name %q should be rejected", n)
		}
	}
	if ValidateOutputName("Speakers") != nil {
		t.Error("valid name rejected")
	}
	for v, ok := range map[int]bool{-1: false, 0: true, 100: true, 101: false} {
		if (ValidateOutputVolume(v) == nil) != ok {
			t.Errorf("volume %d: ok=%v", v, ok)
		}
	}
	for v, ok := range map[int]bool{-1: false, 0: true, 500: true, 501: false} {
		if (ValidateOutputDelay(v) == nil) != ok {
			t.Errorf("delay %d: ok=%v", v, ok)
		}
	}
}

func TestStatusOutputsParse(t *testing.T) {
	const doc = `{"version":"1","power":true,
	"output_devices":["Speakers","Headphones"],
	"selected_output":"Headphones",
	"outputs":[
	 {"id":"{a}","name":"Speakers","enabled":false,"present":true,"volume":100,"delay_ms":0,"channel_mode":"both"},
	 {"id":"{b}","name":"Headphones","enabled":true,"present":true,"volume":40,"delay_ms":120,"channel_mode":"left"},
	 {"id":"{c}","name":"TV","enabled":true,"present":false,"volume":70,"delay_ms":5,"channel_mode":"mono"}]}`
	var s Status
	if err := json.Unmarshal([]byte(doc), &s); err != nil {
		t.Fatal(err)
	}
	if len(s.Outputs) != 3 || s.SelectedOutput != "Headphones" {
		t.Fatalf("unexpected status: %+v", s)
	}
	want := OutputInfo{ID: "{b}", Name: "Headphones", Enabled: true, Present: true, Volume: 40, DelayMs: 120, ChannelMode: "left"}
	if got := s.FindOutput("Headphones"); got == nil || *got != want {
		t.Fatalf("got %+v, want %+v", got, want)
	}
	if s.FindOutput("TV").Present {
		t.Error("TV should not be present")
	}
	if s.FindOutput("Nope") != nil {
		t.Error("unknown output should be nil")
	}

	// An old status.json without "outputs" still parses.
	var old Status
	if err := json.Unmarshal([]byte(`{"selected_output":"X"}`), &old); err != nil || len(old.Outputs) != 0 {
		t.Fatalf("old status: %v %+v", err, old)
	}
}

func TestResolveOutput(t *testing.T) {
	s := &Status{Outputs: []OutputInfo{
		{Name: "Speakers", Present: true},
		{Name: "TV", Enabled: true, Present: false},
	}}
	if o, err := s.ResolveOutput("Speakers", true); err != nil || len(o) != 1 || o[0].Name != "Speakers" {
		t.Fatalf("Speakers: %v", err)
	}
	if _, err := s.ResolveOutput("TV", false); err != nil {
		t.Fatalf("TV without present requirement: %v", err)
	}
	_, err := s.ResolveOutput("TV", true)
	if err == nil || !strings.Contains(err.Error(), "not connected") || !strings.Contains(err.Error(), `"Speakers"`) {
		t.Fatalf("TV needPresent: %v", err)
	}
	_, err = s.ResolveOutput("Nope", false)
	if err == nil || !strings.Contains(err.Error(), `"Speakers"`) || !strings.Contains(err.Error(), `"TV"`) {
		t.Fatalf("Nope: %v", err)
	}
}

func TestMatchOutputsSharedName(t *testing.T) {
	// Two identical monitors, the first disabled, plus an enabled entry of the
	// same name that is not connected (a device back under a new endpoint id).
	s := &Status{Outputs: []OutputInfo{
		{ID: "{m1}", Name: "Monitor", Enabled: false, Present: true},
		{ID: "{m2}", Name: "Monitor", Enabled: true, Present: true},
		{ID: "{old}", Name: "Monitor", Enabled: true, Present: false},
		{ID: "{s}", Name: "Speakers", Present: true},
	}}
	ids := func(os []OutputInfo) []string {
		var r []string
		for _, o := range os {
			r = append(r, o.ID)
		}
		return r
	}
	if got := ids(s.MatchOutputs("Monitor", false)); !reflect.DeepEqual(got, []string{"{m1}", "{m2}", "{old}"}) {
		t.Errorf("disable/volume/delay by name: %v", got)
	}
	if got := ids(s.MatchOutputs("Monitor", true)); !reflect.DeepEqual(got, []string{"{m1}", "{m2}"}) {
		t.Errorf("enable by name: %v", got)
	}
	if got := ids(s.MatchOutputs("{m2}", false)); !reflect.DeepEqual(got, []string{"{m2}"}) {
		t.Errorf("by id: %v", got)
	}
	if got := s.MatchOutputs("{old}", true); len(got) != 0 {
		t.Errorf("enable by id of a disconnected output: %v", got)
	}
	if o := s.FindOutput("{s}"); o == nil || o.Name != "Speakers" {
		t.Errorf("FindOutput by id: %+v", o)
	}
	_, err := s.ResolveOutput("{old}", true)
	if err == nil || !strings.Contains(err.Error(), "not connected") {
		t.Errorf("enable by id of a disconnected output: %v", err)
	}
}
