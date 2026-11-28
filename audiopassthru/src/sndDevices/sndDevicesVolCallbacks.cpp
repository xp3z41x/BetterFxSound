/*
FxSound
Copyright (C) 2025  FxSound LLC

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
#include "codedefs.h"

/* Standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>

#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <atomic>
#include <mutex>

#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"
#include "MasterVolume.h"
#include "OwedVolume.h"

// BetterFxSound: master volume of the FxSound virtual device as a linear gain, mute included.
// Starts at unity until the first read in sndDevicesReInit().
static std::atomic<float> s_masterVolumeGain(1.0f);

// BetterFxSound: orders the two publishers (the volume callback and the reinit's read), see masterVolumePublishIfUnchanged().
// Never taken by the audio threads, which only load s_masterVolumeGain.
static std::mutex s_masterVolumeMutex;
static unsigned long s_masterVolumeSequence = 0;

static void masterVolumeStore(float level_scalar, bool muted)
{
	float gain = 0.0f;

	// "x > 0" is also false for NaN.
	if (!muted && level_scalar > 0.0f)
	{
		// Never boost: the processed audio is already close to full scale.
		const float level = (level_scalar < 1.0f) ? level_scalar : 1.0f;
		// Same squared curve as the output trim (outputTrimGain()): 50% is about -12 dB.
		gain = level * level;
	}

	s_masterVolumeGain.store(gain, std::memory_order_relaxed);
}

void masterVolumePublish(float level_scalar, bool muted)
{
	std::lock_guard<std::mutex> lock(s_masterVolumeMutex);
	s_masterVolumeSequence++;
	masterVolumeStore(level_scalar, muted);
}

unsigned long masterVolumeSequence()
{
	std::lock_guard<std::mutex> lock(s_masterVolumeMutex);
	return s_masterVolumeSequence;
}

bool masterVolumePublishIfUnchanged(float level_scalar, bool muted, unsigned long sequence)
{
	std::lock_guard<std::mutex> lock(s_masterVolumeMutex);
	if (s_masterVolumeSequence != sequence)
		return false;
	s_masterVolumeSequence++;
	masterVolumeStore(level_scalar, muted);
	return true;
}

float masterVolumeGain()
{
	return s_masterVolumeGain.load(std::memory_order_relaxed);
}

// These are for registering the volume callbacks. For some reason the callbacks would crash
// when these were objects in the handle, perhaps because it was allocated?
//CsndDevicesAudioEndpointVolumeCallbackCapture  g_EPVolEventsCapture;
//CsndDevicesAudioEndpointVolumeCallbackPlayback g_EPVolEventsPlayback;

//-----------------------------------------------------------
// Client implementation of IAudioEndpointVolumeCallback
// interface. When a method in the IAudioEndpointVolume
// interface changes the volume level or muting state of the
// endpoint device, the change initiates a call to the
// client's IAudioEndpointVolumeCallback::OnNotify method.
//-----------------------------------------------------------
// PTNOTE - since we have 2 volume controls being changed we need unique callback function
// for capture and playback to be able to distinguish which control generated the callback.

// Implementation of the Capture callback functions.
CsndDevicesAudioEndpointVolumeCallbackCapture::CsndDevicesAudioEndpointVolumeCallbackCapture() :  _cRef(0)
{
}

CsndDevicesAudioEndpointVolumeCallbackCapture::~CsndDevicesAudioEndpointVolumeCallbackCapture()
{
}

// IUnknown methods -- AddRef, Release, and QueryInterface

ULONG STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackCapture::AddRef()
{
  return InterlockedIncrement(&_cRef);
}

ULONG STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackCapture::Release()
{
  ULONG ulRef = InterlockedDecrement(&_cRef);
  return ulRef;
}

HRESULT STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackCapture::QueryInterface(REFIID riid, VOID **ppvInterface)
{
  if (IID_IUnknown == riid)
  {
		AddRef();
		*ppvInterface = (IUnknown*)this;
  }
  else if (__uuidof(IAudioEndpointVolumeCallback) == riid)
  {
		AddRef();
		*ppvInterface = (IAudioEndpointVolumeCallback*)this;
  }
  else
  {
		*ppvInterface = NULL;
		return E_NOINTERFACE;
  }
  return S_OK;
}

// PTNOTE - this is the callback hit when the capture volume controls is moved.
// BetterFxSound: the capture device is the FxSound virtual device, whose volume is the Windows master
// volume. It is no longer copied to the real playback device. It is published as a software gain that
// the app applies to every output (see MasterVolume.h). Nothing here writes to any device.
HRESULT STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackCapture::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify)
{
	struct sndDevicesHdlType *cast_handle;

	cast_handle = (struct sndDevicesHdlType *)g_sndDevicesVolumeCallbackCapture_hdl;

	if (cast_handle == NULL)
		return(S_FALSE);

	if (cast_handle->ignoreVolumeCallbacks)
		return(S_OK);

	if (pNotify == NULL)
	{
		return E_INVALIDARG;
	}

	// Only the values the notification carries are used: no COM call, and nothing is read from the handle's
	// audio objects, which a reinit on another thread may be releasing while this callback runs.
	masterVolumePublish(pNotify->fMasterVolume, pNotify->bMuted != FALSE);

	return S_OK;
}

// Function to be called from sndDevicesInit to pass in allocated handle for by methods in this class.
void CsndDevicesAudioEndpointVolumeCallbackCapture::SetPtHandle(PT_HANDLE *sndDevices_hdl)
{
	g_sndDevicesVolumeCallbackCapture_hdl = sndDevices_hdl;
}

bool CsndDevicesAudioEndpointVolumeCallbackCapture::IsRegistered()
{
	return _cRef > 0;
}

// Implementation of the Playback callback functions
CsndDevicesAudioEndpointVolumeCallbackPlayback::CsndDevicesAudioEndpointVolumeCallbackPlayback() :  _cRef(0)
{
}

CsndDevicesAudioEndpointVolumeCallbackPlayback::~CsndDevicesAudioEndpointVolumeCallbackPlayback()
{
}

// IUnknown methods -- AddRef, Release, and QueryInterface

ULONG STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackPlayback::AddRef()
{
  return InterlockedIncrement(&_cRef);
}

ULONG STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackPlayback::Release()
{
  ULONG ulRef = InterlockedDecrement(&_cRef);
  return ulRef;
}

HRESULT STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackPlayback::QueryInterface(REFIID riid, VOID **ppvInterface)
{
  if (IID_IUnknown == riid)
  {
		AddRef();
		*ppvInterface = (IUnknown*)this;
  }
  else if (__uuidof(IAudioEndpointVolumeCallback) == riid)
  {
		AddRef();
		*ppvInterface = (IAudioEndpointVolumeCallback*)this;
  }
  else
  {
		*ppvInterface = NULL;
		return E_NOINTERFACE;
  }
  return S_OK;
}

// PTNOTE - this is the callback hit when the playback volume control is moved.
// BetterFxSound: the real playback device's volume and mute are no longer copied to the FxSound virtual
// device. Only the saved mute setting and level are kept up to date, so that what gets put back on the
// device when FxSound stops driving it is what the user set last. The engine's own changes (100% and
// unmuted when it starts using the device, the setting put back) carry guidThisApplication and are ignored.
HRESULT STDMETHODCALLTYPE CsndDevicesAudioEndpointVolumeCallbackPlayback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify)
{
	struct sndDevicesHdlType *cast_handle;

	cast_handle = (struct sndDevicesHdlType *)g_sndDevicesVolumeCallbackPlayback_hdl;

	if (cast_handle == NULL)
		return(S_FALSE);

	if (cast_handle->ignoreVolumeCallbacks)
		return(S_OK);

	if (pNotify == NULL)
	{
		return E_INVALIDARG;
	}

	// Ignore any callbacks caused by our own volume setting calls in this app. BetterFxSound: also those of the additional
	// outputs and of the owed-volume hand-back (kEngineVolumeEventContext), which may target the carrier's device for a
	// moment (it is still one of the router's outputs right after a reinit made it the carrier).
	if( (pNotify->guidEventContext == cast_handle->guidThisApplication) || (pNotify->guidEventContext == kEngineVolumeEventContext) )
		return S_OK;

	// BetterFxSound: a driver reporting back, without this app's event context, the level and mute this app has
	// just set (Bluetooth devices with absolute volume do): not a change the user made. The engine sets the level, then
	// the mute, and a driver may report the level change alone: the new level with the mute from before. A late report
	// of the engine's previous change, overwritten since, is not the user's either. Level and mute were compared as one
	// pair with the last change only, so those reports were recorded as the user's own setting.
	{
		const DWORD now = GetTickCount();
		const DWORD sinceEngineSet = now - cast_handle->engineSetPlaybackTick.load();
		const DWORD sincePrevEngineSet = now - cast_handle->engineSetPlaybackPrevTick.load();
		const bool muted = (pNotify->bMuted != FALSE);
		const float levelDiff = pNotify->fMasterVolume - cast_handle->engineSetPlaybackLevel.load();
		const float prevLevelDiff = pNotify->fMasterVolume - cast_handle->engineSetPlaybackPrevLevel.load();
		if( (sinceEngineSet < 3000) &&
			 (levelDiff < 0.01f) && (levelDiff > -0.01f) &&
			 ((muted == (cast_handle->engineSetPlaybackMute.load() != FALSE)) || (muted == (cast_handle->engineSetPlaybackFromMute.load() != FALSE))) )
			return S_OK;
		if( (sincePrevEngineSet < 3000) &&
			 (prevLevelDiff < 0.01f) && (prevLevelDiff > -0.01f) &&
			 (muted == (cast_handle->engineSetPlaybackPrevMute.load() != FALSE)) )
			return S_OK;
	}

	// BetterFxSound: the callback is on the device the saved setting belongs to. It used to compare the saved device with
	// the list entry at playbackDeviceNum, which a reinit resets and an enumeration rewrites, so changes were dropped.
	if (cast_handle->playbackSettingsAreSaved && (cast_handle->playbackCallbackIsOnSavedDevice.load() != FALSE))
	{
		const bool atFullVolume = (pNotify->fMasterVolume >= 0.99f) && (pNotify->bMuted == FALSE);

		// BetterFxSound: a few seconds after the engine set the device to 100%, a change away from it is the device's own
		// (a Bluetooth headset sending its stored absolute volume once connected), not the user's: 100% is asked for again.
		// It used to be recorded as the user's setting after 3 s, and the carrier then played at that level on top of the
		// app's gain. BetterFxSound: also while a take-over is under way (playbackTakeOversPending).
		const DWORD tookOverTick = cast_handle->engineTookOverPlaybackTick.load();
		const bool takeOverPending = (cast_handle->playbackTakeOversPending.load() > 0);
		if( (takeOverPending || ((tookOverTick != 0) && ((GetTickCount() - tookOverTick) < 5000))) && !atFullVolume && !cast_handle->playbackLevelIsUsers.load() )
		{
			cast_handle->playbackFullVolumeReassert = TRUE;
			return S_OK;
		}

		// BetterFxSound: 100% and unmuted while the engine holds the device there (or is about to) and the user has not set a
		// level of their own is the engine's own setting (a driver's late echo of it, past the echo guards above), not the
		// user's: it was recorded as the device's own level, which was then put back at the hand-back, leaving the device at
		// 100%. The same policy as the additional outputs.
		if( (takeOverPending || (tookOverTick != 0)) && atFullVolume && !cast_handle->playbackLevelIsUsers.load() )
			return S_OK;

		cast_handle->savedPlaybackMute = pNotify->bMuted;
		cast_handle->savedPlaybackLevel = pNotify->fMasterVolume;
		cast_handle->savedPlaybackLevelIsSaved = TRUE;
		// While the device is at our 100%, its owed own setting follows the user's change too (OwedVolume.h).
		float owedLevel;
		BOOL owedMute;
		if( owedVolumeGet(cast_handle->savedPlaybackDeviceID, &owedLevel, &owedMute) )
		{
			owedVolumeUpdate(cast_handle->savedPlaybackDeviceID, pNotify->fMasterVolume, pNotify->bMuted);
			// BetterFxSound: the device keeps the user's setting while the engine uses it, a reinit no longer sets it to 100%
			// again (see playbackLevelIsUsers).
			if( !atFullVolume )
				cast_handle->playbackLevelIsUsers = TRUE;
		}
	}
	return S_OK;
}

// Function to be called from sndDevicesInit to pass in allocated handle for by methods in this class.
void CsndDevicesAudioEndpointVolumeCallbackPlayback::SetPtHandle(PT_HANDLE *sndDevices_hdl)
{
	g_sndDevicesVolumeCallbackPlayback_hdl = sndDevices_hdl;
}

bool CsndDevicesAudioEndpointVolumeCallbackPlayback::IsRegistered()
{
	return _cRef > 0;
}