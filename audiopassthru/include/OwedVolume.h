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
#include <string>
#include <vector>

/*
 * BetterFxSound: the devices' own volume, owed back to them.
 *
 * While the engine uses a real device as an enabled output it sets it to 100% and unmuted (the Windows volume is
 * applied inside the app). The device's own level and mute are recorded here before that, and erased once they
 * have been put back on the device. An entry therefore means "this device is at our 100%, its own setting is this".
 * BetterFxSound: or at a level the user set on it while the engine used it, which is then the recorded one (the
 * engine keeps it rather than setting 100% again, until it hands the device back).
 *
 * The record outlives the objects that use the device: it is kept in the registry, under
 * HKCU\Software\BetterFxSound\OwedDeviceVolume (one REG_BINARY value per endpoint ID). So a device that is
 * unplugged while in use, an output whose render thread is stopped before it could hand the device back, or a
 * crash, does not lose the device's own setting:
 *  - when the engine takes the device over again, it keeps the recorded setting instead of saving our 100%;
 *  - when the device is seen again while the engine does not use it, the recorded setting is put back.
 *
 * Thread safe. Registry access: never call from an audio processing or render loop.
 */

// BetterFxSound: the event context of every change the engine makes to a device's own level and mute (the carrier's and
// the additional outputs' take-over to 100% and hand-back). The volume callbacks of the carrier and of the additional
// outputs ignore the notifications that carry it: the engine's own changes are not the user's.
// {9B3E6C41-2D7A-4E85-A1F0-6C2D84B95E17}
const GUID kEngineVolumeEventContext = { 0x9b3e6c41, 0x2d7a, 0x4e85, { 0xa1, 0xf0, 0x6c, 0x2d, 0x84, 0xb9, 0x5e, 0x17 } };

// The device's own level (0..1) and mute, if the engine owes them back.
bool owedVolumeGet(const std::wstring& device_id, float* level, BOOL* mute);

// Records the device's own setting before the engine sets it to 100%. Does nothing if a setting is already owed for
// it: the device is then still at our 100%, and the recorded setting is its real own one.
void owedVolumeRecord(const std::wstring& device_id, float level, BOOL mute);

// Replaces the owed setting with a newer own setting (the user changed the device while the engine used it).
// Does nothing if nothing is owed for the device.
void owedVolumeUpdate(const std::wstring& device_id, float level, BOOL mute);

// The setting was put back on the device: nothing is owed any more.
void owedVolumeErase(const std::wstring& device_id);

// Every device something is owed to.
std::vector<std::wstring> owedVolumeDevices();
