/*
BetterFxSound
Copyright (C) 2026  BetterFxSound contributors

This file is part of BetterFxSound, a modified version of FxSound
(Copyright (C) 2025 FxSound LLC).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <windows.h>
#include <atomic>
#include <climits>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "AudioPassthru.h"

/*
 * Measures how long each output takes to be heard, using the default microphone.
 *
 * A short sweep ("chirp") is played into the FxSound virtual device, so it travels the same
 * path as any other sound: capture, DSP, then the outputs. While one output is being measured
 * all the others are silenced (solo). The microphone recording is matched against the sweep
 * (cross-correlation) to find when it was heard. Every output is measured several times, and
 * the median of the measurements that agree is used (at least two must agree, so noise picked
 * up from an output that cannot be heard gives no result). Every output first gets a short
 * stretch of silence, so streams that were idle are running before the first sweep. An output
 * with a delay is listened to for that much longer.
 *
 * Emission and arrival times are both taken from WASAPI's QPC timestamps, so the results do
 * not depend on how the two streams were buffered. Any delay that is common to all outputs
 * (virtual device, capture) cancels out when the results are compared.
 */
class DelayCalibration
{
public:
	enum class State { Idle = 0, Running = 1, Done = 2, Failed = 3 };

	struct Target
	{
		std::wstring device_id;
		int solo_index;			// -1 = carrier output, otherwise index of the additional output
		int delay_ms;			// The output's current delay: its sweep arrives that much later, so it is listened for that much longer
	};

	// solo(-1) = carrier only, solo(k) = additional output k only, solo(INT_MIN) = everything back on.
	// Targets may be in any order and the carrier may be absent; results follow the order of the targets.
	typedef std::function<void(int)> SoloFunction;
	// BetterFxSound: lines every additional output up exactly on its latency target, whatever its offset (a fresh start
	// leaves up to ~20 ms to the slow drift correction). Called once the warm-up has started them, before the first sweep.
	typedef std::function<void()> AlignFunction;
	// BetterFxSound: true while an output has not done what AlignFunction asked yet. The first sweep waits for it.
	typedef std::function<bool()> AlignPendingFunction;

	DelayCalibration();
	~DelayCalibration();

	// Starts a measurement on its own thread. Returns false if one is already running.
	// With no target it fails at once (state Failed, no thread) with no_targets_error as the error,
	// or a generic one if that is empty. Callers also use this to report why a measurement cannot run.
	// BetterFxSound: stream_generation, if given, must outlive the measurement. It changes whenever the audio of the
	// outputs is interrupted (a processing thread ended); if it no longer equals start_generation while the sweeps
	// play, the measurement fails, as it recorded gaps or partial sweeps. solo, align, align_pending and stream_generation,
	// and what they use, must outlive the measurement thread, or shutdown() returning false (see there).
	bool start(const std::wstring& virtual_device_id, const std::vector<Target>& targets, SoloFunction solo,
		AlignFunction align, AlignPendingFunction align_pending, const std::wstring& no_targets_error = std::wstring(),
		const std::atomic<unsigned int>* stream_generation = nullptr, unsigned int start_generation = 0);
	void cancel();
	/*
	* BetterFxSound: cancels the measurement and waits up to timeout_ms for its thread. True: it ended, this object may be
	* freed. False: it is held in a stream call (a device driver that does not return); it no longer calls solo, align or
	* align_pending nor reads stream_generation, but still uses this object, which the caller must then leave allocated.
	* The destructor waits without bound (FxSound's exit hung on such a driver), so an owner calls this first.
	*/
	bool shutdown(DWORD timeout_ms);
	// Stops a running measurement because it can no longer be valid; it fails with this reason.
	void abort(const std::wstring& reason);
	// BetterFxSound: Running, but cancelled or aborted: the thread ends within a few ms and its results are discarded.
	bool isStopping() const;
	// BetterFxSound: the sweeps are being played and recorded, with an output soloed: the outputs must keep their order.
	// Running but false: the measurement is being analysed (the outputs are all back on, the streams closed).
	bool holdsOutputs() const;
	// BetterFxSound: results that are no longer valid (the carrier changed after the measurement ended) are dropped: a
	// Done measurement whose results were not taken yet (getResults()) becomes Failed with this reason. Nothing otherwise.
	void invalidate(const std::wstring& reason);

	State getState(int* progress_percent) const;
	std::vector<DelayCalibrationResult> getResults() const;
	std::wstring getError() const;

	static constexpr int kSoloNone = INT_MIN;

private:
	static DWORD WINAPI threadProc(LPVOID param);
	void run();
	void fail(const std::wstring& message);
	// BetterFxSound: publishes why the measurement stopped early: a cancel (Idle), an abort (Failed with its reason), or
	// a device error (cancelled false). Only once the streams are closed and the outputs are all back on.
	void publishStopped(bool cancelled);
	// BetterFxSound: the caller's functions and stream_generation, called or read by the thread under callback_mutex_
	// and never once detached_ (shutdown() gave up waiting): the caller may have freed what they use.
	void callSolo(int index);
	void callAlign();
	bool callAlignPending();
	bool streamsInterrupted();

	std::wstring virtual_device_id_;
	std::vector<Target> targets_;
	SoloFunction solo_;
	AlignFunction align_;
	AlignPendingFunction align_pending_;
	const std::atomic<unsigned int>* stream_generation_ = nullptr;
	unsigned int start_generation_ = 0;

	HANDLE thread_ = NULL;
	std::atomic<int> state_{ 0 };
	std::atomic<int> progress_{ 0 };
	std::atomic<bool> cancel_{ false };
	std::atomic<bool> holds_outputs_{ false };	// BetterFxSound: see holdsOutputs()
	std::mutex callback_mutex_;		// BetterFxSound: see callSolo()
	bool detached_ = false;			// BetterFxSound: shutdown() gave up waiting for the thread, under callback_mutex_

	mutable std::mutex mutex_;
	std::vector<DelayCalibrationResult> results_;
	std::wstring error_;
	std::wstring abort_reason_;		// Set by abort(), under mutex_
	mutable bool results_taken_ = false;	// BetterFxSound: getResults() was called since the last Done, under mutex_
};
