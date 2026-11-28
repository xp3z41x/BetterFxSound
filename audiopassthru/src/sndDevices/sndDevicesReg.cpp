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
#include <Propvarutil.h>
#include "slout.h"
#include "reg.h"
#include "dfxpDefs.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

#define DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE		L"DFX"
#define DFXP_VENDOR_CODE_UNIVERSAL			  23
#define DFX_VERSION               13.028

/*
 * FUNCTION: sndDevicesWriteToRegistry()
 * DESCRIPTION:
 */
int sndDevicesWriteToRegistry(PT_HANDLE *hp_sndDevices, int i_hkeySection, wchar_t *wcp_ValueName, wchar_t *wcp_guid)
{
	struct sndDevicesHdlType *cast_handle;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	wchar_t wcp_key_value[PT_MAX_PATH_STRLEN];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];

	/* Construct registry fullpath to key, no version number if REG_LOCAL_MACHINE */
	if( i_hkeySection == REG_CURRENT_USER )
	{
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
		DFXP_REGISTRY_TOP_WIDE, DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE, (int)DFX_VERSION, 
		DFXP_VENDOR_CODE_UNIVERSAL, SND_DEVICES_REGISTRY_DEVICES_WIDE, wcp_ValueName);
	}
	else if( i_hkeySection == REG_LOCAL_MACHINE )
	{
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%s\\%s", 
		DFXP_REGISTRY_TOP_WIDE, DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE, 
		DFXP_VENDOR_CODE_UNIVERSAL, SND_DEVICES_REGISTRY_DEVICES_WIDE, wcp_ValueName);
	}
	else
		return(NOT_OKAY);

	swprintf(wcp_key_value, L"%s", wcp_guid);

	if (regCreateKey_Wide(i_hkeySection, wcp_full_key_path, wcp_key_value) != OKAY)
		return(NOT_OKAY);

	return(OKAY);
}

/*
 * FUNCTION: sndDeviceReadFromRegistry()
 * DESCRIPTION:
 */
int sndDeviceReadFromRegistry(PT_HANDLE *hp_sndDevices, int i_hkeySection, wchar_t *wcp_ValueName, wchar_t *wcp_guid)
{
	struct sndDevicesHdlType *cast_handle;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	wchar_t wcp_key_value[PT_MAX_PATH_STRLEN];
	wchar_t wcp_full_key_path[PT_MAX_PATH_STRLEN];
	int i_key_exists;
	int use_default;

	/* Construct registry fullpath to key, no version number if REG_LOCAL_MACHINE */
	if( i_hkeySection == REG_CURRENT_USER )
	{
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%d\\%s\\%s", 
		DFXP_REGISTRY_TOP_WIDE, DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE, (int)DFX_VERSION, 
		DFXP_VENDOR_CODE_UNIVERSAL, SND_DEVICES_REGISTRY_DEVICES_WIDE, wcp_ValueName);
	}
	else if( i_hkeySection == REG_LOCAL_MACHINE )
	{
	swprintf(wcp_full_key_path, L"%s\\%s\\%d\\%s\\%s", 
		DFXP_REGISTRY_TOP_WIDE, DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE, 
		DFXP_VENDOR_CODE_UNIVERSAL, SND_DEVICES_REGISTRY_DEVICES_WIDE, wcp_ValueName);
	}
	else
		return(NOT_OKAY);

	if (regReadKey_Wide(i_hkeySection, wcp_full_key_path, &i_key_exists, wcp_key_value,
	              (unsigned long)DFXP_REGISTRY_BUFFER_LENGTH) != OKAY)
	      return(NOT_OKAY);

	use_default = IS_TRUE;
	if (i_key_exists)
	{
		if (wcslen(wcp_key_value) > 0)
		{
			swprintf(wcp_guid, L"%s", wcp_key_value);
			use_default = IS_FALSE;
		}
	}

	if (use_default)
	{
		swprintf(wcp_guid, L"");
	}

	return(OKAY);
}

/*
 * FUNCTION: sndDevicesStoreHandedBackRecord()
 * DESCRIPTION:
 * BetterFxSound: keeps the handed-back record (handedBackPlaybackDeviceID and the rest, set by
 * sndDevicesRestoreDefaultDevice()) in the registry, so a hand-back at exit still counts at the next start of the app:
 * a change the user makes to the device's volume or mute while the app is not running is then taken by the master at
 * the first reinit, as it is after a power off. One value, "<mute> <level is saved> <level x 100000> <device ID>", so it
 * is never read half written. The level is stored as an integer, as the C runtime's number format follows the locale.
 */
void sndDevicesStoreHandedBackRecord(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType *cast_handle;
	wchar_t record[PT_MAX_GENERIC_STRLEN + 64];
	float level;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return;

	level = cast_handle->handedBackPlaybackLevel;
	if (!(level >= 0.0f))
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;

	if (swprintf_s(record, PT_MAX_GENERIC_STRLEN + 64, L"%d %d %d %s",
		(cast_handle->handedBackPlaybackMute != FALSE) ? 1 : 0,
		(cast_handle->handedBackPlaybackLevelIsSaved != FALSE) ? 1 : 0,
		(int)(level * 100000.0f + 0.5f),
		cast_handle->handedBackPlaybackDeviceID) < 0)
		return;

	sndDevicesWriteToRegistry(hp_sndDevices, REG_CURRENT_USER, SND_DEVICES_REGISTRY_HANDED_BACK_PLAYBACK_WIDE, record);
}

/*
 * FUNCTION: sndDevicesLoadHandedBackRecord()
 * DESCRIPTION:
 * BetterFxSound: makes the record kept by sndDevicesStoreHandedBackRecord() pending again, at the start of the app
 * (sndDevicesInit(), before its first reinit). A missing or malformed record leaves nothing pending.
 */
void sndDevicesLoadHandedBackRecord(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType *cast_handle;
	wchar_t record[PT_MAX_PATH_STRLEN];
	int mute = 0;
	int levelIsSaved = 0;
	int level = 0;
	int idOffset = 0;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return;

	if (sndDeviceReadFromRegistry(hp_sndDevices, REG_CURRENT_USER, SND_DEVICES_REGISTRY_HANDED_BACK_PLAYBACK_WIDE, record) != OKAY)
		return;

	if (swscanf_s(record, L"%d %d %d %n", &mute, &levelIsSaved, &level, &idOffset) != 3)
		return;

	if ((idOffset <= 0) || (record[idOffset] == L'\0') || (wcslen(record + idOffset) >= PT_MAX_GENERIC_STRLEN) ||
		(level < 0) || (level > 100000))
		return;

	wcscpy_s(cast_handle->handedBackPlaybackDeviceID, PT_MAX_GENERIC_STRLEN, record + idOffset);
	cast_handle->handedBackPlaybackMute = (mute != 0) ? TRUE : FALSE;
	cast_handle->handedBackPlaybackLevel = (float)level / 100000.0f;
	cast_handle->handedBackPlaybackLevelIsSaved = (levelIsSaved != 0) ? TRUE : FALSE;
	cast_handle->handedBackPlaybackIsPending = TRUE;
}

/*
 * FUNCTION: sndDevicesClearHandedBackRecord()
 * DESCRIPTION:
 * BetterFxSound: drops the record kept by sndDevicesStoreHandedBackRecord(), once the reinit it was pending for used it.
 */
void sndDevicesClearHandedBackRecord(PT_HANDLE *hp_sndDevices)
{
	wchar_t emptyRecord[1] = { L'\0' };

	if (hp_sndDevices == NULL)
		return;

	sndDevicesWriteToRegistry(hp_sndDevices, REG_CURRENT_USER, SND_DEVICES_REGISTRY_HANDED_BACK_PLAYBACK_WIDE, emptyRecord);
}
