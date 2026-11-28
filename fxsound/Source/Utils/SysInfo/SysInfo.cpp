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

#include "SysInfo.h"
#include <Audioclient.h>

void SysInfo::enumAudioOutputs(StringArray& audioOutputNames)
{
	HRESULT hRes = S_OK;
	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDeviceCollection *pDeviceCollection = NULL;

	hRes = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
		CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
		(void**)&pEnumerator);
	if (FAILED(hRes))
		return;

	audioOutputNames.clear();

	hRes = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL & (~DEVICE_STATE_NOTPRESENT), &pDeviceCollection);
	if (SUCCEEDED(hRes))
	{
		UINT count;
		hRes = pDeviceCollection->GetCount(&count);
		for (UINT index = 0; index < count; index++)
		{
			IMMDevice *pDevice = NULL;
			hRes = pDeviceCollection->Item(index, &pDevice);
			if (SUCCEEDED(hRes))
			{
				auto audioOutputName = getAudioOutputName(pDevice);
				pDevice->Release();
				if (audioOutputName.isEmpty())
				{
					pDeviceCollection->Release();
					pEnumerator->Release();
					return;
				}
				else
				{
					audioOutputNames.add(audioOutputName);
				}
			}
			else
				break;

		}

		pDeviceCollection->Release();
	}

	pEnumerator->Release();
}

std::wstring SysInfo::getDefaultAudioOutputId()
{
	std::wstring device_id;
	IMMDeviceEnumerator *pEnumerator = NULL;

	HRESULT hRes = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
		CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
		(void**)&pEnumerator);
	if (FAILED(hRes) || pEnumerator == NULL)
		return device_id;

	IMMDevice *pDevice = NULL;
	hRes = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
	if (SUCCEEDED(hRes) && pDevice != NULL)
	{
		LPWSTR pwszID = NULL;
		if (SUCCEEDED(pDevice->GetId(&pwszID)) && pwszID != NULL)
		{
			device_id = pwszID;
			CoTaskMemFree(pwszID);
		}
		pDevice->Release();
	}

	pEnumerator->Release();
	return device_id;
}

DWORD SysInfo::getAudioDeviceSampleRate(const std::wstring& device_id)
{
	DWORD sample_rate = 0;
	if (device_id.empty())
		return sample_rate;

	IMMDeviceEnumerator *pEnumerator = NULL;
	HRESULT hRes = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
		CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
		(void**)&pEnumerator);
	if (FAILED(hRes) || pEnumerator == NULL)
		return sample_rate;

	IMMDevice *pDevice = NULL;
	hRes = pEnumerator->GetDevice(device_id.c_str(), &pDevice);
	if (SUCCEEDED(hRes) && pDevice != NULL)
	{
		IAudioClient *pAudioClient = NULL;
		hRes = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
		if (SUCCEEDED(hRes) && pAudioClient != NULL)
		{
			WAVEFORMATEX *pwfx = NULL;
			if (SUCCEEDED(pAudioClient->GetMixFormat(&pwfx)) && pwfx != NULL)
			{
				sample_rate = pwfx->nSamplesPerSec;
				CoTaskMemFree(pwfx);
			}
			pAudioClient->Release();
		}
		pDevice->Release();
	}

	pEnumerator->Release();
	return sample_rate;
}

String SysInfo::getAudioOutputName(IMMDevice *pDevice)
{
	String audioOutputName;

	if (pDevice == NULL)
		return audioOutputName;

	IPropertyStore *pPropStore = NULL;
	PROPVARIANT propVariant;

	PropVariantInit(&propVariant);
	if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pPropStore)))
	{
		if (SUCCEEDED(pPropStore->GetValue(PKEY_DeviceInterface_FriendlyName, &propVariant)))
		{
			if (propVariant.vt == VT_LPWSTR)
			{
				audioOutputName = propVariant.pwszVal;
			}
		}

		pPropStore->Release();
	}
	PropVariantClear(&propVariant);

	return audioOutputName;
}

bool SysInfo::isServiceRunning(LPCWSTR service_name)
{
    SC_HANDLE sch = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (sch == NULL) 
    {
        return false;
    }

    SC_HANDLE svc = OpenService(sch, service_name, SC_MANAGER_ALL_ACCESS);
    if (svc == NULL) 
    {
        return false;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytes_needed = 0;
    BOOL ret = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE*)&status, sizeof(status), &bytes_needed);
    if (ret == FALSE) 
    {
        return false;
    }
    
    CloseServiceHandle(svc);
    CloseServiceHandle(sch);

    if (status.dwCurrentState == SERVICE_RUNNING) 
    {
        return true;
    }
    else 
    {
        return false;
    }
}

bool SysInfo::canSupportHotkeys()
{
    return true;
}

bool SysInfo::isRemoteSession()
{
	return GetSystemMetrics(SM_REMOTESESSION) != FALSE;
}