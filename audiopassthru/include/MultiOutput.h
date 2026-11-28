/*
BetterFxSound
Copyright (C) 2026  BetterFxSound contributors

This file is part of BetterFxSound, a modified version of FxSound
(Copyright (C) 2025 FxSound LLC). It adds support for sending the processed
audio to additional output devices at the same time as the main output.

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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AudioPassthru.h"

class SecondaryOutput;

// BetterFxSound: sets an output device's own endpoint volume to 100% and unmutes it, so that the
// only attenuation is the app's software gain (Windows master volume x the output's trim). Called
// when the engine starts using the device, never from an audio processing or render loop.
// event_context tags the change in the device's volume notifications (NULL is allowed).
// Returns false if the device or its volume control is not available. The caller saves the device's
// own setting first, and makes sure it is never used on the FxSound virtual device. BetterFxSound: if record_own,
// own_level and own_mute are recorded as owed (OwedVolume.h) once the take-over runs, unless a setting is owed already.
bool setDeviceFullVolumeById(const std::wstring& device_id, const GUID* event_context, bool record_own, float own_level,
	BOOL own_mute);

// BetterFxSound: reads a present device's own endpoint level (0..1) and mute. False if the device or its volume control
// is not available. Never from an audio processing or render loop.
bool getDeviceVolumeById(const std::wstring& device_id, float* level, BOOL* mute);

// BetterFxSound: puts the device's own level and mute back if the engine owes them (see OwedVolume.h), then forgets
// them. Returns false if something is owed and the device or its volume control is not available (it stays owed), or
// if the device was taken over again meanwhile (it is then left at 100% and stays owed).
// For devices the engine does not use (any more). Never from an audio processing or render loop. BetterFxSound: never
// waits for a hand-back of the same device that is already running (on a render thread, possibly stuck in a driver
// call): it then does nothing, returns false (the device stays owed) and sets *busy (if not null), so the caller tries
// again later.
bool handBackOwedVolumeById(const std::wstring& device_id, bool* busy = nullptr);

// Rewrites an interleaved block in place for the given channel mode (no-op for Both).
// Channel 0 is front left and channel 1 is front right in every standard layout. BetterFxSound: with more than two
// channels, left, right and mono are taken from the stereo fold-down (center and surrounds at -3 dB, no LFE), and played
// on front left and front right only (the other channels are silenced).
void applyOutputChannelMode(float* buffer, UINT32 frames, UINT32 channels, OutputChannelMode mode);

/*
 * Fans the processed audio out to any number of additional ("secondary") output devices.
 *
 * The carrier output (the original low latency path) keeps working as before and stays the
 * clock master. Every secondary output owns a ring buffer and its own event driven WASAPI
 * render thread, so a slow or failing secondary device can never stall the carrier. Each secondary
 * output compensates its own clock drift with an adaptive resampler.
 *
 * Threading:
 *  - configure() and stopAll() are called from the UI/timer thread. BetterFxSound: new outputs are created (ring,
 *    events, render thread) before the list is changed, which only swaps it, so the audio thread is held off for
 *    that swap alone. A block it could not hand over meanwhile is queued later as that much silence.
 *  - write() is called from the audio processing thread. It never blocks, never
 *    allocates and never calls into COM.
 *  - Each output's render thread holds a reference to its output, so an output that is removed while its
 *    thread is stuck in a driver call (a Bluetooth device going away can block WASAPI calls for seconds) is
 *    never freed under it: configure() only asks the removed outputs' threads to stop (BetterFxSound: it no longer
 *    waits for them, which froze the UI and the timer), and a thread frees its output on its own once it exits;
 *    stopAll() (exit) waits for them for a bounded time. BetterFxSound: it then leaves the device's volume to the engine's sweep
 *    (takeLateExits()) while the router drives its outputs, and hands it back itself while it does not.
 */
class MultiOutputRouter
{
public:
	MultiOutputRouter();
	~MultiOutputRouter();

	// BetterFxSound: alignPaths() delays a path (an output here or the carrier) by this much at most. The carrier's delay
	// line keeps room for it, see AudioPassthruPrivate::applyMainDelay().
	static constexpr int kMaxPathOffsetMs = 200;

	// Replaces the set of secondary outputs. Outputs whose device is already open keep
	// running (only their delay is updated), so calling this repeatedly is cheap.
	void configure(const std::vector<AdditionalOutputConfig>& outputs);

	// Closes every secondary output.
	void stopAll();

	// BetterFxSound: an output whose render thread could not be created (handles or memory exhausted) never plays; it
	// is created again here, at most once a second, with the settings of the last configure(). UI/timer thread.
	void retryFailedOutputs();

	// BetterFxSound: the state of each output, in the order of the last configure() (empty device IDs and duplicates
	// left out). Any thread.
	std::vector<AdditionalOutputStatus> getStatus() const;

	// Whether the engine drives the outputs: FxSound is on and processing, with the FxSound virtual
	// device as the Windows default. Each output saves its device's own level and mute and sets it to
	// 100% and unmuted while driven (at stream open, or here when driving starts), and puts the saved
	// setting back when driving stops (here, on power off), when it is removed, and at exit.
	// The device volume calls are made on the calling thread, without holding the router's lock. BetterFxSound: an
	// output whose render thread is opening its stream (and setting its device's volume) is not waited for, in either
	// direction: that thread applies the change itself once it is done (see pendingVolumeDevices()). UI/timer thread.
	void setDriven(bool driven);

	// BetterFxSound: the devices of the outputs whose render thread has not yet applied a setDriven() it was busy for
	// (opening its stream). Their owed volume is that thread's to hand back: the engine's sweep leaves them. Any thread.
	std::vector<std::wstring> pendingVolumeDevices() const;

	// Whether the engine drives the outputs (see setDriven()). Only then do they take their devices over.
	bool isDriven() const;

	// BetterFxSound: true once (then cleared) after a removed output's render thread, which the router stopped waiting
	// for, has exited. Such an output leaves its device's owed volume to the engine's sweep (handBackOwedVolumeById() for
	// the devices not in use), which the caller then runs: handing it back late, on that thread, undid a newer take-over
	// of the device. UI/timer thread.
	bool takeLateExits();

	// BetterFxSound: the outputs' device volume work: a take-over whose 100% failed is tried again, and a change of a
	// device's own level or mute is looked at (re-asserted to 100% right after the take-over, the user's later). It was
	// done on the outputs' render threads, between periods. UI/timer thread, called regularly while processing.
	void serviceVolume();

	// BetterFxSound: every output lines itself up once exactly on its latency target (by the offset of its averaged level),
	// however small that offset: a start leaves up to ~20 ms of it to the drift correction, which takes ~10 s to remove
	// it. Within a few hundred ms (an output that is not playing yet does it once it plays, for a few seconds). For the
	// delay measurement, before its first sweep. Not the audio thread (it takes the list's lock).
	void requestExactAlignment();
	// BetterFxSound: an output has not carried a requestExactAlignment() out yet (an output that does not play drops it
	// after a few seconds, so this ends). Not the audio thread.
	bool exactAlignmentPending();

	/*
	* BetterFxSound: audio thread, once per block; never blocks. Gives the carrier and the outputs here the same latency
	* from the processed block to the device, so an output's delay (and a measured one) means the same whether its device
	* is the carrier or plays here. The carrier holds its queue at carrier_latency_ms (half the capture buffer); an output
	* here holds its level at its own nominal latency (SecondaryOutput::pathLatencyMs()). Each path is delayed up to the
	* largest of them: the outputs here by themselves, the carrier by the value returned (ms, 0 when there is no output
	* here, so the carrier alone keeps its own latency). They differed by up to tens of ms, so delays synchronized with
	* one carrier were wrong once another device became the carrier.
	* BetterFxSound: an output not opened yet counts at the nominal latency of an output (the least one opens with), and the
	* common latency is lowered only while carrier_silent (the carrier's delay line holds no sound): the carrier's offset
	* jumped whenever the set of opened outputs changed, a fade and a gap of tens of ms on the carrier in mid-audio.
	*/
	int alignPaths(int carrier_latency_ms, bool carrier_silent);

	// Audio thread: queues one block of processed, interleaved 32 bit float audio.
	// solo_index >= 0: only that output gets the audio, the others get silence.
	// solo_index == -1: every output here gets silence (the carrier output is being measured).
	// Any other negative value: normal operation.
	void write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, int solo_index);

private:
	MultiOutputRouter(const MultiOutputRouter&) = delete;
	MultiOutputRouter& operator=(const MultiOutputRouter&) = delete;

	// BetterFxSound: configure() with configure_mutex_ held. wait_for_removed: wait (bounded) for the removed outputs'
	// render threads to exit (stopAll() only).
	void configureLocked(const std::vector<AdditionalOutputConfig>& outputs, bool wait_for_removed);

	mutable SRWLOCK lock_;
	// BetterFxSound: serializes the changes of the list (configure(), retryFailedOutputs()), which build the new list
	// without lock_. Guards configs_ and last_retry_tick_.
	std::mutex configure_mutex_;
	std::vector<AdditionalOutputConfig> configs_;	// As passed to the last configure()
	DWORD last_retry_tick_ = 0;
	// BetterFxSound, audio thread only: frames write() could not hand over while the list was swapped, at this rate,
	// since this tick. Queued as silence with the next block (at most 100 ms of them, and only shortly after).
	UINT32 skipped_frames_ = 0;
	UINT32 skipped_rate_ = 0;
	DWORD skipped_tick_ = 0;
	int carrier_path_offset_ms_ = 0;	// BetterFxSound, audio thread only: the last alignPaths() result
	int path_reference_ms_ = 0;			// BetterFxSound, audio thread only: the common latency alignPaths() last used
	// Read by the outputs' render threads, which may outlive the router (see above), hence shared.
	std::shared_ptr<std::atomic<bool>> driven_;
	// BetterFxSound: set by an output freed late on its render thread (takeLateExits()). Shared for the same reason.
	std::shared_ptr<std::atomic<bool>> late_exits_;
	// shared_ptr so that setDriven() can keep the outputs alive while it works on them without the lock.
	// Only the UI thread holds extra references, so an output is never destroyed on the audio thread.
	std::vector<std::shared_ptr<SecondaryOutput>> outputs_;
};
