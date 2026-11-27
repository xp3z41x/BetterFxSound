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
// Returns false if the device or its volume control is not available. The caller makes sure it is
// never used on the FxSound virtual device.
bool setDeviceFullVolume(IMMDevice* device, const GUID* event_context);
bool setDeviceFullVolumeById(const std::wstring& device_id, const GUID* event_context);

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
	// device as the Windows default. While it is not, an output whose stream opens leaves its device's
	// volume alone (the device may be the Windows default, playing the apps' sound directly); its
	// device is set to 100% and unmuted here, on the calling thread, once driving starts again.
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
	std::atomic<bool> driven_{ false };	// Read by the outputs' render threads
	std::vector<std::unique_ptr<SecondaryOutput>> outputs_;
};
