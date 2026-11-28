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
#include <endpointvolume.h>

#include "BladeMP3EncDLL.h"
#include "slout.h"
#include "reg.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

/*
 * FUNCTION: sndDevicesInit()
 * DESCRIPTION:
 * NOTE - this is not an allocated handle, a pointer to an already instantiated handle structure
 * is passed in so the Init function is structured differently and there is no FreeUp function.
 * This was done to allow the C++ elements in the handle that use callback(virtual) functions to
 * operate correctly and also to allow the C++ elements in the handle to deallocate their memory correctly.
 */
int PT_DECLSPEC sndDevicesInit(PT_HANDLE *hp_sndDevices, CSlout *hp_slout, int i_initType, int i_trace_on, int *ip_status_flag)
{
	struct sndDevicesHdlType *cast_handle;
	IMMDeviceEnumeratorPtr pEnumerator = NULL;
	HRESULT hr;
	int numRealDevices;
	int DfxDeviceEnabledFlag;
	int statusFlag;
	int i;

	*ip_status_flag = SND_DEVICES_DEVICE_OPERATION_COMPLETED;


	// CoInitialize must be called at least once in every thread that uses COM functions.
	// Since this module may be used in a separate thread its called here.
	// Its ok to make this call multiple times if it has already been called in other modules or the main.
	// Apparently must be called before any variables are assigned.
	hr = CoInitialize( NULL );

	/* Allocate the handle */
	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;
	if (cast_handle == NULL)
		return(NOT_OKAY);

	cast_handle->slout_hdl = hp_slout;
	cast_handle->i_trace_on = i_trace_on;
	cast_handle->function_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;
	cast_handle->deviceChangeCallback = NULL;

	SLOUT_FIRST_LINE(L"sndDevicesInit():: sndDevicesInit() enters");

	SLOUT_FIRST_LINE(L"sndDevicesInit():: Calling CoCreateGuid()");
	hr = CoCreateGuid( &(cast_handle->guidThisApplication) );	// For identifying in callbacks device volume control changes made by this app
	
	cast_handle->fCaptureBuf  = NULL;
	cast_handle->fPlaybackBuf = NULL;
	cast_handle->fFilePlaybackBuf = NULL;
	cast_handle->captureBufAllocSize  = 0;
	cast_handle->playbackBufAllocSize = 0;

	cast_handle->initializationMode = i_initType;

	SLOUT_FIRST_LINE(L"sndDevicesInit()::  Setting PT handles");

	cast_handle->DeviceEvents.SetPtHandle(hp_sndDevices);
	cast_handle->EPVolEventsCapture.SetPtHandle(hp_sndDevices);
	cast_handle->EPVolEventsPlayback.SetPtHandle(hp_sndDevices);

	// Set all objects that will be later allocated to NULL and set all strings to "".
	cast_handle->pCaptureDevice = NULL;
	cast_handle->pPlaybackDevice = NULL;
	cast_handle->pAudioClientCapture = NULL;
	cast_handle->pAudioCaptureLoopback = NULL;
	cast_handle->pAudioClientPlayback = NULL;
    cast_handle->pAudioClientPlaybackRender = NULL;
	cast_handle->pEndptVolCapture = NULL;
	cast_handle->pEndptVolPlayback  = NULL;
	cast_handle->ignoreVolumeCallbacks = FALSE;
	cast_handle->ignoreDeviceCallbacks = FALSE;
	cast_handle->enumeratedDefaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	wcscpy(cast_handle->dfxDeviceID, L"");
	InterlockedExchange(&(cast_handle->deviceListIsStale), 0);
	InterlockedExchange(&(cast_handle->deviceFormatChanged), 0);
	cast_handle->carrierDelayTailFrames = 0;
	cast_handle->capturedBlockIsSilenceFill = FALSE;
	cast_handle->channelCacheCount = 0;
	cast_handle->activeDeviceIsUnprobed = FALSE;
	InterlockedExchange(&(cast_handle->channelProbeIsPending), 0);
	cast_handle->lastEnumerationTick = 0;
	wcscpy(cast_handle->requestedPlaybackID, L"");
	cast_handle->outputsAreManaged = FALSE;

	// No playback device has been used yet, so there is no mute setting of one to hold onto.
	// Note these must survive a re-init, they are only reset here in the one time init call.
	wcscpy(cast_handle->savedPlaybackDeviceID, L"");
	cast_handle->savedPlaybackMute = FALSE;
	cast_handle->savedPlaybackLevel = 1.0f;
	cast_handle->savedPlaybackLevelIsSaved = FALSE;
	cast_handle->playbackSettingsAreSaved = FALSE;
	cast_handle->engineSetPlaybackLevel = -1.0f;
	cast_handle->engineSetPlaybackMute = FALSE;
	cast_handle->engineSetPlaybackTick = 0;
	cast_handle->engineSetPlaybackFromMute = FALSE;
	cast_handle->engineSetPlaybackPrevLevel = -1.0f;
	cast_handle->engineSetPlaybackPrevMute = FALSE;
	cast_handle->engineSetPlaybackPrevTick = 0;
	cast_handle->engineTookOverPlaybackTick = 0;
	cast_handle->playbackFullVolumeReassert = FALSE;
	cast_handle->playbackTakeOversPending = 0;
	cast_handle->playbackLevelIsUsers = FALSE;
	cast_handle->playbackCallbackIsOnSavedDevice = FALSE;
	wcscpy(cast_handle->handedBackPlaybackDeviceID, L"");
	cast_handle->handedBackPlaybackMute = FALSE;
	cast_handle->handedBackPlaybackLevel = 1.0f;
	cast_handle->handedBackPlaybackLevelIsSaved = FALSE;
	cast_handle->handedBackPlaybackIsPending = FALSE;
	{
		std::lock_guard<std::mutex> lock(cast_handle->virtualDefaultMutex);
		cast_handle->virtualDefaultGuardArmed = FALSE;
		cast_handle->virtualDefaultGuardHint = FALSE;
		wcscpy(cast_handle->pickedDefaultID, L"");
		cast_handle->pickedDefaultSequence = 0;
		cast_handle->virtualDefaultGuardEpoch++;
	}
	InterlockedExchange(&(cast_handle->defaultPickSequence), 0);

	for(i=0; i<SND_DEVICES_MAX_NUM_DEVICES; i++)
	{
		cast_handle->pAllDevices[i] = NULL;
		wcscpy(cast_handle->deviceFriendlyName[i], L"");
		wcscpy(cast_handle->deviceDescription[i], L"");
		wcscpy(cast_handle->pwszID[i], L"");
		cast_handle->pwszIDRealDevices[i] = NULL;
		cast_handle->deviceFriendlyNameRealDevices[i] = NULL;
		cast_handle->deviceDescriptionRealDevices[i] = NULL;
		cast_handle->deviceState[i] = 0;
		wcscpy(cast_handle->deviceFormFactor[i], L"");
	}

	cast_handle->numRealDevices = 0;
	cast_handle->numPreviousRealDevices = 0;

	SLOUT_FIRST_LINE(L"sndDevicesInit():: Setting GUID ID vars");

	/* Set these GUID ID vars */
	cast_handle->CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
	cast_handle->IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
	cast_handle->IID_IAudioClient = __uuidof(IAudioClient);
	cast_handle->IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
	cast_handle->IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
	cast_handle->IID_IAudioEndpointVolume = __uuidof(IAudioEndpointVolume);


	/* Do additional initialization, use initType passed into init call. */
	SLOUT_FIRST_LINE(L"sndDevicesInit():: calling sndDevicesReInit");

	if( sndDevicesReInit(hp_sndDevices, i_initType, &numRealDevices, &DfxDeviceEnabledFlag, &statusFlag) != OKAY )
		return(NOT_OKAY);

	*ip_status_flag = statusFlag;

	hr = CoCreateInstance(cast_handle->CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, cast_handle->IID_IMMDeviceEnumerator, (void**)&pEnumerator);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_INSTANCE_CREATE_FAILED)

	// Setup callbacks that will catch devices "events" such as the default device being changed.
	hr = pEnumerator->RegisterEndpointNotificationCallback(&(cast_handle->DeviceEvents));
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_REGISTER_FAILED)

	SLOUT_FIRST_LINE(L"sndDevicesInit():: Returns OKAY");

	return(OKAY);
} 

/*
 * FUNCTION: sndDevices_FreeReuseableObjects()
 * DESCRIPTION:
 *  Frees the objects that will be reallocated when playback loop resets.
 *  This should only free objects that need to be reset, such as the audio device objects.
 */
int PT_DECLSPEC sndDevices_FreeReuseableObjects(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType *cast_handle;
	int i;
    
	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	// The SAFE_RELEASE macro checks to see if the var is NULL before making "release" call.
	// These audio client / loopback objects are re-created on every re-init, so they must
	// be released here to avoid leaking them on each sndDevicesReInit().  The processing
	// thread has already been stopped before this function is called.
	SAFE_RELEASE(cast_handle->pAudioCaptureLoopback);
	SAFE_RELEASE(cast_handle->pAudioClientCapture);
	SAFE_RELEASE(cast_handle->pAudioClientPlayback);
	SAFE_RELEASE(cast_handle->pAudioClientPlaybackRender);
	//cast_handle->pwfxCapture = NULL;
	//cast_handle->pwfxPlayback = NULL;

	for(i=0; i<SND_DEVICES_MAX_NUM_DEVICES; i++)
	{
		// pCaptureDevice/pPlaybackDevice are aliases into pAllDevices[] (no separate
		// reference), so releasing pAllDevices here is what releases those objects too.
		SAFE_RELEASE(cast_handle->pAllDevices[i]);
		// The MS example code was calling CoTastMemFree on these pwszID strings, but I don't know if its safe
		// to make this call on a string that may not have been allocated, don't do call for now.
		//CoTaskMemFree(cast_handle->pwszID[i]);
		//CoTaskMemFree(cast_handle->pwszIDRealDevices[i]);
		wcscpy(cast_handle->deviceFriendlyName[i], L"");
		wcscpy(cast_handle->deviceDescription[i], L"");
		wcscpy(cast_handle->deviceFormFactor[i], L"");
		cast_handle->deviceFriendlyNameRealDevices[i] = NULL;
		cast_handle->deviceDescriptionRealDevices[i] = NULL;
		cast_handle->deviceState[i] = 0;
	}

	// pCaptureDevice/pPlaybackDevice alias pAllDevices[] entries that were released above,
	// so just NULL the aliases - releasing them again would be a double-release.
	cast_handle->pCaptureDevice = NULL;
	cast_handle->pPlaybackDevice = NULL;

	return(OKAY);
}

/*
 * FUNCTION: sndDevicesFree()
 * DESCRIPTION:
 *  NOTE - this is not a standard PT handle, instead of allocating the handle in the init call,
 *  an instance of the handle is declared outside and a pointer to this handle is passed in to
 *  the init function. This was done so that C++ objects in the handle that use callbacks will
 *  function correctly.
 *  Since the handle was not allocated there is no FreeUp function, this free function is only
 *  used to deallocate the C++ objects in the handle.
 */
int PT_DECLSPEC sndDevicesFree(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType *cast_handle;
	IMMDeviceEnumeratorPtr pEnumerator = NULL;
	HRESULT hr;

	cast_handle = (struct sndDevicesHdlType *)(hp_sndDevices);

	if (cast_handle == NULL)
		return(OKAY);

	sndDevices_ReleaseAllAudioObjects(hp_sndDevices);

	// PTNOTE - trying to do a smart close of these in process recording files was causing problems.
	// Close will be eliminated but the .tmp recording file will be cleaned up on next startup.
	/*
	if( cast_handle->RecordingInProgress )
	{
		if( sndDevicesStartStopRecording(hp_sndDevices, SND_DEVICES_STOP_CAPTURE) != OKAY )
			return(NOT_OKAY);
	} */

	// Force the recording thread to exit.
	InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);

	if( (cast_handle->initializationMode == SND_DEVICES_INIT_FOR_PROCESSING))
	{
		if( sndDevices_FreeReuseableObjects(hp_sndDevices) != OKAY )
			return(NOT_OKAY);

		// Free buffers.
		if( cast_handle->fCaptureBuf != NULL )
			free( cast_handle->fCaptureBuf );

		if( cast_handle->fPlaybackBuf != NULL )
			free( cast_handle->fPlaybackBuf );

		if( cast_handle->fFilePlaybackBuf != NULL )
			free( cast_handle->fFilePlaybackBuf );
	}
	
	hr = CoCreateInstance(cast_handle->CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, cast_handle->IID_IMMDeviceEnumerator, (void**)&pEnumerator);

	hr = pEnumerator->UnregisterEndpointNotificationCallback(&(cast_handle->DeviceEvents));

	return(OKAY);
}

/*
 * FUNCTION: sndDevices_ReleaseAllAudioObjects()
 * DESCRIPTION: Stops and releases audio objects opened in Capture and Playback setup calls.
 */
int PT_DECLSPEC sndDevices_ReleaseAllAudioObjects(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType* cast_handle;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	// BetterFxSound: each endpoint volume pointer is taken out of the handle before its object is unregistered
	// and released, so the handle never holds a pointer to an object being released. The volume callbacks,
	// which can still be running on a COM thread at this point, do not use these pointers at all: they only
	// use the values their notification carries and plain fields of this static handle.
	IAudioEndpointVolume *pEndptVolCapture = cast_handle->pEndptVolCapture;
	IAudioEndpointVolume *pEndptVolPlayback = cast_handle->pEndptVolPlayback;
	cast_handle->pEndptVolCapture = NULL;
	cast_handle->pEndptVolPlayback = NULL;

	// Turn off capture device volume control callback
	if (pEndptVolCapture != NULL)
	{
		if (cast_handle->EPVolEventsCapture.IsRegistered())
		{
			pEndptVolCapture->UnregisterControlChangeNotify((IAudioEndpointVolumeCallback*)&(cast_handle->EPVolEventsCapture));
		}
		pEndptVolCapture->Release();
	}

	// BetterFxSound: the callback is about to leave the device whose setting is saved.
	cast_handle->playbackCallbackIsOnSavedDevice = FALSE;

	// Turn off playback device volume control callback
	if (pEndptVolPlayback != NULL)
	{
		if (cast_handle->EPVolEventsPlayback.IsRegistered())
		{
			pEndptVolPlayback->UnregisterControlChangeNotify((IAudioEndpointVolumeCallback*)&(cast_handle->EPVolEventsPlayback));
		}
		pEndptVolPlayback->Release();
	}

	/*if (cast_handle->pAudioClientCapture != NULL)
	{
		hr = cast_handle->pAudioClientCapture->Release();
	}

	if( cast_handle->pAudioClientPlayback != NULL )
	{
		hr = cast_handle->pAudioClientPlayback->Release();
	}*/

	return(OKAY);
}
