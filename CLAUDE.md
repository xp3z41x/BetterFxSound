# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

FxSound is a digital audio enhancement application for Windows. It acts as a
system-wide virtual soundcard, providing clean audio passthrough plus DSP-based
effects for volume, timbre, and equalization.

## Architecture

The core application has three components, each with a different risk profile:

1. **FxSound GUI** (`fxsound/`) — the JUCE-based application (`FxSound_App`).
   Most contributions land here. Bugs are usually cosmetic, UX-related, or
   preset/settings handling. `FxController` (`fxsound/Source/GUI/FxController.*`)
   is the hub: it owns the `DfxDsp` instance, holds the `AudioPassthru` pointer,
   receives device-change callbacks (`AudioPassthruCallback::onSoundDeviceChange`),
   applies command-line options (`applyConfig`), and writes
   `%APPDATA%\FxSound\status.json`. `FxModel` holds UI state; views (`FxProView`,
   `FxLiteView`, `FxSystemTrayView`) and widgets live alongside it in `Source/GUI/`.
   Settings persistence is in `Source/Utils/Settings/`.
2. **Audiopassthru** (`audiopassthru/`, static lib) — interfaces with the FxSound
   Audio Enhancer virtual audio driver and runs the audio processing thread,
   which calls `DfxDsp::processAudio` on each buffer
   (`src/AudioPassthru/AudioPassthruPrivate.cpp`). **Treat changes here with
   extra scrutiny** — bugs can affect system audio for all users, not just this
   app, and issues here are harder for users to self-diagnose or roll back.
3. **DfxDsp** (`dsp/`, static lib) — the DSP engine that processes audio in real
   time. Public API in `dsp/include/DfxDsp.h` (pimpl over `DfxDspPrivate`);
   effect stages live under `dsp/ptechDsp/` (Aural = Clarity, Lex = Ambience,
   wide = Surround, Maximizer = Dynamic Boost, Play = effects chain/Bass Boost,
   Peq = equalizer). Engine state is persisted in the registry under
   `HKEY_CURRENT_USER\SOFTWARE\DFX`. Changes here affect audio quality and
   performance directly; watch for real-time-safety issues (allocations, locks,
   or blocking calls in the audio callback path) and numerical correctness in
   filter/effect code.

Supporting components:

- **fxdiag** (`fxdiag/`) — standalone C++ diagnostics tool (`fxdiag.exe`) that
  dumps audio device/session state (`--json` writes `fxdiag.json`). Its
  post-build step copies the exe to `bin\<platform>\`.
- **fxmcp** (`fxmcp/`) — Go MCP server that exposes fxdiag output and
  `FxSound.exe`'s command-line options as MCP tools/resources. It drives a
  running FxSound via those CLI options and reads `status.json`; see
  `fxmcp/README.md` and `docs/MCP_SERVER.md`.
- **DSP tutorial** (`web/dsp-tutorial/`) — static web page whose `js/dsp/` is a
  JavaScript port of `dsp/` that must stay bit-identical to the C engine. Any
  behavior change in `dsp/` should be mirrored there (see `docs/DSP_TUTORIAL.md`).
  Published to GitHub Pages by `.github/workflows/publish-dsp-tutorial.yml` on
  pushes to `main`.
- `bin/<arch>/` — runtime folder (presets, built exes); `Installer/` —
  Advanced Installer projects; `Resources/` — git submodule.

`docs/COMMAND_LINE_OPTIONS.md` is the canonical reference for `FxSound.exe`
command-line options (parsed in `FxController::applyConfig`). If you change
the options, update that doc and its mirrored copy in fxmcp (below).

## Build and test

All native builds are Windows-only (Visual Studio 2026, toolset v145, Windows
SDK, latest JUCE). Nothing here builds on Linux.

- **App**: open `fxsound/Project/FxSound.sln` in Visual Studio and build the
  desired configuration/platform (x64, x86, ARM64).
- **Running locally** requires FxSound's virtual audio driver, installed via the
  normal FxSound installer (the driver isn't built from this tree). To load
  presets when debugging from VS, set FxSound_App's Debugging → Working
  Directory to `$(SolutionDir)..\..\bin\$(PlatformTarget)`.
- **After a fresh Projucer export** of `fxsound/FxSound.jucer`, re-add
  `audiopassthru/audiopassthru.vcxproj` and `dsp/DfxDsp.vcxproj` to the
  solution and add them as references from FxSound_App. This is a known
  Projucer quirk, not a code defect, if you see it in a PR.
- **Windows 7 build** uses JUCE 6.1.6 via `fxsound/FxSoundWin7.jucer` →
  `fxsound/ProjectWin7/FxSound.sln` (set FxSound_App toolset to v145, then do
  the same project/reference fixups). Exporting it overwrites the shared
  `fxsound/JuceLibraryCode/` (`AppConfig.h`, `BinaryData.*`, `JuceHeader.h`);
  re-export `fxsound/FxSound.jucer` with the latest Projucer afterwards and
  never commit the JUCE 6 versions of those files.
- **fxmcp** (Go 1.26+, run from `fxmcp/`):
  - `.\build.ps1` (all arches → `bin\<arch>\fxmcp.exe`), or
    `.\build.ps1 -Platforms x64 -Configuration Debug`
  - `go test ./...`; single test: `go test ./internal/fxsound -run TestName`
  - `internal/fxsound` unit tests are pure logic. `cmd/fxmcp/*_test.go` are
    integration tests that need FxSound installed (some skip if it isn't
    running); `write_tools_test.go` changes the real running app's settings.
  - After editing `docs/COMMAND_LINE_OPTIONS.md`, run `go generate ./...` to
    refresh `internal/mcpserver/docsdata/command_line_options.md`.
- **DSP tutorial** (Node, no build step; run from `web/dsp-tutorial/`):
  - `node tests/spot-values.js` — checks derived DSP parameters against the C sources.
  - `node tests/serve.js 8737` — serves the page under a strict CSP.
  - Golden comparison against the real engine (Windows only):
    `tests\golden\build-harness.cmd`, then `node tests/golden/run-golden.js`
    (back up and restore `HKCU\SOFTWARE\DFX` around it), then
    `node tests/golden-diff.js tests/golden/work` — expect `max|err| = 0`.
  - When changing the page, bump the `?v=` suffix on every asset in
    `index.html` and `VERSION` in `js/ui.js`.

There is no automated test suite for the C++ app, audiopassthru, or DfxDsp.

## Contribution conventions (from CONTRIBUTING.md)

- Branch from and target `develop/<platform>` (e.g. `develop/windows`), never
  `main` or `release/<platform>`. Temporary branches: `bugfix/<title>`,
  `feature/<title>`.
- Every PR links an issue; keep each PR to a single issue.
- Commit messages: lowercase conventional commits with the issue id, e.g.
  `fix: [#123] preset is not saved after renaming`.
- FxSound_App follows the Google C++ Style Guide; otherwise match the style of
  the module you're editing.

## PR review priorities

When reviewing pull requests:
- Flag any change to `audiopassthru/` or `dsp/` explicitly as higher-risk,
  even if the diff looks small.
- In `dsp/`, check for anything that could block or allocate on the real-time
  audio thread, and whether `web/dsp-tutorial/js/dsp/` needs a matching change.
- In `audiopassthru/`, check for driver-interface assumptions that might not
  hold across Windows versions (Windows 7 is still a supported target).
- Standard C++ concerns apply otherwise: resource leaks, undefined behavior,
  thread safety, off-by-one errors.
- Don't flag pure formatting/style differences unless they violate patterns
  already established nearby in the same file.

## Issue triage

When triaging an issue, report (don't apply labels unless explicitly asked to
in a separate `@claude apply labels` step):
- **Type**: bug, feature request, question, or build/installation issue.
- **Priority**: Urgent/High/Medium/Low, based on severity and how many users
  are likely affected — but note this is set on the GitHub Projects board's
  Priority field, not a label, so state your suggestion in the comment rather
  than assuming it can be applied automatically.
- **Related/duplicate issues**: search both open and closed issues.
- **Fixed status**: check recent commits on `main` and closed issues/PRs to
  see if this may already be fixed but not yet released.

## Labels

Only apply labels that already exist in the repository. Don't create new
labels. If no suitable label exists for something you'd normally tag, say so
in your comment instead of guessing.

## What not to do

- Don't merge, close, or assign issues/PRs yourself.
- Don't modify the GitHub Projects Priority field unless a workflow has been
  explicitly given permission and instruction to do so.
- Don't make changes to `audiopassthru/` or `dsp/` in an automated fix without
  flagging it clearly for human review before merge.
