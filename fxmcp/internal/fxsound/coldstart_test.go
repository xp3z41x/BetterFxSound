package fxsound

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"
)

func stub[T any](t *testing.T, p *T, v T) {
	t.Helper()
	old := *p
	*p = v
	t.Cleanup(func() { *p = old })
}

// fakeApp simulates FxSound.exe: launching it makes it "running", and a
// "--status" run while it is running writes status.json.
type fakeApp struct {
	mu       sync.Mutex
	running  bool
	launched [][]string
	ran      [][]string
}

func newFakeApp(t *testing.T, running bool) *fakeApp {
	t.Helper()
	appData := t.TempDir()
	t.Setenv("APPDATA", appData)
	if err := os.MkdirAll(filepath.Join(appData, "FxSound"), 0o755); err != nil {
		t.Fatal(err)
	}
	f := &fakeApp{running: running}
	stub(t, &fxSoundRunning, func() (bool, error) {
		f.mu.Lock()
		defer f.mu.Unlock()
		return f.running, nil
	})
	stub(t, &startProcess, func(_ string, args []string) error {
		f.mu.Lock()
		defer f.mu.Unlock()
		f.launched = append(f.launched, append([]string{}, args...))
		f.running = true
		return nil
	})
	stub(t, &runProcess, func(_ context.Context, _ string, args []string, _, _ *bytes.Buffer) error {
		f.mu.Lock()
		f.ran = append(f.ran, append([]string{}, args...))
		running := f.running
		f.mu.Unlock()
		if running && len(args) == 1 && args[0] == "--status" {
			return os.WriteFile(filepath.Join(appData, "FxSound", "status.json"), []byte(`{"power":true}`), 0o644)
		}
		return nil
	})
	return f
}

var fakePaths = &Paths{FxSoundExe: `C:\fake\FxSound.exe`, FxDiagExe: `C:\fake\fxdiag.exe`}

// A cold start must launch FxSound with the flags as its startup command
// line and leave it running, never run it through Run (which waits for the
// process to exit and kills it at the timeout).
func TestApplyColdStartLaunchesWithoutWaiting(t *testing.T) {
	app := newFakeApp(t, false)
	if err := Apply(context.Background(), fakePaths, map[string]string{"power": "1"}, false); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if want := [][]string{{"--power=1"}}; !reflect.DeepEqual(app.launched, want) {
		t.Errorf("launched %v, want %v", app.launched, want)
	}
	for _, args := range app.ran {
		if !reflect.DeepEqual(args, []string{"--status"}) {
			t.Errorf("Run was used for %v on a cold start; only --status readiness checks may go through Run", args)
		}
	}
	if len(app.ran) == 0 {
		t.Error("no readiness check after the cold start")
	}
}

func TestApplyRunningInstanceForwards(t *testing.T) {
	app := newFakeApp(t, true)
	if err := Apply(context.Background(), fakePaths, map[string]string{"power": "0"}, false); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if len(app.launched) != 0 {
		t.Errorf("launched %v while an instance was running", app.launched)
	}
	if want := [][]string{{"--power=0"}}; !reflect.DeepEqual(app.ran, want) {
		t.Errorf("ran %v, want %v", app.ran, want)
	}
}

func TestApplyRejectsBadValueBeforeLaunching(t *testing.T) {
	app := newFakeApp(t, false)
	err := Apply(context.Background(), fakePaths, map[string]string{"enable-output": `X" --power=0`}, true)
	if err == nil {
		t.Fatal("expected the quote to be rejected")
	}
	if len(app.launched) != 0 || len(app.ran) != 0 {
		t.Errorf("FxSound was started (%v) or run (%v) for a rejected value", app.launched, app.ran)
	}
}

// Reading the status of a stopped app must not start it: "--status" would
// become the new instance and be killed by Run's timeout.
func TestReadStatusDoesNotStartTheApp(t *testing.T) {
	app := newFakeApp(t, false)
	_, err := ReadStatus(context.Background(), fakePaths)
	var fxErr *Error
	if !errors.As(err, &fxErr) || fxErr.Kind != ErrKindRequiresRunningInstance {
		t.Fatalf("got %v, want a requires_running_instance error", err)
	}
	if len(app.launched) != 0 || len(app.ran) != 0 {
		t.Errorf("FxSound was started (%v) or run (%v)", app.launched, app.ran)
	}
}

func TestEnsureRunningLaunchesWithoutArgs(t *testing.T) {
	app := newFakeApp(t, false)
	if err := EnsureRunning(context.Background(), fakePaths); err != nil {
		t.Fatalf("EnsureRunning: %v", err)
	}
	if want := [][]string{{}}; !reflect.DeepEqual(app.launched, want) {
		t.Errorf("launched %v, want %v", app.launched, want)
	}
}
