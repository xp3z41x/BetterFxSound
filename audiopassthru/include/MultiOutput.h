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
#include <string>
#include <vector>

#include "AudioPassthru.h"

class SecondaryOutput;

// BetterFxSound: sets an output device's own endpoint volume to 100% and unmutes it, so that the
// only attenuation is the app's software gain (Windows master volume x the output's trim). Called
// when the engine starts using the device, never from an audio processing or render loop.
// event_context tags the change in the device's volume notifications (NULL is allowed).
// Returns false if the device or its volume control is not available. The caller saves the device's
// own setting first, and makes sure it is never used on the FxSound virtual device.
bool setDeviceFullVolumeById(const std::wstring& device_id, const GUID* event_context);

// BetterFxSound: puts the device's own level and mute back if the engine owes them (see OwedVolume.h), then forgets
// them. Returns false if something is owed and the device or its volume control is not available (it stays owed).
// For devices the engine does not use (any more). Never from an audio processing or render loop.
bool handBackOwedVolumeById(const std::wstring& device_id);

// Rewrites an interleaved block in place for the given channel mode (no-op for Both).
// Channel 0 is front left and channel 1 is front right in every standard layout.
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
 *  - configure() and stopAll() are called from the UI/timer thread.
 *  - write() is called from the audio processing thread. It never blocks, never
 *    allocates and never calls into COM.
 *  - Each output's render thread holds a reference to its output, so an output that is removed while its
 *    thread is stuck in a driver call (a Bluetooth device going away can block WASAPI calls for seconds) is
 *    never freed under it: removal waits for the threads for a bounded time, and a thread that is still busy
 *    finishes and frees its output on its own.
 */
class MultiOutputRouter
{
public:
	MultiOutputRouter();
	~MultiOutputRouter();

	// Replaces the set of secondary outputs. Outputs whose device is already open keep
	// running (only their delay is updated), so calling this repeatedly is cheap.
	void configure(const std::vector<AdditionalOutputConfig>& outputs);

	// Closes every secondary output.
	void stopAll();

	// Whether the engine drives the outputs: FxSound is on and processing, with the FxSound virtual
	// device as the Windows default. Each output saves its device's own level and mute and sets it to
	// 100% and unmuted while driven (at stream open, or here when driving starts), and puts the saved
	// setting back when driving stops (here, on power off), when it is removed, and at exit.
	// The device volume calls are made on the calling thread, without holding the router's lock.
	// UI/timer thread.
	void setDriven(bool driven);

	// Audio thread: queues one block of processed, interleaved 32 bit float audio.
	// solo_index >= 0: only that output gets the audio, the others get silence.
	// solo_index == -1: every output here gets silence (the carrier output is being measured).
	// Any other negative value: normal operation.
	void write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, int solo_index);

private:
	MultiOutputRouter(const MultiOutputRouter&) = delete;
	MultiOutputRouter& operator=(const MultiOutputRouter&) = delete;

	SRWLOCK lock_;
	// Read by the outputs' render threads, which may outlive the router (see above), hence shared.
	std::shared_ptr<std::atomic<bool>> driven_;
	// shared_ptr so that setDriven() can keep the outputs alive while it works on them without the lock.
	// Only the UI thread holds extra references, so an output is never destroyed on the audio thread.
	std::vector<std::shared_ptr<SecondaryOutput>> outputs_;
};
