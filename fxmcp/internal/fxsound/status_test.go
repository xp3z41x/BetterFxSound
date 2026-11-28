package fxsound

import (
	"reflect"
	"testing"
)

func TestParseStatusEngine(t *testing.T) {
	data := []byte(`{
		"version": "1.0.2",
		"power": true,
		"outputs": [
			{"id": "{0.0.0.00000000}.{a}", "name": "Speakers", "enabled": true, "present": true, "volume": 100, "delay_ms": 0, "channel_mode": "both"},
			{"id": "{0.0.0.00000000}.{b}", "name": "Headphones", "enabled": true, "present": true, "volume": 80, "delay_ms": 20, "channel_mode": "mono"}
		],
		"selected_output": "Speakers",
		"engine": {
			"processing": true,
			"muted": false,
			"parked": false,
			"handed_back": false,
			"driven": true,
			"carrier_id": "{0.0.0.00000000}.{a}",
			"carrier_name": "Speakers",
			"carrier_enabled": true,
			"pending_carrier_id": "",
			"failed_carrier_id": "",
			"outputs": [
				{"id": "{0.0.0.00000000}.{b}", "name": "Headphones", "started": true, "open": false, "streaming": false,
				 "primed": false, "failed": true, "last_error": "0x88890004", "volume_taken": false, "device_rate": 0,
				 "queue_ms": 0, "target_ms": 0, "opens": 3, "open_failures": 7, "stream_failures": 2, "underflows": 4,
				 "reprimes": 1, "realigns": 5, "stall_jumps": 1, "overflows": 4294967295}
			]
		}
	}`)

	s, err := parseStatus(data)
	if err != nil {
		t.Fatal(err)
	}
	if len(s.Outputs) != 2 || s.SelectedOutput != "Speakers" {
		t.Fatalf("settings outputs not parsed: %+v", s)
	}
	if s.Engine == nil {
		t.Fatal("engine object not parsed")
	}
	e := s.Engine
	if !e.Processing || !e.Driven || !e.CarrierEnabled || e.Muted || e.Parked || e.HandedBack {
		t.Fatalf("engine flags: %+v", e)
	}
	if e.CarrierID != "{0.0.0.00000000}.{a}" || e.CarrierName != "Speakers" || e.PendingCarrierID != "" || e.FailedCarrierID != "" {
		t.Fatalf("engine carrier: %+v", e)
	}
	want := []EngineOutput{{
		ID: "{0.0.0.00000000}.{b}", Name: "Headphones", Started: true, Failed: true, LastError: "0x88890004",
		Opens: 3, OpenFailures: 7, StreamFailures: 2, Underflows: 4, Reprimes: 1, Realigns: 5, StallJumps: 1,
		Overflows: 4294967295,
	}}
	if !reflect.DeepEqual(e.Outputs, want) {
		t.Fatalf("engine outputs:\n got %+v\nwant %+v", e.Outputs, want)
	}
}

// An older FxSound, or one whose engine is not initialized, writes no engine
// object: Engine stays nil and the rest still parses.
func TestParseStatusWithoutEngine(t *testing.T) {
	s, err := parseStatus([]byte(`{"version": "1.0.1", "power": false, "outputs": [], "selected_output": ""}`))
	if err != nil {
		t.Fatal(err)
	}
	if s.Engine != nil {
		t.Fatalf("Engine = %+v, want nil", s.Engine)
	}
	if s.Version != "1.0.1" {
		t.Fatalf("Version = %q", s.Version)
	}
}

func TestParseStatusInvalid(t *testing.T) {
	if _, err := parseStatus([]byte(`{"engine": [`)); err == nil {
		t.Fatal("expected an error for malformed JSON")
	}
}
