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
#include <string>


#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

// This is for registering the device callbacks. For some reason the callbacks would crash
// when this was an object in the handle, perhaps because it was allocated?
//CsndDevicesMMNotificationClient g_DeviceEvents;

CsndDevicesMMNotificationClient::CsndDevicesMMNotificationClient() :_cRef(1),_pEnumerator(NULL)
{
}

CsndDevicesMMNotificationClient::~CsndDevicesMMNotificationClient()
{
	// For some reason even though _pEnumerator comes in here non-null, this crashes when called in windevcon.
  //SAFE_RELEASE(_pEnumerator)
}

// IUnknown methods -- AddRef, Release, and QueryInterface

ULONG STDMETHODCALLTYPE CsndDevicesMMNotificationClient::AddRef()
{
  return InterlockedIncrement(&_cRef);
}

ULONG STDMETHODCALLTYPE CsndDevicesMMNotificationClient::Release()
{
  ULONG ulRef = InterlockedDecrement(&_cRef);
  if (0 == ulRef)
  {
      delete this;
  }
  return ulRef;
}

HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::QueryInterface(
                          REFIID riid, VOID **ppvInterface)
{
  if (IID_IUnknown == riid)
  {
      AddRef();
      *ppvInterface = (IUnknown*)this;
  }
  else if (__uuidof(IMMNotificationClient) == riid)
  {
      AddRef();
      *ppvInterface = (IMMNotificationClient*)this;
  }
  else
  {
      *ppvInterface = NULL;
      return E_NOINTERFACE;
  }
  return S_OK;
}

// BetterFxSound: helpers for the device notification callbacks below. The callbacks run on the audio service's
// notification threads while the timer thread may be rewriting the device list (sndDevicesReInit(), sndDevices_GetAll()),
// so they read each shared index once, check its bounds, and only trust the list when enumerationSequence says it was
// not being rewritten during the read.

// The format ID of PKEY_AudioEngine_DeviceFormat ({f19f064d-082c-4e27-bc73-6882a1bb8e4c}, pid 0, mmdeviceapi.h),
// defined here so this file does not need initguid.h for it.
static const GUID kSndDevicesAudioEngineDeviceFormatFmtid =
	{ 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } };

// True for a capture (recording) endpoint. MMDevice endpoint IDs start with "{0.0.<flow>.", flow being 0 for render and
// 1 for capture. The callbacks must not release the last reference to an MMDevice API object, so they cannot open the
// endpoint to ask for its data flow. An ID in another format is treated as a playback endpoint.
static BOOL sndDevicesCallbackIsCaptureEndpoint(LPCWSTR pwstrDeviceId)
{
	if ((pwstrDeviceId != NULL) && (wcsncmp(pwstrDeviceId, L"{0.0.1.", 7) == 0))
		return(TRUE);

	return(FALSE);
}

// Looks the endpoint up in the playback device list read at the last enumeration. Returns FALSE when the list cannot
// be relied on (being rewritten, or reset by a reinit that has not enumerated yet). Otherwise returns TRUE with
// *ip_index set to the device's index (SND_DEVICES_DEVICE_NOT_PRESENT if it is not listed) and *dp_state to its state
// at that enumeration.
static BOOL sndDevicesCallbackFindDevice(struct sndDevicesHdlType *cast_handle, LPCWSTR pwstrDeviceId, int *ip_index, DWORD *dp_state)
{
	LONG seq_before;
	LONG seq_after;
	int total;
	int i;

	*ip_index = SND_DEVICES_DEVICE_NOT_PRESENT;
	*dp_state = 0;

	if (pwstrDeviceId == NULL)
		return(FALSE);

	seq_before = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
	if ((seq_before & 1) != 0)
		return(FALSE);

	total = *(volatile int *)&(cast_handle->totalNumDevices);
	if (total > SND_DEVICES_MAX_NUM_DEVICES)
		total = SND_DEVICES_MAX_NUM_DEVICES;

	for (i = 0; i < total; i++)
	{
		if (wcsncmp(cast_handle->pwszID[i], pwstrDeviceId, PT_MAX_GENERIC_STRLEN) == 0)
		{
			*ip_index = i;
			*dp_state = cast_handle->deviceState[i];
			break;
		}
	}

	seq_after = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
	if ((seq_after != seq_before) || (total <= 0))
	{
		*ip_index = SND_DEVICES_DEVICE_NOT_PRESENT;
		*dp_state = 0;
		return(FALSE);
	}

	return(TRUE);
}

// True when a change of this playback endpoint must stop the processing, which interrupts the carrier and every
// additional output: the endpoint is the virtual device or the carrier, or there is no carrier and the endpoint becomes
// usable (b_becomes_active). Any other endpoint (an unused HDMI monitor going to sleep, a spare DAC, another app's
// virtual device, an additional output) is left to the timer, which reads the device list again and refreshes the
// outputs without stopping (deviceListIsStale). When the list cannot be relied on (an enumeration is rewriting it), only
// the virtual device stops the processing: any other endpoint marks the list stale, and the timer then compares the
// list with the system (sndCheckDeviceChanges(), same rule), which stops the processing if the change concerns the
// carrier. If that enumeration already read the change, the comparison finds no difference: sndDevices_GetAll() then
// stops the processing itself when the carrier or the virtual device is gone or changed to or from active. Stopping
// on an unreliable list let the rest of a notification burst (a device connect raises about ten state changes) land
// in the enumeration started by its first one and stop the processing anyway. A stale flag set during a reinit's
// enumeration is kept, the reinit clears it before enumerating.
static BOOL sndDevicesCallbackChangeStopsProcessing(struct sndDevicesHdlType *cast_handle, LPCWSTR pwstrDeviceId, BOOL b_becomes_active)
{
	LONG seq_before;
	LONG seq_after;
	int total;
	int carrier_num;
	BOOL is_carrier;

	if (pwstrDeviceId == NULL)
		return(TRUE);

	if ((cast_handle->dfxDeviceID[0] != L'\0') &&
		(wcsncmp(pwstrDeviceId, cast_handle->dfxDeviceID, PT_MAX_GENERIC_STRLEN) == 0))
		return(TRUE);

	seq_before = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
	if ((seq_before & 1) != 0)
		return(FALSE);

	total = *(volatile int *)&(cast_handle->totalNumDevices);
	if (total > SND_DEVICES_MAX_NUM_DEVICES)
		total = SND_DEVICES_MAX_NUM_DEVICES;
	carrier_num = *(volatile int *)&(cast_handle->playbackDeviceNum);

	is_carrier = FALSE;
	if ((carrier_num >= 0) && (carrier_num < total))
		is_carrier = (wcsncmp(cast_handle->pwszID[carrier_num], pwstrDeviceId, PT_MAX_GENERIC_STRLEN) == 0);

	seq_after = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
	if (seq_after != seq_before)
		return(FALSE);

	if ((carrier_num < 0) || (carrier_num >= total))
		return(b_becomes_active);

	return(is_carrier);
}

// Records the new Windows default in defaultDeviceNum. The sequence is taken odd from an even value for the store, as
// sndDevices_GetAll() does, so an enumeration cannot publish its own default in between and then have it overwritten
// with an index of the list it replaced. If an enumeration is under way, nothing is stored: it publishes the default it
// reads (and when that read came before this change, the stop flag makes the reinit run again).
static void sndDevicesCallbackPublishDefault(struct sndDevicesHdlType *cast_handle, LPCWSTR pwstrDeviceId)
{
	LONG sequence;
	int total;
	int device_num;
	int i;

	sequence = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
	if ((sequence & 1) != 0)
		return;
	if (InterlockedCompareExchange(&(cast_handle->enumerationSequence), sequence + 1, sequence) != sequence)
		return;

	total = cast_handle->totalNumDevices;
	if (total > SND_DEVICES_MAX_NUM_DEVICES)
		total = SND_DEVICES_MAX_NUM_DEVICES;

	device_num = SND_DEVICES_DEVICE_NOT_PRESENT;
	for (i = 0; i < total; i++)
	{
		if (wcsncmp(cast_handle->pwszID[i], pwstrDeviceId, PT_MAX_GENERIC_STRLEN) == 0)
		{
			device_num = i;
			break;
		}
	}

	// A reinit that has reset the list (total not set yet) publishes the default itself.
	if (total > 0)
		cast_handle->defaultDeviceNum = device_num;

	InterlockedExchange(&(cast_handle->enumerationSequence), sequence + 2);
}

// Callback methods for device-event notifications.

HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::OnDefaultDeviceChanged(
                          EDataFlow flow, ERole role,
                          LPCWSTR pwstrDeviceId)
{  
  struct sndDevicesHdlType *cast_handle;
  BOOL is_virtual_device;
  int default_num;
    
  cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

  if (cast_handle == NULL)
    return(S_OK);

  if (pwstrDeviceId == NULL)
	  return(S_OK);

  // BetterFxSound: only the default playback device for multimedia (the one GetAll reads and the processing follows)
  // matters. Changes of the default recording device or of the communications device used to overwrite
  // defaultDeviceNum, so the virtual device was no longer seen as the default.
  if( (flow != eRender) || (role != eMultimedia) )
	  return(S_OK);

  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDefaultDeviceChanged() enters");

  // BetterFxSound: the virtual device is recognized by its ID, which is kept across reinits. This notification used
  // to be dropped while a reinit had reset dfxDeviceNum, so a default change made then was never acted on.
  is_virtual_device = (cast_handle->dfxDeviceID[0] != L'\0') &&
	  (wcsncmp(pwstrDeviceId, cast_handle->dfxDeviceID, PT_MAX_GENERIC_STRLEN) == 0);

  // Don't think we need to kill process thread if DFX itself is selected as default from within our code
  if( (!is_virtual_device) && (cast_handle->ignoreDeviceCallbacks == FALSE) )
  {
	  // If one of the DFX capture devices was set as the default and the default device has been changed, force a re-initialization.
	  // Note- setting the flag only if a DFX device was the default is filtering out callbacks we want made.
	  //if( (cast_handle->dfxDeviceNum == cast_handle->defaultDeviceNum) || (cast_handle->dfx48DeviceNum == cast_handle->defaultDeviceNum) )
	  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDefaultDeviceChanged() setting processing thread kill flag");

	  //std::wstring temp_string(pwstrDeviceId);
	  /* Set the newly targeted playback device as the default.  It will then automatically become the targeted device */
	  //if (sndDevicesSetDeviceType(g_sndDevicesCallbacks_hdl, SND_DEVICES_DEFAULT, &temp_string[0], &i_resultFlag) != OKAY)
		//  return(NOT_OKAY);

	  InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
  }

  // Ignore the notification for the device selected in the application.
  // BetterFxSound: the index is read once and bounds checked, a reinit can reset it at any time.
  default_num = *(volatile int *)&(cast_handle->defaultDeviceNum);
  if( (default_num >= 0) && (default_num < SND_DEVICES_MAX_NUM_DEVICES) &&
	  (wcsncmp(pwstrDeviceId, cast_handle->pwszID[default_num], PT_MAX_GENERIC_STRLEN) == 0) )
  {
	  return(S_OK);
  }

  // BetterFxSound: defaultDeviceNum is only written while no enumeration can rewrite the list or publish its own
  // default, see sndDevicesCallbackPublishDefault().
  sndDevicesCallbackPublishDefault(cast_handle, pwstrDeviceId);

  if (is_virtual_device)
	  return(S_OK);

  if (cast_handle->deviceChangeCallback != NULL)
	  cast_handle->deviceChangeCallback();

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
  struct sndDevicesHdlType *cast_handle;
    
  cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

  if (cast_handle == NULL)
    return(S_OK);

  // BetterFxSound: a recording endpoint (a microphone, a webcam, the hands-free side of a headset) is never used for
  // playback, so it no longer stops the processing and interrupts every output.
  if (sndDevicesCallbackIsCaptureEndpoint(pwstrDeviceId))
	  return(S_OK);
	
  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceAdded() enters");

  // These callbacks only appear to get hit when a device is added that didn't already have a driver installed.
  // BetterFxSound: only the virtual device, or any device while there is no carrier, stops the processing. Another
  // device (a new output, or a better carrier the controller may switch to) is picked up by the timer without a stop.
  if( cast_handle->ignoreDeviceCallbacks == FALSE )
  {
	  if( sndDevicesCallbackChangeStopsProcessing(cast_handle, pwstrDeviceId, TRUE) )
	  {
		  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceAdded() setting processing thread kill flag");

		  InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
	  }
	  else
		  InterlockedExchange(&(cast_handle->deviceListIsStale), 1);
  }

  if (cast_handle->deviceChangeCallback != NULL)
	  cast_handle->deviceChangeCallback();

  return S_OK;
};

HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
  struct sndDevicesHdlType *cast_handle;
  int device_num;
  DWORD device_state;
    
  cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

  if (cast_handle == NULL)
    return(S_OK);

  // BetterFxSound: a recording endpoint is never used for playback, see OnDeviceAdded().
  if (sndDevicesCallbackIsCaptureEndpoint(pwstrDeviceId))
	  return(S_OK);

  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceRemoved() enters");

  // These callbacks only appear to get hit when a device is added that didn't already have a driver installed.
  // BetterFxSound: a playback endpoint that was not in the device list (neither active nor unplugged) was not in use,
  // so its removal does not stop the processing. Nor does a listed one that is neither the virtual device nor the
  // carrier: the timer reads the list again and drops it from the outputs.
  if( cast_handle->ignoreDeviceCallbacks == FALSE )
  {
	  if( !sndDevicesCallbackFindDevice(cast_handle, pwstrDeviceId, &device_num, &device_state) ||
		  (device_num != SND_DEVICES_DEVICE_NOT_PRESENT) )
	  {
		  if( sndDevicesCallbackChangeStopsProcessing(cast_handle, pwstrDeviceId, FALSE) )
		  {
			  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceRemoved() setting processing thread kill flag");

			  InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
		  }
		  else
			  InterlockedExchange(&(cast_handle->deviceListIsStale), 1);
	  }
  }

  if (cast_handle->deviceChangeCallback != NULL)
	  cast_handle->deviceChangeCallback();

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::OnDeviceStateChanged(
                          LPCWSTR pwstrDeviceId,
                          DWORD dwNewState)
{
  struct sndDevicesHdlType *cast_handle;
  BOOL state_changed;
  BOOL stop_processing;
  int device_num;
  DWORD device_state;
    
  cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

  if (cast_handle == NULL)
    return(S_OK);

  //SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceStateChanged() enters");

  // BetterFxSound: a recording endpoint is never used for playback, see OnDeviceAdded(). Bluetooth headsets toggle
  // their hands-free endpoints whenever an app opens or closes the microphone.
  if (sndDevicesCallbackIsCaptureEndpoint(pwstrDeviceId))
	  return(S_OK);

  // DEVICE_STATE_ACTIVE: when connecting device, this gets hit around 10 times.
  // DEVICE_STATE_NOTPRESENT: when disconnecting device, this gets hit around 12 times.

  // Ignore identical callbacks from the same guid. Since device must be disconnected after being connected,
  // we should never miss a connect or disconnect event.
  if( pwstrDeviceId != NULL )
		if( (wcscmp(pwstrDeviceId, cast_handle->lastDeviceAddCallbackGuid) == 0) && ( dwNewState == cast_handle->lastDeviceAddCallbackGuidtype ) )
		{
			SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceStateChanged() ignoring identical callback");
		   return(S_OK);
		}

  if( pwstrDeviceId == NULL )
		wcscpy(cast_handle->lastDeviceAddCallbackGuid, L"");
  else
	   wcsncpy_s(cast_handle->lastDeviceAddCallbackGuid, PT_MAX_GENERIC_STRLEN, pwstrDeviceId, _TRUNCATE);

  cast_handle->lastDeviceAddCallbackGuidtype = dwNewState;

  /*
  * BetterFxSound: only stop the processing (which interrupts every output) when the change can matter to it: the
  * endpoint is the virtual device or the carrier and changes state from or to active, or there is no carrier and an
  * endpoint becomes active (sndDevicesCallbackChangeStopsProcessing()). A state the last enumeration already read is
  * ignored. Any other change (an unused HDMI monitor going to sleep, a spare DAC, a new or vanished additional output)
  * only marks the device list stale: the timer reads it again and refreshes the outputs, without a stop. When the
  * list cannot be relied on (an enumeration is under way), the change is treated as one, and
  * sndDevicesCallbackChangeStopsProcessing() leaves it to the timer unless the endpoint is the virtual device.
  */
  state_changed = TRUE;
  stop_processing = TRUE;
  if (!sndDevicesCallbackFindDevice(cast_handle, pwstrDeviceId, &device_num, &device_state))
	  stop_processing = sndDevicesCallbackChangeStopsProcessing(cast_handle, pwstrDeviceId, (dwNewState == DEVICE_STATE_ACTIVE));
  else
  {
	  if (device_num != SND_DEVICES_DEVICE_NOT_PRESENT)
	  {
		  state_changed = (dwNewState != device_state);
		  stop_processing = state_changed &&
			  ((dwNewState == DEVICE_STATE_ACTIVE) || (device_state == DEVICE_STATE_ACTIVE));
	  }
	  else
	  {
		  // An unlisted endpoint joins the list (enumerated as active or unplugged) in these states only.
		  state_changed = (dwNewState == DEVICE_STATE_ACTIVE) || (dwNewState == DEVICE_STATE_UNPLUGGED);
		  stop_processing = (dwNewState == DEVICE_STATE_ACTIVE);
	  }

	  if (stop_processing)
		  stop_processing = sndDevicesCallbackChangeStopsProcessing(cast_handle, pwstrDeviceId, (dwNewState == DEVICE_STATE_ACTIVE));
  }

  if( cast_handle->ignoreDeviceCallbacks == FALSE )
  {
	  if( stop_processing )
	  {
		  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnDeviceStateChanged() setting processing thread kill flag");

		  InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
	  }
	  else if( state_changed )
		  InterlockedExchange(&(cast_handle->deviceListIsStale), 1);
  }

  if (cast_handle->deviceChangeCallback != NULL)
	  cast_handle->deviceChangeCallback();

  return S_OK;
}

// NOTE - On Vista/Win7 this callback would only get hit when a legitmate "property" of a device changed,
// such as samp freq, num channels. On Win8 this callback is getting hit when the mute setting is changed.
// The thread stop set has been disabled to avoid Win8 mute problems as any change of these properties should
// cause the capture thread to exit with an error condition.
HRESULT STDMETHODCALLTYPE CsndDevicesMMNotificationClient::OnPropertyValueChanged(
                          LPCWSTR pwstrDeviceId,
                          const PROPERTYKEY key)
{
  struct sndDevicesHdlType *cast_handle;

  cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

  if (cast_handle == NULL)
    return(S_OK);

  /*
  * BetterFxSound: a playback endpoint's format changed (PKEY_AudioEngine_DeviceFormat). Its state stays the same, so
  * nothing else tells: a Bluetooth headset that went mono while an app had its microphone open stayed listed as mono
  * (hidden from the outputs, not routed) once it was back to stereo, until some unrelated device event. The device list
  * is read again by the timer (deviceFormatChanged, see sndCheckDeviceChanges()); the carrier's own format change
  * stops the processing, so the reinit opens it with its new format. Every other property (the mute setting on
  * Windows 8 and later, names, icons) is still ignored. The virtual device's format is set by the reinit itself.
  */
  if( (pwstrDeviceId != NULL) && (cast_handle->ignoreDeviceCallbacks == FALSE) &&
	  IsEqualGUID(key.fmtid, kSndDevicesAudioEngineDeviceFormatFmtid) && (key.pid == 0) &&
	  !sndDevicesCallbackIsCaptureEndpoint(pwstrDeviceId) &&
	  !((cast_handle->dfxDeviceID[0] != L'\0') && (wcsncmp(pwstrDeviceId, cast_handle->dfxDeviceID, PT_MAX_GENERIC_STRLEN) == 0)) )
  {
	  SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnPropertyValueChanged() a playback device format changed");

	  InterlockedExchange(&(cast_handle->deviceFormatChanged), 1);
	  if( sndDevicesCallbackChangeStopsProcessing(cast_handle, pwstrDeviceId, FALSE) )
		  InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
	  else
		  InterlockedExchange(&(cast_handle->deviceListIsStale), 1);

	  if (cast_handle->deviceChangeCallback != NULL)
		  cast_handle->deviceChangeCallback();
  }

	/*
	struct sndDevicesHdlType *cast_handle;
	wchar_t guidID[PT_MAX_GENERIC_STRLEN];
	int deviceNum;
	 
	cast_handle = (struct sndDevicesHdlType *)g_sndDevicesCallbacks_hdl;

	if (cast_handle == NULL)
	 return(S_OK);

	// Only force a reset if a property value changed on the playback or capture devices.
	if( cast_handle->ignoreDeviceCallbacks == FALSE )
	{
	  if( pwstrDeviceId != NULL )	// Sometimes this comes in with a null value.
	  {
		  wcscpy(guidID, pwstrDeviceId);

		  if( sndDevices_UtilsGetIndexFromID(g_sndDevicesCallbacks_hdl, guidID, &deviceNum) == OKAY )
			  if( (deviceNum == cast_handle->captureDeviceNum) || (deviceNum == cast_handle->playbackDeviceNum) )
				 cast_handle->stopAudioCaptureAndPlaybackLoop = 1;
	  }
	}
	*/

	//SLOUT_FIRST_LINE(L"CsndDevicesMMNotificationClient::OnPropertyValueChanged() enters");

	return S_OK;
}

// Function to be called from sndDevicesInit to pass in allocated handle for by methods in this class.
void CsndDevicesMMNotificationClient::SetPtHandle(PT_HANDLE *sndDevices_hdl)
{
	g_sndDevicesCallbacks_hdl = sndDevices_hdl;
}
