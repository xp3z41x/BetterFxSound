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

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>

#include <mmreg.h>
#include <initguid.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>

#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"
#include "pstr.h"

//extern CsndDevicesMMNotificationClient g_DeviceEvents;	// This is for registering the device callbacks.

static int sndDevices_GetAllDevices(PT_HANDLE* hp_sndDevices, int* ip_num_devices);

// BetterFxSound: copies the ID of the listed device at i_index, or an empty string when the index is not in the list.
static void sndDevices_GetAllCopyListedID(struct sndDevicesHdlType* cast_handle, int i_index, wchar_t* wcp_ID)
{
	wcp_ID[0] = L'\0';
	if ((i_index >= 0) && (i_index < cast_handle->totalNumDevices) && (i_index < SND_DEVICES_MAX_NUM_DEVICES))
		wcscpy_s(wcp_ID, PT_MAX_GENERIC_STRLEN, cast_handle->pwszID[i_index]);
}

// BetterFxSound: the index of the device with this ID in the list just read, SND_DEVICES_DEVICE_NOT_PRESENT if none.
static int sndDevices_GetAllFindListedID(struct sndDevicesHdlType* cast_handle, const wchar_t* wcp_ID)
{
	int i;

	if (wcp_ID[0] == L'\0')
		return(SND_DEVICES_DEVICE_NOT_PRESENT);

	for (i = 0; (i < cast_handle->totalNumDevices) && (i < SND_DEVICES_MAX_NUM_DEVICES); i++)
	{
		if (wcscmp(cast_handle->pwszID[i], wcp_ID) == 0)
			return(i);
	}

	return(SND_DEVICES_DEVICE_NOT_PRESENT);
}

// BetterFxSound: true when the device listed at i_before_index before an enumeration (with dw_before_state) is gone from
// the list just read, or its state changed to or from active. The same rule as sndCheckDeviceChanges() for a listed device.
static BOOL sndDevices_GetAllListedDeviceChanged(struct sndDevicesHdlType* cast_handle, const wchar_t* wcp_ID,
	int i_before_index, DWORD dw_before_state)
{
	int i_after_index;
	DWORD dw_after_state;

	if ((wcp_ID[0] == L'\0') || (i_before_index == SND_DEVICES_DEVICE_NOT_PRESENT))
		return(FALSE);

	i_after_index = sndDevices_GetAllFindListedID(cast_handle, wcp_ID);
	if (i_after_index == SND_DEVICES_DEVICE_NOT_PRESENT)
		return(TRUE);

	dw_after_state = cast_handle->deviceState[i_after_index];
	return((dw_after_state != dw_before_state) &&
		((dw_after_state == DEVICE_STATE_ACTIVE) || (dw_before_state == DEVICE_STATE_ACTIVE)));
}

// Gets an array of objects for all audio devices and stores indexes to the default device and DFX device if installed.
// BetterFxSound: enumerationSequence is odd while the list is rewritten, see sndDevicesHdlType.
int PT_DECLSPEC sndDevices_GetAll(PT_HANDLE* hp_sndDevices, int* ip_num_devices)
{
	struct sndDevicesHdlType* cast_handle;
	int i_return;
	LONG sequence;
	wchar_t wcp_playback_ID[PT_MAX_GENERIC_STRLEN];
	wchar_t wcp_capture_ID[PT_MAX_GENERIC_STRLEN];
	wchar_t wcp_prior_default_ID[PT_MAX_GENERIC_STRLEN];
	wchar_t wcp_virtual_ID[PT_MAX_GENERIC_STRLEN];
	int i_playback_before;
	int i_virtual_before;
	DWORD dw_playback_state_before;
	DWORD dw_virtual_state_before;
	BOOL b_stop_processing;

	cast_handle = (struct sndDevicesHdlType*)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	// BetterFxSound: the rewrite starts only from an even sequence, so it never overlaps a device notification callback
	// publishing defaultDeviceNum (it holds the sequence odd for a few microseconds) or another enumeration.
	for (;;)
	{
		sequence = InterlockedCompareExchange(&(cast_handle->enumerationSequence), 0, 0);
		if (((sequence & 1) == 0) &&
			(InterlockedCompareExchange(&(cast_handle->enumerationSequence), sequence + 1, sequence) == sequence))
			break;
		Sleep(1);
	}

	// BetterFxSound: this enumeration reads every device's format, so a format change notified before it is covered.
	// One notified from here on sets the flag again and is read by a later enumeration.
	InterlockedExchange(&(cast_handle->deviceFormatChanged), 0);

	// BetterFxSound: the list can be read again while processing (a device change that does not stop it, the GUI's
	// getSoundDevices()), and its order can change. The carrier, capture and prior default indexes follow their devices'
	// IDs, so they keep naming the same devices (or none, if they are gone). A reinit has reset them, so they stay unset then.
	sndDevices_GetAllCopyListedID(cast_handle, cast_handle->playbackDeviceNum, wcp_playback_ID);
	sndDevices_GetAllCopyListedID(cast_handle, cast_handle->captureDeviceNum, wcp_capture_ID);
	sndDevices_GetAllCopyListedID(cast_handle, cast_handle->priorDefaultDeviceNum, wcp_prior_default_ID);

	// BetterFxSound: a change of the carrier or the virtual device always stops the processing. A notification that
	// arrives while this enumeration holds the list only marks it stale, and this enumeration may already read the
	// change, after which the timer's comparison with the system finds no difference and would not stop either. So the
	// carrier's and the virtual device's entries before and after the read are compared here too. Only while a carrier is
	// set: a reinit unsets it before enumerating, so the changes that reinit is for do not stop the processing it starts.
	i_playback_before = SND_DEVICES_DEVICE_NOT_PRESENT;
	dw_playback_state_before = 0;
	wcp_virtual_ID[0] = L'\0';
	i_virtual_before = SND_DEVICES_DEVICE_NOT_PRESENT;
	dw_virtual_state_before = 0;
	if (wcp_playback_ID[0] != L'\0')
	{
		i_playback_before = cast_handle->playbackDeviceNum;
		dw_playback_state_before = cast_handle->deviceState[i_playback_before];

		wcscpy_s(wcp_virtual_ID, PT_MAX_GENERIC_STRLEN, cast_handle->dfxDeviceID);
		i_virtual_before = sndDevices_GetAllFindListedID(cast_handle, wcp_virtual_ID);
		if (i_virtual_before != SND_DEVICES_DEVICE_NOT_PRESENT)
			dw_virtual_state_before = cast_handle->deviceState[i_virtual_before];
	}

	i_return = sndDevices_GetAllDevices(hp_sndDevices, ip_num_devices);

	b_stop_processing =
		sndDevices_GetAllListedDeviceChanged(cast_handle, wcp_playback_ID, i_playback_before, dw_playback_state_before) ||
		sndDevices_GetAllListedDeviceChanged(cast_handle, wcp_virtual_ID, i_virtual_before, dw_virtual_state_before);

	cast_handle->playbackDeviceNum = sndDevices_GetAllFindListedID(cast_handle, wcp_playback_ID);
	cast_handle->captureDeviceNum = sndDevices_GetAllFindListedID(cast_handle, wcp_capture_ID);
	cast_handle->priorDefaultDeviceNum = sndDevices_GetAllFindListedID(cast_handle, wcp_prior_default_ID);

	if (b_stop_processing)
		InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);

	InterlockedIncrement(&(cast_handle->enumerationSequence));

	return(i_return);
}

// BetterFxSound: the index of this device ID in the channel cache written by the previous enumeration, -1 if none.
static int sndDevices_GetAllFindCachedID(struct sndDevicesHdlType* cast_handle, const wchar_t* wcp_ID)
{
	int k;

	if (wcp_ID[0] == L'\0')
		return(-1);

	for (k = 0; (k < cast_handle->channelCacheCount) && (k < SND_DEVICES_MAX_NUM_DEVICES); k++)
	{
		if (wcscmp(cast_handle->channelCacheID[k], wcp_ID) == 0)
			return(k);
	}

	return(-1);
}

// BetterFxSound: the channel count of entry i when its format is not read by this enumeration (its properties or its
// format cannot be read right now). An active device (being reconfigured: a format change, a Bluetooth reconnection, a
// monitor waking up) keeps the count it had: with none it looked absent, or mono, to the controller while the engine
// still played to it. One never read before gets 0 (unknown, neither present as an output nor mono) and is read again
// shortly (sndDevicesGetChannelProbeIsDue()), up to SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES due probes for this device:
// one that can never be read does not use up the retries of another. A device that is not active is read afresh (with
// new retries) once it is active again.
static void sndDevices_GetAllKeepUnreadChannelCount(struct sndDevicesHdlType* cast_handle, int i, int* ip_known_num_channel,
	int* ip_probe_retries, BOOL b_is_probe, BOOL* bp_active_device_is_unprobed)
{
	if (cast_handle->deviceState[i] == DEVICE_STATE_ACTIVE)
	{
		cast_handle->deviceNumChannel[i] = ip_known_num_channel[i];
		if (ip_known_num_channel[i] <= 0)
		{
			if (b_is_probe && (ip_probe_retries[i] < SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES))
				ip_probe_retries[i]++;
			if (ip_probe_retries[i] < SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES)
				*bp_active_device_is_unprobed = TRUE;
		}
	}
	else
	{
		cast_handle->deviceNumChannel[i] = 0;
		ip_known_num_channel[i] = 0;
		ip_probe_retries[i] = 0;
	}
}

// BetterFxSound: an enumeration that stops part way keeps only the entries it has read completely, so the list never
// holds entries of an earlier enumeration (sndCheckDeviceChanges() then finds the difference and the list is read again).
// ip_num_devices is usually &totalNumDevices itself.
static void sndDevices_GetAllKeepEntries(struct sndDevicesHdlType* cast_handle, int* ip_num_devices, int i_count)
{
	*ip_num_devices = i_count;
	cast_handle->totalNumDevices = i_count;
}

static int sndDevices_GetAllDevices(PT_HANDLE* hp_sndDevices, int* ip_num_devices)
{
	int default_device_num = SND_DEVICES_DEVICE_NOT_PRESENT;	// BetterFxSound: published once the enumeration is done
	HRESULT hr;
	IMMDevicePtr pDefaultDevice = NULL;
	IMMDeviceEnumeratorPtr pEnumerator = NULL;
	IMMDeviceCollectionPtr pCollectionAllDevices;
	UINT  deviceCount;
	IPropertyStorePtr pProps = NULL;
	LPWSTR pwszIDdefault = NULL;
	PROPVARIANT FriendlyName;
	PROPVARIANT DeviceDesc;
	ULONG i;
	WAVEFORMATEX wfx;
	int k;
	int i_found_start_location;
	int i_found_dfx_string;
	int resultFlag;
	int known_num_channel[SND_DEVICES_MAX_NUM_DEVICES];	// BetterFxSound: channel counts actually read (or kept), 0 if not
	int probe_retries[SND_DEVICES_MAX_NUM_DEVICES];		// BetterFxSound: per device, see channelCacheProbeRetries
	int cache_index;
	BOOL b_active_device_is_unprobed;
	BOOL b_is_probe;

	EDataFlow flow = eRender;
	ERole role = eMultimedia;

	struct sndDevicesHdlType* cast_handle;

	cast_handle = (struct sndDevicesHdlType*)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	// Release any IMMDevice handles left over from a previous enumeration before they are
	// overwritten below. sndDevices_GetAll can be called repeatedly (the reinit loop calls
	// it up to 20 times per sndDevicesReInit, and the GUI calls it on device changes), so
	// every stale handle would otherwise be leaked. pCaptureDevice/pPlaybackDevice alias
	// pAllDevices[] entries (no separate reference) and are only used during setup on the
	// GUI thread - never by the audio processing thread - so NULL them here too.
	for (i = 0; i < SND_DEVICES_MAX_NUM_DEVICES; i++)
	{
		if (cast_handle->pAllDevices[i] != NULL)
		{
			cast_handle->pAllDevices[i]->Release();
			cast_handle->pAllDevices[i] = NULL;
		}
		known_num_channel[i] = 0;
		probe_retries[i] = 0;
	}
	cast_handle->pCaptureDevice = NULL;
	cast_handle->pPlaybackDevice = NULL;
	cast_handle->enumeratedDefaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	// BetterFxSound: nor does the default index of an earlier list survive an enumeration that stops part way. The device
	// notification callbacks cannot publish it meanwhile (enumerationSequence is odd).
	cast_handle->defaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;

	// BetterFxSound: whether this enumeration was asked for by a due channel probe (sndDevicesGetChannelProbeIsDue()).
	// Only such an enumeration counts as a retry for a device whose channel count is still unknown.
	b_is_probe = (InterlockedExchange(&(cast_handle->channelProbeIsPending), 0) != 0);

	// BetterFxSound: the list of previous real devices, for the new device rule, is no longer copied here: any enumeration
	// (the GUI's, a refresh) hid a device added since the last reinit from it. sndDevicesImplementDeviceRules() keeps it.
	cast_handle->numRealDevices = 0;

	hr = CoCreateInstance(cast_handle->CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, cast_handle->IID_IMMDeviceEnumerator, (void**)&pEnumerator);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_INSTANCE_CREATE_FAILED)

		// Enumerate all playback audio devices, "eRender" means look only for playback devices.
		hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &pCollectionAllDevices);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_ENUMERATE_FAILED)

		// Now get total number of playback devices
		hr = pCollectionAllDevices->GetCount(&deviceCount);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_GETCOUNT_FAILED)

	// BetterFxSound: every per-device array holds SND_DEVICES_MAX_NUM_DEVICES entries.
	if (deviceCount > SND_DEVICES_MAX_NUM_DEVICES)
		deviceCount = SND_DEVICES_MAX_NUM_DEVICES;

	cast_handle->totalNumDevices = deviceCount;
	*ip_num_devices = deviceCount;

	// There are no active sound devices and thus also no default device.
	if (cast_handle->totalNumDevices <= 0)
	{
		cast_handle->numRealDevices = 0;
		cast_handle->dfxDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
		cast_handle->defaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
		cast_handle->enumeratedDefaultDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
		for (k = 0; k < SND_DEVICES_MAX_NUM_DEVICES; k++)
			cast_handle->pwszIDRealDevices[k] = NULL;
		cast_handle->activeDeviceIsUnprobed = FALSE;
		cast_handle->lastEnumerationTick = GetTickCount();
		return(OKAY);
	}

	// First get the ID string for the current default playback device.
	// BetterFxSound: there may be none (only unplugged endpoints), which is not a reason to leave the entries unread.
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDefaultDevice);
	if (SUCCEEDED(hr) && (pDefaultDevice != NULL))
	{
		hr = pDefaultDevice->GetId(&pwszIDdefault);
		if (FAILED(hr))
			pwszIDdefault = NULL;
	}

	// -------------------------------------------------------------------------
	// Pass 1: Retrieve all device handles, IDs, names, and identify the DFX
	//         device index before classifying any device as "real".
	// -------------------------------------------------------------------------
	cast_handle->dfxDeviceNum = SND_DEVICES_DEVICE_NOT_PRESENT;
	b_active_device_is_unprobed = FALSE;

	for (i = 0; i < deviceCount; i++)
	{
		// BetterFxSound: every field of the entry is written on every path below, so it never keeps another device's
		// values from an earlier enumeration. An endpoint that cannot be read (being removed while the list is read)
		// keeps an empty ID and state 0, and the enumeration goes on: stopping there cut the list short, so a carrier or
		// virtual device listed after it looked gone (a reinit, a dropout on every output). An entry with an empty ID is
		// no device: lookups by ID never match it, and it is not listed to the controller nor as a real device.
		cast_handle->pwszID[i][0] = L'\0';
		cast_handle->deviceState[i] = 0;
		wcscpy_s(cast_handle->deviceFriendlyName[i], PT_MAX_GENERIC_STRLEN, L"Unknown");
		wcscpy_s(cast_handle->deviceDescription[i], PT_MAX_GENERIC_STRLEN, L"Unknown");
		wcscpy_s(cast_handle->deviceFormFactor[i], PT_MAX_GENERIC_STRLEN, L"Unknown");
		cast_handle->deviceNumChannel[i] = 1;

		hr = pCollectionAllDevices->Item(i, &(cast_handle->pAllDevices[i]));
		if (FAILED(hr) || (cast_handle->pAllDevices[i] == NULL))
		{
			cast_handle->pAllDevices[i] = NULL;
			continue;
		}

		// Get and store the endpoint ID string for this device.
		LPWSTR pID = NULL;
		hr = cast_handle->pAllDevices[i]->GetId(&pID);
		if (FAILED(hr) || (pID == NULL))
		{
			CoTaskMemFree(pID);
			continue;
		}
		wcscpy_s(cast_handle->pwszID[i], PT_MAX_GENERIC_STRLEN, pID);
		CoTaskMemFree(pID);

		// BetterFxSound: its state first (sndCheckDeviceChanges() compares it with the system and would report a stale
		// one as a change on every comparison). A property that cannot be read leaves the names written above; the
		// channel count is then set by sndDevices_GetAllKeepUnreadChannelCount(), from the count and the probe retries
		// the previous enumeration had for this device.
		if (FAILED(cast_handle->pAllDevices[i]->GetState(&cast_handle->deviceState[i])))
			cast_handle->deviceState[i] = 0;
		cache_index = sndDevices_GetAllFindCachedID(cast_handle, cast_handle->pwszID[i]);
		if (cache_index >= 0)
		{
			known_num_channel[i] = cast_handle->channelCacheNumChannel[cache_index];
			probe_retries[i] = cast_handle->channelCacheProbeRetries[cache_index];
		}

		// Access the registry properties for this device.
		hr = cast_handle->pAllDevices[i]->OpenPropertyStore(STGM_READ, &pProps);
		if (FAILED(hr))
		{
			sndDevices_GetAllKeepUnreadChannelCount(cast_handle, (int)i, known_num_channel, probe_retries, b_is_probe,
				&b_active_device_is_unprobed);
			continue;
		}

		PropVariantInit(&FriendlyName);
		hr = pProps->GetValue(PKEY_Device_FriendlyName, &FriendlyName);
		if (FAILED(hr))
		{
			PropVariantClear(&FriendlyName);
			sndDevices_GetAllKeepUnreadChannelCount(cast_handle, (int)i, known_num_channel, probe_retries, b_is_probe,
				&b_active_device_is_unprobed);
			continue;
		}

		PropVariantInit(&DeviceDesc);
		hr = pProps->GetValue(PKEY_Device_DeviceDesc, &DeviceDesc);
		if (FAILED(hr))
		{
			PropVariantClear(&FriendlyName);
			PropVariantClear(&DeviceDesc);
			sndDevices_GetAllKeepUnreadChannelCount(cast_handle, (int)i, known_num_channel, probe_retries, b_is_probe,
				&b_active_device_is_unprobed);
			continue;
		}

		if (FriendlyName.pwszVal != NULL && DeviceDesc.pwszVal != NULL)
		{
			wcscpy(cast_handle->deviceFriendlyName[i], FriendlyName.pwszVal);
			wcscpy(cast_handle->deviceDescription[i], DeviceDesc.pwszVal);

			// Read the endpoint form factor and map it to a type string.
			PROPVARIANT FormFactor;
			PropVariantInit(&FormFactor);
			if (SUCCEEDED(pProps->GetValue(PKEY_AudioEndpoint_FormFactor, &FormFactor)) && FormFactor.vt == VT_UI4)
			{
				const wchar_t* formFactorStr = L"Unknown";
				switch (FormFactor.uintVal)
				{
				case RemoteNetworkDevice:       formFactorStr = L"NetworkDevice"; break;
				case Speakers:                  formFactorStr = L"Speakers"; break;
				case LineLevel:                 formFactorStr = L"LineLevel"; break;
				case Headphones:                formFactorStr = L"Headphones"; break;
				case Microphone:                formFactorStr = L"Microphone"; break;
				case Headset:                   formFactorStr = L"Headset"; break;
				case Handset:                   formFactorStr = L"Handset"; break;
				case UnknownDigitalPassthrough: formFactorStr = L"DigitalPassthrough"; break;
				case SPDIF:                     formFactorStr = L"SPDIF"; break;
				case DigitalAudioDisplayDevice: formFactorStr = L"HDMI"; break;
				default:                        formFactorStr = L"Unknown"; break;
				}
				wcscpy(cast_handle->deviceFormFactor[i], formFactorStr);
			}
			PropVariantClear(&FormFactor);

			// Get device number of channels.
			if (sndDevicesGetFormatFromID(hp_sndDevices, cast_handle->pwszID[i], &wfx, &resultFlag) != OKAY)
			{
				sndDevices_GetAllKeepEntries(cast_handle, ip_num_devices, (int)i);
				CoTaskMemFree(pwszIDdefault);
				PropVariantClear(&FriendlyName);
				PropVariantClear(&DeviceDesc);
				return(NOT_OKAY);
			}
			if ((resultFlag == SND_DEVICES_DEVICE_OPERATION_COMPLETED) && (wfx.nChannels == 1) && (known_num_channel[i] >= 2) &&
				(cast_handle->deviceState[i] == DEVICE_STATE_ACTIVE))
			{
				// BetterFxSound: an active device that had two or more channels and reads mono now (a Bluetooth headset
				// in hands-free mode while an app has its microphone open) keeps its count while it stays active. Mono
				// devices are left out of the outputs and refused as the carrier, so the carrier moved to another device
				// (perhaps a disabled one playing silence) and the headset was dropped until it was back to stereo. Its
				// mono format is played correctly: a mono carrier is processed in stereo and averaged
				// (sndDevicesDoPlayback()), an additional output folds its stereo ring down after the channel mode.
				cast_handle->deviceNumChannel[i] = known_num_channel[i];
				probe_retries[i] = 0;
			}
			else if (resultFlag == SND_DEVICES_DEVICE_OPERATION_COMPLETED)
			{
				cast_handle->deviceNumChannel[i] = wfx.nChannels;
				known_num_channel[i] = wfx.nChannels;
				probe_retries[i] = 0;
			}
			else
			{
				// BetterFxSound: the format cannot be read right now.
				sndDevices_GetAllKeepUnreadChannelCount(cast_handle, (int)i, known_num_channel, probe_retries, b_is_probe,
					&b_active_device_is_unprobed);
			}

			// Check ID strings to see if this is the default device.
			if (pwszIDdefault != NULL && wcscmp(cast_handle->pwszID[i], pwszIDdefault) == 0)
				default_device_num = (int)i;

			// Check friendly name to see if this is the DFX device.
			// NOTE: We check if the DFX name is a substring rather than an exact match because
			//       Windows can prepend the device name with e.g. "2-", yielding "2- DFX Audio Enhancer 11.1".
			if (pstrCalcLocationOfStrInStr_Wide(cast_handle->deviceFriendlyName[i], SND_DEVICES_DFX_DEVICE_STRING,
				0, &i_found_start_location, &i_found_dfx_string) != OKAY)
			{
				sndDevices_GetAllKeepEntries(cast_handle, ip_num_devices, (int)i);
				CoTaskMemFree(pwszIDdefault);
				PropVariantClear(&FriendlyName);
				PropVariantClear(&DeviceDesc);
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_INSTANCE_CREATE_FAILED);
			}
			if (i_found_dfx_string)
				cast_handle->dfxDeviceNum = i;
		}
		else
		{
			// BetterFxSound: a name came back empty (VT_EMPTY); the format is not read either.
			sndDevices_GetAllKeepUnreadChannelCount(cast_handle, (int)i, known_num_channel, probe_retries, b_is_probe,
				&b_active_device_is_unprobed);
		}

		PropVariantClear(&FriendlyName);
		PropVariantClear(&DeviceDesc);
	}

	// BetterFxSound: the channel counts read by this enumeration, for the next one.
	for (k = 0; k < (int)deviceCount; k++)
	{
		wcscpy_s(cast_handle->channelCacheID[k], PT_MAX_GENERIC_STRLEN, cast_handle->pwszID[k]);
		cast_handle->channelCacheNumChannel[k] = known_num_channel[k];
		cast_handle->channelCacheProbeRetries[k] = probe_retries[k];
	}
	cast_handle->channelCacheCount = (int)deviceCount;

	cast_handle->activeDeviceIsUnprobed = b_active_device_is_unprobed;
	cast_handle->lastEnumerationTick = GetTickCount();

	// -------------------------------------------------------------------------
	// Pass 2: Now that dfxDeviceNum is fully resolved, classify all non-DFX
	//         devices as real playback devices.
	// -------------------------------------------------------------------------
	for (i = 0; i < deviceCount; i++)
	{
		// BetterFxSound: an entry that could not be read (empty ID) is no device.
		if (((int)i != cast_handle->dfxDeviceNum) && (cast_handle->pwszID[i][0] != L'\0'))
		{
			cast_handle->pwszIDRealDevices[cast_handle->numRealDevices] = cast_handle->pwszID[i];
			cast_handle->deviceFriendlyNameRealDevices[cast_handle->numRealDevices] = cast_handle->deviceFriendlyName[i];
			cast_handle->deviceDescriptionRealDevices[cast_handle->numRealDevices] = cast_handle->deviceDescription[i];
			cast_handle->numRealDevices += 1;
		}
	}

	// BetterFxSound: no pointer from an earlier, longer list is left past the end of this one.
	for (k = cast_handle->numRealDevices; k < SND_DEVICES_MAX_NUM_DEVICES; k++)
	{
		cast_handle->pwszIDRealDevices[k] = NULL;
		cast_handle->deviceFriendlyNameRealDevices[k] = NULL;
		cast_handle->deviceDescriptionRealDevices[k] = NULL;
	}

	CoTaskMemFree(pwszIDdefault);

	// BetterFxSound: publish the default read by this enumeration. The device rules use enumeratedDefaultDeviceNum,
	// which the device notification callbacks never write.
	cast_handle->enumeratedDefaultDeviceNum = default_device_num;
	cast_handle->defaultDeviceNum = default_device_num;

	// BetterFxSound: remember the virtual device's ID for the device notification callbacks.
	if ((cast_handle->dfxDeviceNum != SND_DEVICES_DEVICE_NOT_PRESENT) &&
		(wcscmp(cast_handle->dfxDeviceID, cast_handle->pwszID[cast_handle->dfxDeviceNum]) != 0))
		wcscpy_s(cast_handle->dfxDeviceID, PT_MAX_GENERIC_STRLEN, cast_handle->pwszID[cast_handle->dfxDeviceNum]);

	return(OKAY);
}
