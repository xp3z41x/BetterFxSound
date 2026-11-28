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

#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <endpointvolume.h>

#include "slout.h"
#include "reg.h"
#include "u_sndDevices.h"
#include "sndDevices.h"
#include "MasterVolume.h"

/*
 * FUNCTION: sndDevicesReInit()
 * DESCRIPTION:
 *  This function will be called when any event that changes the properties of capture/playback devices occurs,
 *  such as a change in the playback devices sampling frequency that causes the processing loop to exit or
 *  a change in the currently selected default device.
 */
int PT_DECLSPEC sndDevicesReInit(PT_HANDLE *hp_sndDevices, int i_initType, int *ipNumRealDevices, int *ipDfxDeviceEnabledFlag, int *ip_status)
{
	struct sndDevicesHdlType *cast_handle;
	float captureVolScalar = 1.0f;
	wchar_t name[PT_MAX_GENERIC_STRLEN];
	wchar_t regVal[PT_MAX_GENERIC_STRLEN];
	HRESULT hr;
	int resultFlag;
	int loopCount;
	int captureAllocSize, playbackAllocSize;
	int captureBufferChannelsForAllocation;
	BOOL captureMuteState;
    
	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	SLOUT_FIRST_LINE(L"sndDevicesReInit():: sndDevicesReInit() enters");

	*ip_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;
	*ipNumRealDevices = 0;
	*ipDfxDeviceEnabledFlag = IS_FALSE;
	cast_handle->function_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;	// Clear function error status code.
	cast_handle->playbackSetupFailed = FALSE;	// BetterFxSound: set below when the carrier itself fails

	//cast_handle->ignoreDeviceCallbacks = TRUE;
	//cast_handle->ignoreVolumeCallbacks = TRUE;

	cast_handle->totalNumDevices = SND_DEVICES_DEVICE_NOT_PRESENT;
	cast_handle->dfxDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	cast_handle->captureDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	cast_handle->playbackDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	cast_handle->defaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	cast_handle->priorDefaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;

	cast_handle->upsampleRatio = 1;
	cast_handle->numPlaybackFramesAvailableToFill = 0;
	cast_handle->playbackIsActive = SND_DEVICES_PLAYBACK_IS_STOPPED;
	cast_handle->playbackStreamIsTemporarilyPaused = 0;

	// BetterFxSound: the only place the stop flag is cleared, before the devices are enumerated below. A stop requested
	// from now on (by a device notification or a carrier switch) is kept, and the processing thread started after this
	// reinit exits at once so another reinit sees it.
	InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 0);
	// BetterFxSound: the list read below and the outputs refresh that follows the reinit cover any earlier change.
	InterlockedExchange(&(cast_handle->deviceListIsStale), 0);

	wcscpy(cast_handle->lastDeviceAddCallbackGuid, L"");
	cast_handle->lastDeviceAddCallbackGuidtype = 0;
	
	cast_handle->noBufferCount = 0;
	cast_handle->MeasuredVirtualSilentBufferCount = 0;
	cast_handle->MeasuredTrueSilentBufferCount = 0;
	cast_handle->WindowsSilentBufferCount = 0;

	{	// The SND_DEVICES_INIT_FOR_PROCESSING and SND_DEVICES_INIT_NO_PROCESSING cases.

		// Free any reuseable objects that have been allocated.
		if( sndDevices_FreeReuseableObjects(hp_sndDevices) != OKAY )
			return(NOT_OKAY);

		*ipDfxDeviceEnabledFlag = IS_TRUE; //Will get set to false in loop below if DFX device is not found.
		loopCount = 0;
		do
		{
			Sleep(50);

			// Gets the properties of all current devices, finds DFX device if present.
			if( sndDevices_GetAll( hp_sndDevices, &(cast_handle->totalNumDevices) ) != OKAY )
				return(NOT_OKAY);

			*ipNumRealDevices = cast_handle->numRealDevices;

			// When we can read the DFX name, its enabled.
			if( sndDevicesGetFriendlyName(hp_sndDevices, SND_DEVICES_VIRTUAL_PLAYBACK_DFX, name, &resultFlag) != OKAY )
				return(NOT_OKAY);

			loopCount++;

			if( loopCount > 20 )	// 20 loops of 50 millisecs is 1 second.
			{
				if( i_initType == SND_DEVICES_INIT_FOR_PROCESSING )
				{
					*ipDfxDeviceEnabledFlag = IS_FALSE;
					return(OKAY);
				}
				else
				{
					break;
				}
			}
		}
		while( resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED );
	}

	// If audio processing will be performed, do additional initialization.
	if( (i_initType == SND_DEVICES_INIT_FOR_PROCESSING))
	{
		if (cast_handle->numRealDevices > 0) 
		{
			// Based on current and past default device settings and user selections,
			// assign the current playback device, the DFX capture device and the default device.
			if (sndDevicesImplementDeviceRules(hp_sndDevices, &resultFlag) != OKAY)
				return(NOT_OKAY);

			if (resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = resultFlag;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_RULES_NOT_POSSIBLE);
			}

            sndDevices_ReleaseAllAudioObjects(hp_sndDevices);

			// Do the initial non-format dependent setup of capture and playback devices.
			if (sndDevicesInitialSetupCaptureDevice(hp_sndDevices, &resultFlag) != OKAY)
				return(NOT_OKAY);

			if (resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = resultFlag;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_RULES_NOT_POSSIBLE);
			}

			if (sndDevicesInitialSetupPlaybackDevice(hp_sndDevices, &resultFlag) != OKAY)
				return(NOT_OKAY);

			if (resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				cast_handle->playbackSetupFailed = TRUE;
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = resultFlag;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_RULES_NOT_POSSIBLE);
			}

			// BetterFxSound: read the Windows master volume (the FxSound virtual device's level and mute) and
			// publish it as the software gain applied to every output. It is no longer copied to the real
			// playback device. If the carrier is an enabled output, its real device is set to 100% and unmuted
			// after the reinit, by AudioPassthruPrivate::applyAdditionalOutputs().
			hr = cast_handle->pEndptVolCapture->GetMute(&captureMuteState);
			if (hr != S_OK)
			{
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = SND_DEVICES_RULES_NOT_POSSIBLE;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_SET_MUTE_FAILED);
			}

			hr = cast_handle->pEndptVolCapture->GetMasterVolumeLevelScalar(&captureVolScalar);
			if (hr != S_OK)
			{
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = SND_DEVICES_RULES_NOT_POSSIBLE;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_GET_MASTER_VOLUME_FAILED);
			}

			masterVolumePublish(captureVolScalar, captureMuteState != FALSE);

			// Setup capture and playback buffers
			cast_handle->bufferSizeMilliSecs = SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS;

			// Read the buffer_size from the user setting location, if present set the internal value.
			if (sndDeviceReadFromRegistry(hp_sndDevices, REG_CURRENT_USER, SND_DEVICES_REGISTRY_USER_BUFFER_SIZE, regVal) != OKAY)
				return(NOT_OKAY);

			if (wcscmp(regVal, L"") != 0)
			{
				swscanf(regVal, L"%d", &(cast_handle->bufferSizeMilliSecs));
			}
			else
			{
				// If no user setting, read the default buffer size setting.
				if (sndDeviceReadFromRegistry(hp_sndDevices, REG_LOCAL_MACHINE, SND_DEVICES_REGISTRY_DEFAULT_BUFFER_SIZE, regVal) != OKAY)
					return(NOT_OKAY);

				// If a value was read, set it, otherwise use the default.
				if (wcscmp(regVal, L"") != 0)
				{
					swscanf(regVal, L"%d", &(cast_handle->bufferSizeMilliSecs));
				}
			}

			// Check the ranges on the buffer size setting.
			if ((cast_handle->bufferSizeMilliSecs < SND_DEVICES_CAPTURE_BUFFER_MIN_SIZE_MILLI_SECS) || (cast_handle->bufferSizeMilliSecs >(SND_DEVICES_CAPTURE_BUFFER_MAX_SIZE_MILLI_SECS)))
				cast_handle->bufferSizeMilliSecs = SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS;

			// NOTE bufferSizeMilliSecs is the average bulk delay, actual buffer length is twice this, so use 500 in denoms to correct.
			cast_handle->hnsRequestedDurationCapture = (REFERENCE_TIME)((double)cast_handle->bufferSizeMilliSecs * (double)SND_DEVICES_REFTIMES_PER_SEC / 500.0);
			cast_handle->hnsRequestedDurationPlayback = (REFERENCE_TIME)((double)cast_handle->bufferSizeMilliSecs * (double)SND_DEVICES_REFTIMES_PER_SEC / 500.0);

			// PT-NOTE - in this non-dynamic version lock the allocation size to the max that would be needed
			//captureAllocSize  = (int)(48000  * 8 * (double)SND_DEVICES_CAPTURE_BUFFER_MAX_SIZE_MILLI_SECS * 1.001/500.0);
			//playbackAllocSize = (int)(192000 * 8 * (double)SND_DEVICES_CAPTURE_BUFFER_MAX_SIZE_MILLI_SECS * 1.001/500.0);

			// Note - with non-matched sample rates, in DoPlayback playback channels are temporarily copied back
			// to the capture buffer, but while still at capture sampling rate. So capture buffer must have size
			// for the same number of channels in the playback buffer.
			if ((cast_handle->wfxPlayback.nChannels > cast_handle->wfxCapture.nChannels) && (cast_handle->upsampleRatio > 1))
				captureBufferChannelsForAllocation = cast_handle->wfxPlayback.nChannels;
			else
				captureBufferChannelsForAllocation = cast_handle->wfxCapture.nChannels;

			captureAllocSize = (int)(cast_handle->wfxCapture.nSamplesPerSec  * captureBufferChannelsForAllocation  * (double)cast_handle->bufferSizeMilliSecs * 1.001 / 500.0);
			// BetterFxSound: the processing buffer holds captured frames (capture rate) in the processing format, which
			// may have more channels (a mono carrier is processed in stereo) and a higher rate than the carrier's.
			{
				DWORD allocRate = cast_handle->wfxPlayback.nSamplesPerSec;
				WORD allocChannels = cast_handle->wfxPlayback.nChannels;
				if (cast_handle->wfxCapture.nSamplesPerSec > allocRate)
					allocRate = cast_handle->wfxCapture.nSamplesPerSec;
				if (cast_handle->wfxDfxProcessing.nChannels > allocChannels)
					allocChannels = cast_handle->wfxDfxProcessing.nChannels;
				playbackAllocSize = (int)(allocRate * allocChannels * (double)cast_handle->bufferSizeMilliSecs * 1.001 / 500.0);
			}

			if (cast_handle->fCaptureBuf != NULL)
			{
				if (captureAllocSize != cast_handle->captureBufAllocSize)
				{
					free(cast_handle->fCaptureBuf);
					cast_handle->fCaptureBuf = (float *)calloc(captureAllocSize, sizeof(float));
				}
			}
			else
				cast_handle->fCaptureBuf = (float *)calloc(captureAllocSize, sizeof(float));

			cast_handle->captureBufAllocSize = captureAllocSize;

			if (cast_handle->fPlaybackBuf != NULL)
			{
				if (playbackAllocSize != cast_handle->playbackBufAllocSize)
				{
					free(cast_handle->fPlaybackBuf);
					free(cast_handle->fFilePlaybackBuf);
					cast_handle->fPlaybackBuf = (float *)calloc(playbackAllocSize, sizeof(float));
					cast_handle->fFilePlaybackBuf = (float *)calloc(playbackAllocSize, sizeof(float));
				}
			}
			else
			{
				cast_handle->fPlaybackBuf = (float *)calloc(playbackAllocSize, sizeof(float));
				cast_handle->fFilePlaybackBuf = (float *)calloc(playbackAllocSize, sizeof(float));
			}

			cast_handle->playbackBufAllocSize = playbackAllocSize;

			if ((cast_handle->fCaptureBuf == NULL) || (cast_handle->fPlaybackBuf == NULL) || (cast_handle->fFilePlaybackBuf == NULL))
				return(NOT_OKAY);

			// Do the final format dependent setup of capture and playback devices.
			if (sndDevicesFinalSetupCaptureDevice(hp_sndDevices, &resultFlag) != OKAY)
				return(NOT_OKAY);

			if (resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = resultFlag;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_SET_MUTE_FAILED);
			}

			if (sndDevicesFinalSetupPlaybackDevice(hp_sndDevices, &resultFlag) != OKAY)
				return(NOT_OKAY);

			if (resultFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				cast_handle->playbackSetupFailed = TRUE;
				*ipDfxDeviceEnabledFlag = IS_FALSE;
				*ip_status = resultFlag;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_SET_MUTE_FAILED);
			}

			cast_handle->ignoreDeviceCallbacks = FALSE;
			cast_handle->ignoreVolumeCallbacks = FALSE;
		}
	}

	SLOUT_FIRST_LINE(L"sndDevicesReInit():: Returns OKAY");

	return(OKAY);
}

// BetterFxSound: whether a difference between the device list and the system for this endpoint must stop the
// processing. Same rule as the device notification callbacks (sndDevicesCallbackChangeStopsProcessing()): the virtual
// device, the carrier, or any endpoint becoming active while there is no carrier.
static BOOL sndCheckDeviceChangeStopsProcessing(struct sndDevicesHdlType* cast_handle, const wchar_t* wcp_ID,
	const wchar_t* wcp_carrier_ID, BOOL b_becomes_active)
{
	if ((cast_handle->dfxDeviceID[0] != L'\0') && (wcscmp(wcp_ID, cast_handle->dfxDeviceID) == 0))
		return(TRUE);

	if (wcp_carrier_ID[0] == L'\0')
		return(b_becomes_active);

	return(wcscmp(wcp_ID, wcp_carrier_ID) == 0);
}

/*
* BetterFxSound: TRUE when the carrier's mix format, read now, has another channel count or rate than the one the
* carrier was opened with (wfxPlayback). FALSE when it is the same or cannot be read (an invalidated stream ends the
* processing thread by itself). Timer thread.
*/
static BOOL sndCheckCarrierFormatChanged(struct sndDevicesHdlType* cast_handle, IMMDeviceEnumerator* pEnumerator, const wchar_t* wcp_carrier_ID)
{
	IMMDevice* pDevice = NULL;
	IAudioClient* pClient = NULL;
	WAVEFORMATEX* pwfx = NULL;
	BOOL changed = FALSE;

	if ((pEnumerator == NULL) || (wcp_carrier_ID[0] == 0) || (cast_handle->wfxPlayback.nChannels == 0))
		return(FALSE);

	if (SUCCEEDED(pEnumerator->GetDevice(wcp_carrier_ID, &pDevice)) && (pDevice != NULL))
	{
		if (SUCCEEDED(pDevice->Activate(cast_handle->IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pClient)) && (pClient != NULL))
		{
			if (SUCCEEDED(pClient->GetMixFormat(&pwfx)) && (pwfx != NULL))
			{
				if ((pwfx->nChannels != cast_handle->wfxPlayback.nChannels) || (pwfx->nSamplesPerSec != cast_handle->wfxPlayback.nSamplesPerSec))
					changed = TRUE;
				CoTaskMemFree(pwfx);
			}
			pClient->Release();
		}
		pDevice->Release();
	}

	return(changed);
}

/*
* BetterFxSound: reports any difference between the device list read at the last enumeration and the system (count, IDs,
* states), so the caller reads the list again. The processing is only stopped when a difference concerns it (see
* sndCheckDeviceChangeStopsProcessing()), as the device notification callbacks do. Stopping it for any difference
* interrupted every output, at a moment unrelated to the change (opening the tray menu or the outputs list), for
* changes the callbacks had deliberately let through. Other differences mark the list stale, so the timer refreshes the
* outputs while processing.
*/
int PT_DECLSPEC sndCheckDeviceChanges(PT_HANDLE* hp_sndDevices, BOOL* bp_deviceChanged)
{
	struct sndDevicesHdlType* cast_handle;
	IMMDeviceEnumeratorPtr pEnumerator = NULL;
	IMMDeviceCollectionPtr pCollectionAllDevices = NULL;
	HRESULT hr;
	UINT deviceCount = 0;
	UINT i;
	int j;
	int listed_count;
	int carrier_num;
	BOOL deviceFound;
	BOOL stop_processing;
	BOOL read_all;
	BOOL list_differs;
	BOOL listed_seen[SND_DEVICES_MAX_NUM_DEVICES];
	wchar_t carrier_ID[PT_MAX_GENERIC_STRLEN];
	LPWSTR pID = NULL;

    *bp_deviceChanged = FALSE;

	cast_handle = (struct sndDevicesHdlType*)hp_sndDevices;
	if (cast_handle == NULL)
		return(NOT_OKAY);

	listed_count = cast_handle->totalNumDevices;
	if (listed_count < 0)
		listed_count = 0;
	if (listed_count > SND_DEVICES_MAX_NUM_DEVICES)
		listed_count = SND_DEVICES_MAX_NUM_DEVICES;
	for (j = 0; j < SND_DEVICES_MAX_NUM_DEVICES; j++)
		listed_seen[j] = FALSE;

	carrier_ID[0] = L'\0';
	carrier_num = cast_handle->playbackDeviceNum;
	if ((carrier_num >= 0) && (carrier_num < listed_count))
		wcscpy_s(carrier_ID, PT_MAX_GENERIC_STRLEN, cast_handle->pwszID[carrier_num]);

	stop_processing = FALSE;
	read_all = TRUE;
	list_differs = FALSE;

	hr = CoCreateInstance(cast_handle->CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, cast_handle->IID_IMMDeviceEnumerator, (void**)&pEnumerator);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_INSTANCE_CREATE_FAILED)

	// Enumerate all playback audio devices, "eRender" means look only for playback devices.
	hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &pCollectionAllDevices);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_ENUMERATE_FAILED)

	// Now get total number of playback devices
	hr = pCollectionAllDevices->GetCount(&deviceCount);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_GETCOUNT_FAILED)

	// BetterFxSound: sndDevices_GetAll() lists only the first SND_DEVICES_MAX_NUM_DEVICES endpoints, so only those are
	// compared. With more, the count always differed and the endpoints past the limit were never found in the list: the
	// list was read again on every tick, and stopped the processing on every check when one of them was the virtual device.
	if (deviceCount > SND_DEVICES_MAX_NUM_DEVICES)
		deviceCount = SND_DEVICES_MAX_NUM_DEVICES;

	// A different count means a device was added or removed (which one is found below).
	if (deviceCount != (UINT)listed_count)
	{
        *bp_deviceChanged = TRUE;
		list_differs = TRUE;
	}

	// Compare device IDs and states
	for (i = 0; i < deviceCount; i++)
	{
		IMMDevice* pDevice = NULL;
		hr = pCollectionAllDevices->Item(i, &pDevice);
		if (FAILED(hr) || pDevice == NULL)
		{
			read_all = FALSE;
			continue;
		}

		pID = NULL;
		hr = pDevice->GetId(&pID);
		if (FAILED(hr) || pID == NULL)
		{
			if (pDevice) pDevice->Release();
			read_all = FALSE;
			continue;
		}

		DWORD currentState = 0;
		hr = pDevice->GetState(&currentState);
		if (FAILED(hr))
			currentState = 0;

		deviceFound = FALSE;
		for (j = 0; j < listed_count; j++)
		{
			if (wcscmp(pID, cast_handle->pwszID[j]) == 0)
			{
				deviceFound = TRUE;
				listed_seen[j] = TRUE;

				// Device ID matched - now check if its state has changed
				if (SUCCEEDED(hr) && currentState != cast_handle->deviceState[j])
				{
					*bp_deviceChanged = TRUE;
					list_differs = TRUE;
					if (((currentState == DEVICE_STATE_ACTIVE) || (cast_handle->deviceState[j] == DEVICE_STATE_ACTIVE)) &&
						sndCheckDeviceChangeStopsProcessing(cast_handle, pID, carrier_ID, (currentState == DEVICE_STATE_ACTIVE)))
						stop_processing = TRUE;
				}

				break;
			}
		}

		if (!deviceFound)
		{
            *bp_deviceChanged = TRUE;
			list_differs = TRUE;
			if (sndCheckDeviceChangeStopsProcessing(cast_handle, pID, carrier_ID, (currentState == DEVICE_STATE_ACTIVE)))
				stop_processing = TRUE;
		}

		CoTaskMemFree(pID);
		pDevice->Release();
	}

	// Listed devices that are gone from the system (removed, or no longer active nor unplugged). When some device could
	// not be read, one of them may be the unread one, so it is only reported (the caller reads the list again), not
	// taken for gone: nor does it mark the list stale, as a device that keeps failing to be read would otherwise have
	// the timer refresh the outputs on every tick.
	for (j = 0; j < listed_count; j++)
	{
		// BetterFxSound: an entry sndDevices_GetAll() could not read has an empty ID and names no device. If its device
		// can be read now, it was not found in the list above, which reports the difference.
		if (cast_handle->pwszID[j][0] == L'\0')
			continue;

		if (!listed_seen[j])
		{
			*bp_deviceChanged = TRUE;
			if (read_all)
			{
				list_differs = TRUE;
				if (sndCheckDeviceChangeStopsProcessing(cast_handle, cast_handle->pwszID[j], carrier_ID, FALSE))
					stop_processing = TRUE;
			}
		}
	}

	// BetterFxSound: a playback endpoint's format changed since the last enumeration (see deviceFormatChanged): its
	// channel count may differ from the listed one, so the list is read again. That enumeration clears the flag.
	// The processing is stopped only when the carrier's format now differs (channel count or rate) from the one it was
	// opened with: the notification comes in bursts, or unchanged, for which it used to stop the processing every time.
	if (InterlockedCompareExchange(&(cast_handle->deviceFormatChanged), 0, 0) != 0)
	{
		*bp_deviceChanged = TRUE;
		list_differs = TRUE;
		if (sndCheckCarrierFormatChanged(cast_handle, pEnumerator, carrier_ID))
			stop_processing = TRUE;
	}

	// BetterFxSound: an active device whose channel count the last enumeration could not read is read again (the list
	// differs from what the system would give now), a few times; see sndDevicesGetChannelProbeIsDue().
	if (!list_differs)
	{
		BOOL probe_is_due = FALSE;
		if ((sndDevicesGetChannelProbeIsDue(hp_sndDevices, &probe_is_due) == OKAY) && probe_is_due)
		{
			*bp_deviceChanged = TRUE;
			list_differs = TRUE;
		}
	}

	if (stop_processing)
		InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
	else if (list_differs)
		InterlockedExchange(&(cast_handle->deviceListIsStale), 1);

	return(OKAY);
}

/*
* FUNCTION: sndDevicesGetChannelProbeIsDue()
* DESCRIPTION: BetterFxSound. TRUE when the last enumeration, at least SND_DEVICES_CHANNEL_PROBE_INTERVAL_MS ago, found
* an active device whose channel count it could not read (and had not read before), and that fewer than
* SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES due probes have tried to read (counted per device, channelCacheProbeRetries).
* Its count is then read again: such a device (being reconfigured as it arrives) is left out of the outputs while its
* count is unknown, and no notification follows once its format can be read. A device that keeps failing is no longer
* retried until it stops being active (or its count is read); another device still gets its own retries. A TRUE result
* marks the probe pending, so the next enumeration (the one the caller makes the timer start) counts as a retry.
*/
int PT_DECLSPEC sndDevicesGetChannelProbeIsDue(PT_HANDLE* hp_sndDevices, BOOL* bp_isDue)
{
	struct sndDevicesHdlType* cast_handle;

	*bp_isDue = FALSE;

	cast_handle = (struct sndDevicesHdlType*)hp_sndDevices;
	if (cast_handle == NULL)
		return(NOT_OKAY);

	if (!cast_handle->activeDeviceIsUnprobed)
		return(OKAY);

	if ((GetTickCount() - cast_handle->lastEnumerationTick) < (DWORD)SND_DEVICES_CHANNEL_PROBE_INTERVAL_MS)
		return(OKAY);

	InterlockedExchange(&(cast_handle->channelProbeIsPending), 1);
	*bp_isDue = TRUE;
	return(OKAY);
}
