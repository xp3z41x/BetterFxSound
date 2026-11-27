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

#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"
#include "MasterVolume.h"

// BetterFxSound: master volume of the FxSound virtual device as a linear gain, mute included.
// Starts at unity until the first read in sndDevicesReInit().
static std::atomic<float> s_masterVolumeGain(1.0f);

void masterVolumePublish(float level_scalar, float level_db, bool level_db_valid, bool muted)
{
	float gain;

	// "!(x > 0)" also catches NaN.
	if (muted || !(level_scalar > 0.0f))
	{
		gain = 0.0f;
	}
	else if (level_db_valid && level_db == level_db)
	{
		// The level in dB follows the device's own volume curve, like Windows would apply it.
		gain = powf(10.0f, level_db / 20.0f);
	}
	else
	{
		// Fallback, close to the usual audio taper.
		gain = level_scalar * level_scalar;
	}

	// Never boost: the processed audio is already close to full scale.
	if (!(gain > 0.0f)) gain = 0.0f;
	if (gain > 1.0f) gain = 1.0f;

	s_masterVolumeGain.store(gain, std::memory_order_relaxed);
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
	float levelDb = 0.0f;
	bool levelDbValid = false;

	if (pNotify == NULL)
	{
		return E_INVALIDARG;
	}

	cast_handle = (struct sndDevicesHdlType *)g_sndDevicesVolumeCallbackCapture_hdl;

	// Publishing is harmless, so it is done even while ignoreVolumeCallbacks is set: the gain must always
	// follow the slider. The level in dB is not part of the notification, so it is read from the endpoint.
	if (cast_handle != NULL && cast_handle->pEndptVolCapture != NULL)
	{
		levelDbValid = SUCCEEDED(cast_handle->pEndptVolCapture->GetMasterVolumeLevel(&levelDb));
	}

	masterVolumePublish(pNotify->fMasterVolume, levelDb, levelDbValid, pNotify->bMuted != FALSE);

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
// BetterFxSound: the real playback device's volume and mute are the user's own and are no longer copied
// to the FxSound virtual device. Only the saved mute setting is kept up to date, so that what gets put
// back on the device when FxSound stops driving it is what the user set last.
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

	// Ignore any callbacks caused by our own volume setting calls in this app.
	if( pNotify->guidEventContext != cast_handle->guidThisApplication )
	{
		if (cast_handle->playbackSettingsAreSaved &&
			 (cast_handle->playbackDeviceNum >= 0) && (cast_handle->playbackDeviceNum < SND_DEVICES_MAX_NUM_DEVICES) &&
			 (wcscmp(cast_handle->savedPlaybackDeviceID, cast_handle->pwszID[cast_handle->playbackDeviceNum]) == 0))
		{
			cast_handle->savedPlaybackMute = pNotify->bMuted;
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