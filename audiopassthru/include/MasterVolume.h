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

/*
 * BetterFxSound: the Windows master volume, applied by the app as a software gain on every output.
 *
 * Windows' volume slider, volume keys and mute act on the FxSound virtual device, which is the
 * default playback device. The app captures that device with loopback, and loopback is taken
 * before the endpoint volume, so the captured audio is always full scale. The level and mute of
 * the virtual device are therefore read here and applied by the app itself to each output. The
 * volume of the real devices is never changed.
 *
 * Threading: masterVolumePublish() is called from the COM thread that delivers the endpoint
 * volume callback and from the thread doing the device reinit. masterVolumeGain() is a single
 * atomic load and is safe on the audio threads.
 */

// Publishes the master volume of the virtual device. level_scalar is the 0..1 slider value,
// level_db the matching level in dB (used when level_db_valid is true, because it follows the
// device's own volume curve).
void masterVolumePublish(float level_scalar, float level_db, bool level_db_valid, bool muted);

// Linear gain for the master volume, mute included, 0..1. Lock-free, real-time safe.
float masterVolumeGain();

// Linear gain for an output's own volume trim, 0..100 percent. Squared curve so the slider
// feels even: 50% is about -12 dB.
inline float outputTrimGain(int volume_percent)
{
	if (volume_percent <= 0) return 0.0f;
	if (volume_percent >= 100) return 1.0f;
	const float level = static_cast<float>(volume_percent) / 100.0f;
	return level * level;
}

// One step of the one-pole gain ramp used on every output. step is 1 / (time constant in
// samples). Snaps to the target once it is inaudibly close, so the gain never ends up in
// denormal range on a fade to silence.
inline float rampGain(float current, float target, float step)
{
	current += (target - current) * step;
	const float diff = target - current;
	if (diff < 1e-5f && diff > -1e-5f)
		current = target;
	return current;
}
