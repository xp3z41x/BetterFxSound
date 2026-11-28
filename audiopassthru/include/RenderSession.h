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
#include <Audioclient.h>
#include <audiopolicy.h>

/*
 * BetterFxSound: the audio session of every stream the engine renders (the carrier, the additional outputs and the
 * delay measurement's test sound). They all used the process's default session (a NULL session GUID), which on each
 * device is created by whichever stream opens it first and keeps that stream's flags until the app exits: visible in
 * the volume mixer and not persisted when an additional output opened it, hidden and persisted (its stored level
 * restored) when the carrier did. A level or mute set on it, in the mixer or stored by Windows, scaled that output on
 * top of the app's own gain, and carried over when the device changed role. Now every stream joins one private
 * session per device, always hidden and never persisted, and its level is put back to 100% and unmuted at each open.
 */

// {5C0A9E3B-71D2-4F86-B3A4-9E1D27C4F058}
const GUID kRenderSessionGuid = { 0x5c0a9e3b, 0x71d2, 0x4f86, { 0xb3, 0xa4, 0x9e, 0x1d, 0x27, 0xc4, 0xf0, 0x58 } };

// Session flags for IAudioClient::Initialize(), added to the stream's own flags. DISPLAY_HIDE is Windows 7 and later.
const DWORD kRenderSessionFlags = AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE | AUDCLNT_STREAMFLAGS_NOPERSIST;

// Sets the session of an initialized client to 100% and unmuted. Never on an audio processing or render loop.
// BetterFxSound: and opts it out of the Windows communications ducking. The virtual device is the communications default
// too while FxSound processes (sndDevicesSetCommunicationsDefault()), so a call is played through these sessions: ducking
// them for the call ("reduce other sounds") lowered the call itself. The other apps' sounds are still ducked, on the
// virtual device, before FxSound captures them. IAudioSessionControl2::SetDuckingPreference() is Windows 7 and later;
// where it is missing the session is left as it is.
inline void resetRenderSessionVolume(IAudioClient* client)
{
	if (client == nullptr)
		return;

	IAudioSessionControl* control = nullptr;
	if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(&control))) && control != nullptr)
	{
		IAudioSessionControl2* control2 = nullptr;
		if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2 != nullptr)
		{
			control2->SetDuckingPreference(TRUE);
			control2->Release();
		}
		control->Release();
	}

	ISimpleAudioVolume* volume = nullptr;
	if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) && volume != nullptr)
	{
		volume->SetMasterVolume(1.0f, NULL);
		volume->SetMute(FALSE, NULL);
		volume->Release();
	}
}
