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
#include <memory>
#include <string>
#include <vector>

#include "AudioPassthru.h"

class SecondaryOutput;

/*
 * Fans the processed audio out to any number of additional ("secondary") output devices.
 *
 * The main output keeps working exactly as before and stays the clock master. Every
 * secondary output owns a ring buffer and its own event driven WASAPI render thread, so
 * a slow or failing secondary device can never stall the main output. Each secondary
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

	// Audio thread: queues one block of processed, interleaved 32 bit float audio.
	void write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate);

private:
	MultiOutputRouter(const MultiOutputRouter&) = delete;
	MultiOutputRouter& operator=(const MultiOutputRouter&) = delete;

	SRWLOCK lock_;
	std::vector<std::unique_ptr<SecondaryOutput>> outputs_;
};
