/*
BetterFxSound
Copyright (C) 2026  BetterFxSound contributors

This file is part of BetterFxSound, a modified version of FxSound
(Copyright (C) 2025 FxSound LLC). It adds support for sending the processed
audio to additional output devices at the same time as the main output.

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

#include <windows.h>
#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "MultiOutput.h"
#include "MasterVolume.h"
#include "OwedVolume.h"
#include "RenderSession.h"

// BetterFxSound: MMCSS for the render threads (Vista and later).
#pragma comment(lib, "avrt.lib")

namespace
{
	// {00000003-0000-0010-8000-00aa00389b71}, same value as KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
	// Defined locally so this file does not need ksmedia.h/ksuser.lib.
	const GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

	// Speaker masks, same values as the KSAUDIO_SPEAKER_* constants in ksmedia.h.
	// BetterFxSound: the layouts of channelRole() below. 5.1 is the virtual device's own (side surrounds, 0x60F, see
	// sndDevicesSetDfxDeviceSampleRateAndChannels()), it was labelled 5.1 with back surrounds (0x3F).
	DWORD defaultChannelMask(UINT32 channels)
	{
		switch (channels)
		{
		case 1: return 0x4;     // FRONT_CENTER
		case 2: return 0x3;     // STEREO
		case 3: return 0x7;     // Front left, right, center
		case 4: return 0x33;    // QUAD
		case 5: return 0x607;   // 5.0: fronts, center, side surrounds
		case 6: return 0x60F;   // 5.1 surround
		case 7: return 0x637;   // 7.0: fronts, center, back and side surrounds
		case 8: return 0x63F;   // 7.1 surround
		default: return 0;
		}
	}

	// BetterFxSound: what each channel of an interleaved frame carries, by channel count, in the order of the processing
	// format (WAVEFORMATEXTENSIBLE: front left, front right, center, LFE, then the surrounds; 3 channels: no LFE,
	// 4: quad, 5: 5.0, 7: 7.0, see sndDevicesCaptureChannelSource()). Back and side surrounds play the same role here.
	enum ChannelRole { kRoleLeft, kRoleRight, kRoleCenter, kRoleLfe, kRoleSurroundLeft, kRoleSurroundRight, kRoleCount };
	constexpr UINT32 kRoleMaxChannels = 8;
	constexpr float kFoldGain = 0.70710678f;	// -3 dB: center and surrounds folded into left and right

	ChannelRole channelRole(UINT32 channels, UINT32 c)
	{
		static const ChannelRole kRoles[kRoleMaxChannels + 1][kRoleMaxChannels] = {
			{ kRoleCenter },
			{ kRoleCenter },
			{ kRoleLeft, kRoleRight },
			{ kRoleLeft, kRoleRight, kRoleCenter },
			{ kRoleLeft, kRoleRight, kRoleSurroundLeft, kRoleSurroundRight },
			{ kRoleLeft, kRoleRight, kRoleCenter, kRoleSurroundLeft, kRoleSurroundRight },
			{ kRoleLeft, kRoleRight, kRoleCenter, kRoleLfe, kRoleSurroundLeft, kRoleSurroundRight },
			{ kRoleLeft, kRoleRight, kRoleCenter, kRoleSurroundLeft, kRoleSurroundRight, kRoleSurroundLeft, kRoleSurroundRight },
			{ kRoleLeft, kRoleRight, kRoleCenter, kRoleLfe, kRoleSurroundLeft, kRoleSurroundRight, kRoleSurroundLeft, kRoleSurroundRight },
		};
		if (channels > kRoleMaxChannels || c >= channels)
			return kRoleLfe;	// Not one of the listed layouts: left out of a fold-down
		return kRoles[channels][c];
	}

	// BetterFxSound: left and right of one frame folded down to stereo (center and surrounds at -3 dB, LFE left out).
	void foldToStereo(const float* frame, UINT32 channels, float* left, float* right)
	{
		if (channels == 1)
		{
			*left = frame[0];
			*right = frame[0];
			return;
		}
		if (channels == 2)
		{
			*left = frame[0];
			*right = frame[1];
			return;
		}

		float l = 0.0f;
		float r = 0.0f;
		for (UINT32 c = 0; c < channels; c++)
		{
			switch (channelRole(channels, c))
			{
			case kRoleLeft: l += frame[c]; break;
			case kRoleRight: r += frame[c]; break;
			case kRoleCenter: l += kFoldGain * frame[c]; r += kFoldGain * frame[c]; break;
			case kRoleSurroundLeft: l += kFoldGain * frame[c]; break;
			case kRoleSurroundRight: r += kFoldGain * frame[c]; break;
			default: break;
			}
		}
		*left = l;
		*right = r;
	}

	// BetterFxSound: the coefficients that take a frame of src_channels to dst_channels (both 1 to kRoleMaxChannels,
	// and different): out[d] = sum over s of matrix[d * kRoleMaxChannels + s] * in[s]. A channel goes to the channels
	// of the same role (a pair of them shares it at -3 dB each); with none, the center and the surrounds are folded
	// into left and right, the LFE is left out. Mono is the average of the stereo fold-down.
	void buildChannelMatrix(UINT32 src_channels, UINT32 dst_channels, float* matrix)
	{
		for (UINT32 i = 0; i < kRoleMaxChannels * kRoleMaxChannels; i++)
			matrix[i] = 0.0f;

		if (dst_channels == 1)
		{
			float stereo[kRoleMaxChannels * kRoleMaxChannels];
			buildChannelMatrix(src_channels, 2, stereo);
			for (UINT32 s = 0; s < src_channels; s++)
				matrix[s] = 0.5f * (stereo[s] + stereo[kRoleMaxChannels + s]);
			return;
		}

		int src_count[kRoleCount] = {};
		int dst_count[kRoleCount] = {};
		for (UINT32 s = 0; s < src_channels; s++)
			src_count[channelRole(src_channels, s)]++;
		for (UINT32 d = 0; d < dst_channels; d++)
			dst_count[channelRole(dst_channels, d)]++;

		for (UINT32 s = 0; s < src_channels; s++)
		{
			const ChannelRole role = channelRole(src_channels, s);
			if (dst_count[role] > 0)
			{
				const float gain = (src_count[role] > 1) ? kFoldGain : 1.0f;
				for (UINT32 d = 0; d < dst_channels; d++)
				{
					if (channelRole(dst_channels, d) == role)
						matrix[d * kRoleMaxChannels + s] += gain;
				}
				continue;
			}

			for (UINT32 d = 0; d < dst_channels; d++)
			{
				const ChannelRole dst_role = channelRole(dst_channels, d);
				if ((dst_role == kRoleLeft && (role == kRoleCenter || role == kRoleSurroundLeft)) ||
					(dst_role == kRoleRight && (role == kRoleCenter || role == kRoleSurroundRight)))
					matrix[d * kRoleMaxChannels + s] += kFoldGain;
			}
		}
	}

	template <class T> void safeRelease(T*& p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	// BetterFxSound: an output's format is its device's channel count and the processing format's rate.
	inline UINT32 packFormat(UINT32 channels, UINT32 sample_rate) { return (sample_rate << 4) | (channels & 0xF); }
	inline UINT32 formatChannels(UINT32 format) { return format & 0xF; }
	inline UINT32 formatRate(UINT32 format) { return format >> 4; }

	// Calls fn(volume) with the device's IAudioEndpointVolume and returns what it returns, or false if
	// the device or its volume control is not available. Never on an audio processing or render loop.
	template <class Fn> bool withEndpointVolume(IMMDevice* device, Fn fn)
	{
		if (device == nullptr)
			return false;

		IAudioEndpointVolume* volume = nullptr;
		HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&volume));
		if (FAILED(hr) || volume == nullptr)
			return false;

		const bool result = fn(volume);
		volume->Release();
		return result;
	}

	// Same, looking the device up by its endpoint ID. Only an active (present) device.
	template <class Fn> bool withEndpointVolumeById(const std::wstring& device_id, Fn fn)
	{
		if (device_id.empty())
			return false;

		IMMDeviceEnumerator* enumerator = nullptr;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
		if (FAILED(hr) || enumerator == nullptr)
			return false;

		IMMDevice* device = nullptr;
		hr = enumerator->GetDevice(device_id.c_str(), &device);
		enumerator->Release();
		if (FAILED(hr) || device == nullptr)
			return false;

		bool result = false;
		DWORD state = 0;
		if (SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE)
			result = withEndpointVolume(device, fn);
		device->Release();
		return result;
	}

	// 100% and unmuted. S_FALSE (the value was already set) counts as success.
	bool setFullVolume(IAudioEndpointVolume* volume, const GUID* event_context)
	{
		const bool level_ok = SUCCEEDED(volume->SetMasterVolumeLevelScalar(1.0f, event_context));
		const bool mute_ok = SUCCEEDED(volume->SetMute(FALSE, event_context));
		return level_ok && mute_ok;
	}

	// Puts a device's own setting back. The level first: some drivers clear the mute flag as a side effect of a
	// level change. BetterFxSound: tagged as the engine's change (kEngineVolumeEventContext), so no volume callback
	// takes it for the user's.
	bool restoreVolume(IAudioEndpointVolume* volume, float level, BOOL mute)
	{
		const bool level_ok = SUCCEEDED(volume->SetMasterVolumeLevelScalar(level, &kEngineVolumeEventContext));
		const bool mute_ok = SUCCEEDED(volume->SetMute(mute, &kEngineVolumeEventContext));
		return level_ok && mute_ok;
	}

	// BetterFxSound: watches an additional output's device for changes of its own level and mute that the engine did not
	// make (the user's, headset buttons, a Bluetooth device's absolute volume). It only raises the output's flag: the
	// engine's timer (MultiOutputRouter::serviceVolume()) reads the device's setting and deals with it
	// (SecondaryOutput::handleVolumeNoticeLocked()), nothing is done on the thread that delivers the notification. Registered while the output's stream is open.
	class VolumeWatch : public IAudioEndpointVolumeCallback
	{
	public:
		explicit VolumeWatch(const std::shared_ptr<std::atomic<bool>>& notice) : ref_count_(1), notice_(notice) {}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const LONG count = InterlockedDecrement(&ref_count_);
			if (count == 0)
				delete this;
			return static_cast<ULONG>(count);
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (ppv == nullptr)
				return E_POINTER;
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback))
			{
				*ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
		{
			if (data != nullptr && data->guidEventContext != kEngineVolumeEventContext && notice_ != nullptr)
				notice_->store(true, std::memory_order_release);
			return S_OK;
		}

		virtual ~VolumeWatch() {}

	private:
		volatile LONG ref_count_;
		const std::shared_ptr<std::atomic<bool>> notice_;
	};

	// ---- Owed volume record (OwedVolume.h) ----

	const wchar_t kOwedVolumeKey[] = L"Software\\BetterFxSound\\OwedDeviceVolume";

	struct OwedSetting
	{
		float level;
		DWORD mute;
	};

	std::mutex& owedVolumeMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	// Caller holds owedVolumeMutex().
	bool owedVolumeReadLocked(const std::wstring& device_id, OwedSetting* setting)
	{
		if (device_id.empty())
			return false;

		OwedSetting value = {};
		DWORD size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, kOwedVolumeKey, device_id.c_str(), RRF_RT_REG_BINARY, NULL, &value, &size) != ERROR_SUCCESS ||
			size != sizeof(value))
			return false;

		// "!(x >= 0)" is also true for NaN.
		if (!(value.level >= 0.0f) || value.level > 1.0f)
			return false;

		*setting = value;
		return true;
	}

	// Caller holds owedVolumeMutex().
	void owedVolumeWriteLocked(const std::wstring& device_id, float level, BOOL mute)
	{
		if (device_id.empty())
			return;

		HKEY key = NULL;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, kOwedVolumeKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
			return;

		OwedSetting value = { level, mute ? 1u : 0u };
		RegSetValueExW(key, device_id.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(&value), sizeof(value));
		RegCloseKey(key);
	}

	// BetterFxSound: counts the take-overs of each device (an additional output's or the carrier's), so a hand-back that
	// ran at the same time as a newer take-over of the same device can tell (restoreOwedVolume()), and the take-overs of
	// it running now (between owedVolumeBeginTakeOver() and owedVolumeEndTakeOver()). Kept for the process's lifetime:
	// one entry per device ever taken over. Under owedVolumeMutex().
	struct VolumeGeneration
	{
		std::wstring device_id;
		unsigned generation;
		unsigned running;	// Take-overs of the device running now
	};

	std::vector<VolumeGeneration>& volumeGenerationsLocked()
	{
		static std::vector<VolumeGeneration> generations;
		return generations;
	}

	// Caller holds owedVolumeMutex(). Null if the device was never taken over.
	VolumeGeneration* findVolumeGenerationLocked(const std::wstring& device_id)
	{
		for (auto& entry : volumeGenerationsLocked())
		{
			if (entry.device_id == device_id)
				return &entry;
		}
		return nullptr;
	}

	// Caller holds owedVolumeMutex().
	VolumeGeneration& volumeGenerationEntryLocked(const std::wstring& device_id)
	{
		VolumeGeneration* entry = findVolumeGenerationLocked(device_id);
		if (entry != nullptr)
			return *entry;
		VolumeGeneration added;
		added.device_id = device_id;
		added.generation = 0;
		added.running = 0;
		volumeGenerationsLocked().push_back(added);
		return volumeGenerationsLocked().back();
	}

	// Caller holds owedVolumeMutex().
	unsigned volumeGenerationLocked(const std::wstring& device_id)
	{
		const VolumeGeneration* entry = findVolumeGenerationLocked(device_id);
		return (entry != nullptr) ? entry->generation : 0;
	}

	// BetterFxSound: a take-over of the device starts (an additional output's or the carrier's), before it reads the
	// device's own setting and sets 100%: the device's generation moves on and the take-over counts as running until
	// owedVolumeEndTakeOver(), which every call is paired with. The setting owed to the device is returned in *level and
	// *mute (false if none). While a take-over runs, no hand-back erases the record (owedVolumeEraseIfGeneration()): the
	// hand-back sets 100% again instead and the device stays owed. The record used to be claimed before and after 100%
	// was set with nothing in between: a hand-back that read the record after the first claim put the device's own level
	// back after the take-over set 100%, and erased the record before the second claim wrote it again. The new owner then
	// believed the device was at 100% while it played at its own level, and nothing corrected it.
	bool owedVolumeBeginTakeOver(const std::wstring& device_id, float* level, BOOL* mute)
	{
		if (device_id.empty())
			return false;
		std::lock_guard<std::mutex> lock(owedVolumeMutex());
		VolumeGeneration& entry = volumeGenerationEntryLocked(device_id);
		entry.generation++;
		entry.running++;
		OwedSetting setting;
		if (!owedVolumeReadLocked(device_id, &setting))
			return false;
		*level = setting.level;
		*mute = setting.mute ? TRUE : FALSE;
		return true;
	}

	// BetterFxSound: the take-over started by owedVolumeBeginTakeOver() is done: the generation moves on again (a
	// hand-back that read the record while it ran keeps it), and, if record, level and mute are recorded as owed unless
	// a setting is owed already (it was erased meanwhile by a hand-back that does not count the take-overs).
	void owedVolumeEndTakeOver(const std::wstring& device_id, bool record, float level, BOOL mute)
	{
		if (device_id.empty())
			return;
		std::lock_guard<std::mutex> lock(owedVolumeMutex());
		VolumeGeneration& entry = volumeGenerationEntryLocked(device_id);
		entry.generation++;
		if (entry.running > 0)
			entry.running--;
		OwedSetting setting;
		if (record && !owedVolumeReadLocked(device_id, &setting))
			owedVolumeWriteLocked(device_id, level, mute);
	}

	// BetterFxSound: the setting owed to the device (false if none) and the device's take-over generation, read together.
	bool owedVolumeSnapshot(const std::wstring& device_id, unsigned* generation, float* level, BOOL* mute)
	{
		std::lock_guard<std::mutex> lock(owedVolumeMutex());
		*generation = volumeGenerationLocked(device_id);
		OwedSetting setting;
		if (!owedVolumeReadLocked(device_id, &setting))
			return false;
		*level = setting.level;
		*mute = setting.mute ? TRUE : FALSE;
		return true;
	}

	// BetterFxSound: erases the record only if no take-over of the device ran since generation was read
	// (owedVolumeSnapshot()) and none is running now, in one step. False otherwise: the record is kept.
	bool owedVolumeEraseIfGeneration(const std::wstring& device_id, unsigned generation)
	{
		if (device_id.empty())
			return false;
		std::lock_guard<std::mutex> lock(owedVolumeMutex());
		const VolumeGeneration* entry = findVolumeGenerationLocked(device_id);
		if ((entry != nullptr) && ((entry->generation != generation) || (entry->running > 0)))
			return false;
		RegDeleteKeyValueW(HKEY_CURRENT_USER, kOwedVolumeKey, device_id.c_str());
		return true;
	}
}

bool owedVolumeGet(const std::wstring& device_id, float* level, BOOL* mute)
{
	std::lock_guard<std::mutex> lock(owedVolumeMutex());
	OwedSetting setting;
	if (!owedVolumeReadLocked(device_id, &setting))
		return false;
	*level = setting.level;
	*mute = setting.mute ? TRUE : FALSE;
	return true;
}

void owedVolumeRecord(const std::wstring& device_id, float level, BOOL mute)
{
	std::lock_guard<std::mutex> lock(owedVolumeMutex());
	OwedSetting setting;
	if (!owedVolumeReadLocked(device_id, &setting))
		owedVolumeWriteLocked(device_id, level, mute);
}

void owedVolumeUpdate(const std::wstring& device_id, float level, BOOL mute)
{
	std::lock_guard<std::mutex> lock(owedVolumeMutex());
	OwedSetting setting;
	if (owedVolumeReadLocked(device_id, &setting) && (setting.level != level || (setting.mute != 0) != (mute != FALSE)))
		owedVolumeWriteLocked(device_id, level, mute);
}

void owedVolumeErase(const std::wstring& device_id)
{
	if (device_id.empty())
		return;
	std::lock_guard<std::mutex> lock(owedVolumeMutex());
	RegDeleteKeyValueW(HKEY_CURRENT_USER, kOwedVolumeKey, device_id.c_str());
}

std::vector<std::wstring> owedVolumeDevices()
{
	std::vector<std::wstring> devices;
	std::lock_guard<std::mutex> lock(owedVolumeMutex());

	HKEY key = NULL;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kOwedVolumeKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return devices;

	for (DWORD index = 0;; index++)
	{
		wchar_t name[512];
		DWORD name_length = sizeof(name) / sizeof(name[0]);
		const LSTATUS status = RegEnumValueW(key, index, name, &name_length, NULL, NULL, NULL, NULL);
		if (status == ERROR_MORE_DATA)
			continue;	// Not an endpoint ID
		if (status != ERROR_SUCCESS)
			break;
		devices.push_back(std::wstring(name, name_length));
	}

	RegCloseKey(key);
	return devices;
}

namespace
{
	// BetterFxSound: puts the setting owed to the device back on it and forgets it (true when nothing is owed any more).
	// If the device or its volume control is not available, it stays owed (false). device may be null, then it is looked
	// up by its ID. A take-over of the same device that ran meanwhile or still runs (an output enabled again, the device
	// made the carrier: owedVolumeBeginTakeOver()) wins: the device is set to 100% again and stays owed (false). A hand-back that
	// was slow in a driver call used to undo it, and erase the record the newer owner relied on. The record is erased
	// only if the generation read with it has not moved, in one step (owedVolumeEraseIfGeneration()).
	bool restoreOwedVolume(const std::wstring& device_id, IMMDevice* device)
	{
		unsigned generation = 0;
		float level = 1.0f;
		BOOL mute = FALSE;
		if (!owedVolumeSnapshot(device_id, &generation, &level, &mute))
			return true;

		auto hand_back = [level, mute](IAudioEndpointVolume* volume) -> bool
		{
			return restoreVolume(volume, level, mute);
		};
		const bool done = (device != nullptr) ? withEndpointVolume(device, hand_back) : withEndpointVolumeById(device_id, hand_back);
		if (!done)
			return false;
		if (owedVolumeEraseIfGeneration(device_id, generation))
			return true;

		auto full_volume = [](IAudioEndpointVolume* volume) -> bool
		{
			return setFullVolume(volume, &kEngineVolumeEventContext);
		};
		if (device != nullptr)
			withEndpointVolume(device, full_volume);
		else
			withEndpointVolumeById(device_id, full_volume);
		return false;
	}
}

/*
 * One additional output device.
 *
 * The audio thread writes into a single producer / single consumer ring buffer. The render
 * thread of this output reads from it through a cubic (Hermite) resampler whose ratio is
 * steered so that the amount of buffered audio stays at the target latency. That absorbs the
 * small clock difference between the main device and this device, so the two never drift
 * apart. BetterFxSound: the stream is opened once in the device's own mix format (its rate and
 * channel count), whatever the processing format. write() converts the processing format's
 * channels to the ring's (the device's; stereo for a mono device, so the channel mode is applied
 * before the fold to mono), and the resampler also takes the ring from the processing rate to the
 * device's (its ratio is the nominal rate ratio times the drift correction). A new carrier with
 * another channel count or in the other rate family (44.1 or 48 kHz) used to close, reopen and
 * prime every output again; now a new processing rate only empties the ring and primes it again.
 */
class SecondaryOutput
{
public:
	static constexpr UINT32 kMaxChannels = 8;
	static constexpr UINT32 kRingFrames = 1u << 16;          // ~1.36 s at 48 kHz, power of two
	static constexpr UINT32 kRingMask = kRingFrames - 1;
	static constexpr int kBaseLatencyMs = 60;                 // Headroom for the bursty writes of the capture loop
	static constexpr int kMaxDelayMs = 500;
	// BetterFxSound: 30 ms WASAPI buffer. It was 20 ms, so with the engine's 10 ms period a wake-up of the render
	// thread more than about 10 ms late (other apps' MMCSS threads, CPU load, power throttling) left the device
	// without audio, a click on this output only. The latency does not change: the device's queue is counted in the
	// level held at the target (render()), the ring simply holds that much less.
	static constexpr REFERENCE_TIME kDeviceBufferHns = 300000;
	static constexpr DWORD kRetryMs = 1000;                   // Retry interval when the device is missing
	static constexpr DWORD kRetryLongMs = 5000;               // BetterFxSound: same, once it failed kRetriesBeforeBackoff times in a row
	static constexpr int kRetriesBeforeBackoff = 5;
	static constexpr DWORD kStallMs = 2000;                   // BetterFxSound: a running stream that makes no progress this long is reopened
	                                                          // (long enough for a Bluetooth stream that starts consuming late)
	static constexpr DWORD kHealthyMs = 10000;                // BetterFxSound: a stream open this long without failing clears the retry backoff
	static constexpr DWORD kIdleStopMs = 2000;                // Stop the stream after this much silence
	static constexpr int kRealignMs = 3;                      // Delay changes from this size on are applied at once
	static constexpr double kRealignErrorSecs = 0.020;        // A lasting offset from the target this big is too
	static constexpr DWORD kFlushMs = 30;                     // Less than the target queued and nothing more for this long: play it
	static constexpr double kWriteGapSecs = 0.050;            // Writes further apart than this are a pause, not the write spacing
	static constexpr double kPhaseCapSecs = 0.100;            // Time since the last write counted in the level of an empty queue at most
	static constexpr int kDeviceAllowanceMs = 10;             // Device queue counted in the target (a 20 ms buffer less a period; kept with the 30 ms buffer, see kDeviceBufferHns)
	static constexpr int kDeviceHeadroomUseMs = 20;           // A larger device queue may take this much of the ring's headroom
	static constexpr DWORD kFullVolumeRetryMs = 1000;         // BetterFxSound: a take-over whose 100% failed is tried again after this
	static constexpr int kFullVolumeAttempts = 5;             // BetterFxSound: that many times at most
	static constexpr DWORD kReassertMs = 5000;                // BetterFxSound: a device's own level change this soon after the take-over is not the user's
	static constexpr int kMaxReasserts = 3;                   // BetterFxSound: 100% set again that many times at most per take-over

	// Creates the output and starts its render thread. The thread holds its own reference to the output, so the
	// output is freed only once the thread no longer uses it (see requestStop()).
	static std::shared_ptr<SecondaryOutput> create(const AdditionalOutputConfig& config, const std::shared_ptr<std::atomic<bool>>& driven,
		const std::shared_ptr<std::atomic<bool>>& late_exits)
	{
		std::shared_ptr<SecondaryOutput> output(new SecondaryOutput(config, driven, late_exits));
		DWORD error = ERROR_SUCCESS;
		if (output->stop_event_ != NULL && output->audio_event_ != NULL)
		{
			auto* thread_reference = new std::shared_ptr<SecondaryOutput>(output);
			output->thread_ = CreateThread(NULL, 0, threadProc, thread_reference, 0, &output->thread_id_);
			if (output->thread_ == NULL)
			{
				error = GetLastError();
				delete thread_reference;
			}
		}
		else
		{
			error = GetLastError();
		}
		// BetterFxSound: an output without its render thread never plays. It is kept in the list (the router's indexes
		// follow the engine's, see the delay measurement) but reported as failed, and the router creates it again
		// (started(), MultiOutputRouter::retryFailedOutputs()). It stayed in the list, silent, until the device left.
		if (output->thread_ == NULL)
			output->last_error_.store(error != ERROR_SUCCESS ? HRESULT_FROM_WIN32(error) : E_OUTOFMEMORY);
		return output;
	}

	// BetterFxSound: whether the render thread runs (create()).
	bool started() const { return thread_ != NULL; }

	// BetterFxSound: the stream is open (written by the render thread), and the error of the last failed open or
	// render (S_OK once a stream opened). Any thread.
	bool isOpen() const { return stream_open_.load(); }
	HRESULT lastError() const { return last_error_.load(); }

	// Runs when the last reference goes: on the UI thread once the render thread has exited, or on the render
	// thread itself, as its last step, if it was still busy when the output was removed. Either way nothing
	// uses this object any more.
	~SecondaryOutput()
	{
		// BetterFxSound: on the render thread, the output was removed while that thread was stuck in a driver call, and
		// the router gave up waiting for it: the device may have been taken over again since (the output enabled again,
		// or the device made the carrier), and handing it back here undid that (the new owner then played at the device's
		// own level times the app's gain, and its record was erased). The engine's sweep, which knows the devices in use
		// now, hands it back instead (MultiOutputRouter::takeLateExits()). Only while the router drives its outputs: while it
		// does not (power off, parked, exit), no output takes a device over and that sweep may not run again before
		// FxSound is on again, so the device is handed back here (a carrier take-over meanwhile still wins:
		// restoreOwedVolume()). It stayed at our 100% and unmuted while apps played to it directly.
		// An output whose render thread never started never took its device over: nothing to hand back (the router
		// replaces it with a new output for the same device, which may have taken it over already).
		const bool late = (thread_id_ != 0 && GetCurrentThreadId() == thread_id_);
		const bool started = (thread_ != NULL);

		if (thread_ != NULL)
			CloseHandle(thread_);

		// The engine stops using the device: its own level and mute go back on it.
		if (late && driven_ != nullptr && driven_->load())
		{
			if (late_exits_ != nullptr)
				late_exits_->store(true);
		}
		else if (started)
		{
			std::lock_guard<std::mutex> lock(volume_mutex_);
			handBackVolumeLocked(nullptr);
		}

		if (stop_event_ != NULL) CloseHandle(stop_event_);
		if (audio_event_ != NULL) CloseHandle(audio_event_);
	}

	const std::wstring& deviceId() const { return device_id_; }

	// BetterFxSound: setDriven() left its change to the render thread, which has not applied it yet. Any thread.
	bool volumeChangePending() const { return volume_recheck_.load(); }

	// Router, UI thread: the output is removed. Asks the render thread to stop; it closes the stream and exits.
	void requestStop()
	{
		stopping_.store(true);
		if (stop_event_ != NULL)
			SetEvent(stop_event_);
	}

	// Router, UI thread, after requestStop(): waits up to timeout_ms for the render thread to exit. If it is still
	// busy (stuck in a driver call), it keeps the output alive and frees it itself when it is done.
	bool waitStopped(DWORD timeout_ms)
	{
		return thread_ == NULL || WaitForSingleObject(thread_, timeout_ms) == WAIT_OBJECT_0;
	}

	// Router, UI thread: the engine starts or stops driving the outputs. Starting takes over a device
	// whose stream is already open (the others are taken over when their stream opens). Stopping
	// puts the device's own level and mute back.
	// BetterFxSound: never waits for the render thread. While it is opening its stream it holds volume_mutex_ across
	// the device volume calls (seconds on a Bluetooth device that is connecting), and the UI thread waited for it (power
	// off froze the app). Then the render thread applies the change itself, with the router's flag as it is then, once
	// it is done. Until it has, volumeChangePending() is true and the engine's sweep of owed volumes
	// (handBackUnusedDevices()) leaves the device to it: a take-over still running would otherwise set the device to
	// 100% after the sweep put its level back and erased the record, losing the user's level. The router cleared its
	// flag first, so no take-over starts after it.
	void setDriven(bool driven)
	{
		std::unique_lock<std::mutex> lock(volume_mutex_, std::try_to_lock);
		if (!lock.owns_lock())
		{
			volume_recheck_.store(true);
			if (audio_event_ != NULL)
				SetEvent(audio_event_);
			return;
		}
		applyDrivenLocked(driven);
	}

private:
	// Takes the device over or hands it back for the engine's driving state. Caller holds volume_mutex_. BetterFxSound:
	// a take-over whose 100% could not be set is tried again. Stopping to drive ends a level the user set meanwhile: the
	// next take-over sets 100% again (as for the carrier).
	void applyDrivenLocked(bool driven)
	{
		if (driven)
		{
			if ((!volume_taken_ || full_volume_pending_) && stream_open_.load() && !stopping_.load())
			{
				full_volume_attempts_ = 0;
				takeOverVolumeLocked(nullptr);
			}
		}
		else
		{
			user_level_ = false;
			handBackVolumeLocked(nullptr);
		}
	}

	// BetterFxSound: render thread, outside render(): a driving change that setDriven() could not apply. Not once the
	// output is being removed: removal hands the device back (~SecondaryOutput()).
	void applyDeferredDriven()
	{
		if (!volume_recheck_.load(std::memory_order_relaxed) || !volume_recheck_.exchange(false))
			return;
		// BetterFxSound: never waits on this thread for the UI thread's device volume work (serviceVolume()): left pending
		// for a later pass.
		std::unique_lock<std::mutex> lock(volume_mutex_, std::try_to_lock);
		if (!lock.owns_lock())
		{
			volume_recheck_.store(true);
			return;
		}
		if (stopping_.load())
			return;
		applyDrivenLocked(driven_ != nullptr && driven_->load());
	}

public:
	// BetterFxSound: UI/timer thread (MultiOutputRouter::serviceVolume()): the device volume work of a driven output. A
	// take-over whose 100% could not be set is tried again, and a change of the device's own setting (VolumeWatch) is
	// looked at. It was done on the render thread between periods, where its endpoint and registry calls (and waits on
	// the owed volume record's lock) could make the output miss a period. Skipped while the render thread holds
	// volume_mutex_ (opening its stream): done on a later call.
	void serviceVolume(DWORD now)
	{
		const bool notice = volume_notice_ != nullptr && volume_notice_->load(std::memory_order_acquire);
		if (!notice && !volume_retry_.load(std::memory_order_relaxed))
			return;

		std::unique_lock<std::mutex> lock(volume_mutex_, std::try_to_lock);
		if (!lock.owns_lock())
			return;

		if (volume_retry_.load(std::memory_order_relaxed) && now - volume_retry_tick_ >= kFullVolumeRetryMs)
		{
			volume_retry_.store(false);
			if (full_volume_pending_ && stream_open_.load() && !stopping_.load() && driven_ != nullptr && driven_->load())
				takeOverVolumeLocked(nullptr);
		}

		if (notice)
		{
			volume_notice_->store(false);
			handleVolumeNoticeLocked(now);
		}
	}

private:
	// BetterFxSound: the device's own level or mute changed while the stream is open, not by the engine. The device's
	// setting now is read (a driver's late report of an earlier change is then simply the current setting). While the
	// engine holds it at 100%: a change within kReassertMs of the take-over is the device's own (a Bluetooth device with
	// absolute volume sending its stored level once connected), 100% is set again, a few times at most; a later one is
	// the user's: the device keeps it, it is owed back to it instead of the old one, and a reopen of the stream does not
	// set 100% again (user_level_), until the engine stops driving the outputs. Nothing watched it: such a change stacked
	// on the app's gain, was lost at the hand-back, and was overwritten by 100% at the next reopen. UI/timer thread
	// (serviceVolume()), on the device by its ID; caller holds volume_mutex_.
	void handleVolumeNoticeLocked(DWORD now)
	{
		if (!volume_taken_ || stopping_.load() || !stream_open_.load() || driven_ == nullptr || !driven_->load())
			return;

		float level = 1.0f;
		BOOL mute = FALSE;
		bool reasserted = false;
		const bool read = withEndpointVolumeById(device_id_, [&](IAudioEndpointVolume* volume) -> bool
		{
			if (FAILED(volume->GetMasterVolumeLevelScalar(&level)) || FAILED(volume->GetMute(&mute)))
				return false;
			const bool full = (level >= 0.99f) && (mute == FALSE);
			if (!user_level_ && !full && took_over_tick_ != 0 && now - took_over_tick_ < kReassertMs && reasserts_ < kMaxReasserts)
			{
				reasserts_++;
				reasserted = true;
				setFullVolume(volume, &kEngineVolumeEventContext);
			}
			return true;
		});
		if (!read || reasserted)
			return;
		const bool at_full = (level >= 0.99f) && (mute == FALSE);

		if (!user_level_ && at_full)
			return;	// At our 100%

		owedVolumeUpdate(device_id_, level, mute);
		user_level_ = !at_full;
		took_over_tick_ = 0;
		full_volume_pending_ = false;
		volume_retry_.store(false);
	}

	// BetterFxSound: render thread. The stream was lost or could not be opened while the device may still be there (an
	// app opened it in exclusive mode, a format change, a stall): the device is not FxSound's until the stream opens
	// again, which takes it over again (openClient()). It stayed at our 100% and unmuted, so an app holding it in
	// exclusive mode played at its full hardware level. Not once the output is being removed (~SecondaryOutput()).
	void handBackAfterLoss()
	{
		std::lock_guard<std::mutex> lock(volume_mutex_);
		if (volume_taken_ && !stopping_.load())
			handBackVolumeLocked(nullptr);
	}

public:
	void setDelayMs(int delay_ms)
	{
		if (delay_ms < 0) delay_ms = 0;
		if (delay_ms > kMaxDelayMs) delay_ms = kMaxDelayMs;
		delay_ms_.store(delay_ms, std::memory_order_relaxed);
	}

	void setChannelMode(OutputChannelMode mode)
	{
		channel_mode_.store(static_cast<int>(mode), std::memory_order_relaxed);
	}

	void setVolumePercent(int percent)
	{
		target_gain_.store(outputTrimGain(percent), std::memory_order_relaxed);
	}

	void applyConfig(const AdditionalOutputConfig& config)
	{
		setDelayMs(config.delay_ms);
		setChannelMode(config.channel_mode);
		setVolumePercent(config.volume_percent);
	}

	// Audio thread. Never blocks. 'silent' queues silence of the same length instead. BetterFxSound: lead_silence
	// frames of silence are queued first, for blocks the router could not hand over (MultiOutputRouter::write()), so the
	// queue keeps its level and the output its alignment.
	void write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, bool silent, UINT32 lead_silence)
	{
		if (data == nullptr || frames == 0 || channels == 0 || channels > kMaxChannels || sample_rate == 0)
			return;

		// BetterFxSound: marks the write in progress before active_format_ is read (both sequentially consistent), so
		// the render thread, after clearing active_format_, empties the ring only once no write that still saw the old
		// format is copying into it (see tryResetRing()).
		writing_.store(true);
		const UINT32 active = active_format_.load();
		const bool rate_changed = pending_rate_.load(std::memory_order_relaxed) != sample_rate;
		// BetterFxSound: the rate being produced is published by every block, not only by one the ring is not set up
		// for. After A, X, then A again before the render thread saw X, pending_rate_ stayed X: the render thread then
		// emptied the working ring for X, and again for A at the next block, a needless dropout.
		if (rate_changed)
			pending_rate_.store(sample_rate, std::memory_order_release);
		if (active == 0 || formatRate(active) != sample_rate)
		{
			// The render thread opens the device (once) and sets the ring up for this rate, then publishes
			// the ring's format in active_format_. Until then the audio is simply not queued for this output.
			if (rate_changed)
				SetEvent(audio_event_);
			writing_.store(false, std::memory_order_release);
			return;
		}

		// BetterFxSound: the ring has the device's channel count (stereo for a mono device), the block is converted to it.
		const UINT32 out_channels = formatChannels(active);
		const bool convert = (out_channels != channels);
		float matrix[kRoleMaxChannels * kRoleMaxChannels];
		if (convert && !silent)
			buildChannelMatrix(channels, out_channels, matrix);

		const UINT32 w0 = write_pos_.load(std::memory_order_relaxed);
		const UINT32 r = read_pos_.load(std::memory_order_acquire);
		const UINT32 free_total = kRingFrames - (w0 - r);
		const UINT32 lead = lead_silence < free_total ? lead_silence : free_total;
		for (UINT32 i = 0; i < lead; i++)
			std::memset(&ring_[static_cast<size_t>((w0 + i) & kRingMask) * kMaxChannels], 0, out_channels * sizeof(float));
		const UINT32 w = w0 + lead;
		const UINT32 free_frames = free_total - lead;
		const UINT32 n = frames < free_frames ? frames : free_frames;

		// Every channel is queued (in the ring's channels). The channel mode is applied by render(), on what is played now, so a
		// change is heard within one device period rather than after the queued audio (latency + delay).
		for (UINT32 i = 0; i < n; i++)
		{
			float* dst = &ring_[static_cast<size_t>((w + i) & kRingMask) * kMaxChannels];
			const float* src = data + static_cast<size_t>(i) * channels;

			if (silent)
			{
				std::memset(dst, 0, out_channels * sizeof(float));
			}
			else if (!convert)
			{
				std::memcpy(dst, src, channels * sizeof(float));
			}
			else
			{
				for (UINT32 d = 0; d < out_channels; d++)
				{
					const float* row = &matrix[d * kRoleMaxChannels];
					float value = 0.0f;
					for (UINT32 c = 0; c < channels; c++)
						value += row[c] * src[c];
					dst[d] = value;
				}
			}
		}
		// BetterFxSound: the write time and the mean spacing of the writes are published with the block, under write_seq_
		// (odd while they change), so render() reads a block's position and its time together (readWriteStamp()). It
		// measures the queue level against the time since the write, not at its own wake-up phase.
		LARGE_INTEGER qpc_now;
		QueryPerformanceCounter(&qpc_now);
		const LONGLONG previous_qpc = last_write_qpc_.load(std::memory_order_relaxed);	// Written by this thread only
		const LONGLONG interval = qpc_now.QuadPart - previous_qpc;
		if (previous_qpc != 0 && interval > 0 && static_cast<double>(interval) < qpc_freq_ * kWriteGapSecs)
		{
			const double spacing = static_cast<double>(interval);
			write_period_avg_ = (write_period_avg_ > 0.0) ? write_period_avg_ + (spacing - write_period_avg_) * 0.05 : spacing;
		}
		const UINT32 seq = write_seq_.load(std::memory_order_relaxed);
		write_seq_.store(seq + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		last_write_qpc_.store(qpc_now.QuadPart, std::memory_order_relaxed);
		write_period_qpc_.store(static_cast<LONGLONG>(write_period_avg_), std::memory_order_relaxed);
		write_pos_.store(w + n, std::memory_order_release);
		write_seq_.store(seq + 2, std::memory_order_release);
		writing_.store(false, std::memory_order_release);

		// BetterFxSound: an output waiting to prime is woken at once, so it does not start late by its poll interval
		// (and with that much more queued than its target).
		if (awaiting_prime_.load(std::memory_order_relaxed) && awaiting_prime_.exchange(false))
			SetEvent(audio_event_);
	}

private:
	SecondaryOutput(const AdditionalOutputConfig& config, const std::shared_ptr<std::atomic<bool>>& driven,
		const std::shared_ptr<std::atomic<bool>>& late_exits)
		: device_id_(config.device_id), driven_(driven), late_exits_(late_exits),
		  volume_notice_(std::make_shared<std::atomic<bool>>(false)),
		  ring_(static_cast<size_t>(kRingFrames) * kMaxChannels, 0.0f)
	{
		applyConfig(config);
		// Fade in from silence: the master volume is applied on top of the trim.
		current_gain_ = 0.0f;
		LARGE_INTEGER qpc_freq;
		QueryPerformanceFrequency(&qpc_freq);
		qpc_freq_ = static_cast<double>(qpc_freq.QuadPart);
		stop_event_ = CreateEvent(NULL, FALSE, FALSE, NULL);
		audio_event_ = CreateEvent(NULL, FALSE, FALSE, NULL);
	}

	static DWORD WINAPI threadProc(LPVOID param)
	{
		auto* thread_reference = static_cast<std::shared_ptr<SecondaryOutput>*>(param);
		const HRESULT hr_com = CoInitializeEx(NULL, COINIT_MULTITHREADED);

		(*thread_reference)->run();

		// Drops this thread's reference. If the output was removed meanwhile this is the last one: the destructor
		// runs here, with COM still initialized for the device volume hand-back.
		delete thread_reference;

		if (SUCCEEDED(hr_com))
			CoUninitialize();
		return 0;
	}

	void run()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		// BetterFxSound: registered with MMCSS (Pro Audio), as the processing thread is, so the threads of other apps'
		// audio (games, calls, DAWs) and CPU load hold it off less often. Without it (the service is not available) it
		// keeps its own priority.
		DWORD mmcss_task_index = 0;
		HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);

		DWORD next_retry = GetTickCount();
		int failures = 0;	// BetterFxSound: opens or streams that failed in a row (retry backoff)
		DWORD opened_tick = next_retry;	// BetterFxSound: when the stream was last opened
		HANDLE handles[2] = { stop_event_, audio_event_ };

		for (;;)
		{
			// While the stream is running WASAPI signals audio_event_ every period. Otherwise poll. BetterFxSound: write()
			// wakes an output that waits to prime; a stopped one with a fragment queued polls every 5 ms, so a sound
			// shorter than the target is flushed on time (render()).
			DWORD timeout = 20;
			if (client_ != nullptr && running_)
				timeout = 200;
			else if (client_ != nullptr && !primed_ && write_pos_.load(std::memory_order_relaxed) != read_pos_.load(std::memory_order_relaxed))
				timeout = 5;
			const DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, timeout);
			if (wait_result == WAIT_OBJECT_0)
				break;
			woke_on_event_ = (wait_result == WAIT_OBJECT_0 + 1);

			applyDeferredDriven();

			const UINT32 pending = pending_rate_.load(std::memory_order_acquire);
			const DWORD now = GetTickCount();

			// BetterFxSound: the device is opened once audio comes, in its own mix format, and only opened again after it
			// failed. A new processing rate (a carrier in the other rate family) only sets the ring up again: the stream
			// keeps running. It used to be closed, opened again and primed, a dropout of seconds on Bluetooth.
			if (client_ == nullptr)
			{
				if (pending == 0 || static_cast<LONG>(now - next_retry) < 0)
					continue; // Nothing to play yet, or the device failed recently: wait before retrying.

				active_format_.store(0);
				if (!openClient())
				{
					closeClient();
					handBackAfterLoss();
					// BetterFxSound: a device that keeps failing (held in exclusive mode, a driver that does not come back)
					// is tried less often. The error is reported (lastError()).
					failures++;
					next_retry = now + (failures > kRetriesBeforeBackoff ? kRetryLongMs : kRetryMs);
					continue;
				}
				last_error_.store(S_OK);
				opened_tick = now;
				ring_rate_ = pending;
				ring_reset_pending_ = true;
			}
			else if (pending != 0 && pending != ring_rate_)
			{
				active_format_.store(0);
				ring_rate_ = pending;
				ring_reset_pending_ = true;
			}

			if (ring_reset_pending_)
				tryResetRing(now);

			if (!render())
			{
				// Typically AUDCLNT_E_DEVICE_INVALIDATED: the device was unplugged or its
				// format changed. Close it and reopen it later. BetterFxSound: also a stream that stalled (render()).
				last_error_.store(FAILED(render_error_) ? render_error_ : E_FAIL);
				active_format_.store(0, std::memory_order_release);
				closeClient();
				handBackAfterLoss();
				failures++;
				next_retry = now + (failures > kRetriesBeforeBackoff ? kRetryLongMs : kRetryMs);
			}
			else if (running_ && failures != 0 && now - opened_tick > kHealthyMs)
			{
				// Playing again. BetterFxSound: only once the stream kept going for a while: one that fails again a
				// few seconds after each open (stalls, is invalidated again) used to reset the count on its first
				// period and was reopened every few seconds forever, never backing off.
				failures = 0;
			}
		}

		active_format_.store(0, std::memory_order_release);
		closeClient();

		if (mmcss_handle != NULL)
			AvRevertMmThreadCharacteristics(mmcss_handle);
	}

	// BetterFxSound: empties the ring and sets it up for ring_rate_ (the processing rate), once no write() that saw the
	// previous format is still copying into it; otherwise on a later pass of run(). The caller cleared active_format_
	// first, so no later write() queues anything until it is published again here. Render thread.
	void tryResetRing(DWORD now)
	{
		if (writing_.load())
			return;

		read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
		play_rate_ = ring_rate_;
		setUpLowPass();
		primed_ = false;
		idle_since_ = now;
		hold_frames_ = 0;
		realign_pending_ = false;
		ring_reset_pending_ = false;
		active_format_.store(packFormat(ring_channels_, ring_rate_), std::memory_order_release);
	}

	// BetterFxSound: a device slower than the processing rate (44.1 kHz next to a 48 kHz carrier, a 16 kHz hands-free
	// headset) is fed through an anti-alias filter first, as the carrier is (sndDevicesDoPlayback()): a 4th order
	// Butterworth low-pass at 0.45 of the device's rate, on the ring frames as they are pulled. The Windows audio engine
	// did that rate conversion before. Render thread, when the ring is set up.
	void setUpLowPass()
	{
		std::memset(lowpass_state_, 0, sizeof(lowpass_state_));
		const double device_ratio = (play_rate_ > 0) ? static_cast<double>(sample_rate_) / static_cast<double>(play_rate_) : 1.0;
		lowpass_ = device_ratio < 0.99;
		if (!lowpass_)
			return;

		static const double q[2] = { 0.54119610014619698, 1.30656296487637652 };
		const double k = std::tan(3.14159265358979323846 * 0.45 * device_ratio);
		for (int stage = 0; stage < 2; stage++)
		{
			const double norm = 1.0 / (1.0 + k / q[stage] + k * k);
			lowpass_b0_[stage] = static_cast<float>(k * k * norm);
			lowpass_a1_[stage] = static_cast<float>(2.0 * (k * k - 1.0) * norm);
			lowpass_a2_[stage] = static_cast<float>((1.0 - k / q[stage] + k * k) * norm);
		}
	}

	// The anti-alias filter on one frame of ring_channels_ (b1 = 2 * b0, b2 = b0), transposed direct form II.
	void lowPassFrame(float* frame)
	{
		for (int stage = 0; stage < 2; stage++)
		{
			const float b0 = lowpass_b0_[stage];
			const float a1 = lowpass_a1_[stage];
			const float a2 = lowpass_a2_[stage];
			for (UINT32 c = 0; c < ring_channels_; c++)
			{
				float& z1 = lowpass_state_[stage][0][c];
				float& z2 = lowpass_state_[stage][1][c];
				const float x = frame[c];
				const float y = b0 * x + z1;
				z1 = 2.0f * b0 * x - a1 * y + z2;
				z2 = b0 * x - a2 * y;
				// No denormals once it has decayed.
				if (std::fabs(z1) < 1e-20f) z1 = 0.0f;
				if (std::fabs(z2) < 1e-20f) z2 = 0.0f;
				frame[c] = y;
			}
		}
	}

	// BetterFxSound: records why the stream could not be opened (lastError()). Render thread.
	bool openFailed(HRESULT hr)
	{
		last_error_.store(FAILED(hr) ? hr : E_FAIL);
		return false;
	}

	bool openClient()
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
		if (FAILED(hr) || enumerator == nullptr)
			return openFailed(hr);

		hr = enumerator->GetDevice(device_id_.c_str(), &device_);
		safeRelease(enumerator);
		if (FAILED(hr) || device_ == nullptr)
			return openFailed(hr);

		DWORD state = 0;
		if (FAILED(device_->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
			return openFailed(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

		hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client_);
		if (FAILED(hr) || client_ == nullptr)
			return openFailed(hr);

		// BetterFxSound: the device's own mix format (channel count at most kMaxChannels), whatever the carrier's:
		// write() converts the processing format's channels to the ring's and render() its rate to the device's. It
		// used to be the processing format's, so every output was closed and opened again when a carrier with another
		// channel count or in the other rate family was chosen.
		UINT32 channels = 0;
		UINT32 sample_rate = 0;
		DWORD mix_mask = 0;		// The mix format's own speaker mask, 0 if it is not a 32 bit float extensible format
		WAVEFORMATEX* mix = nullptr;
		hr = client_->GetMixFormat(&mix);
		if (FAILED(hr) || mix == nullptr)
			return openFailed(hr);
		channels = mix->nChannels;
		const UINT32 mix_channels = channels;
		sample_rate = mix->nSamplesPerSec;
		if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
			mix->wBitsPerSample == 32)
		{
			const WAVEFORMATEXTENSIBLE* mix_ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
			if (IsEqualGUID(mix_ext->SubFormat, kSubtypeIeeeFloat))
				mix_mask = mix_ext->dwChannelMask;
		}
		CoTaskMemFree(mix);
		if (channels == 0 || sample_rate == 0)
			return openFailed(E_UNEXPECTED);
		if (channels > kMaxChannels)
			channels = kMaxChannels;

		// At the device's mix rate: the audio engine has no rate to convert. AUTOCONVERTPCM still maps the channels when
		// the device has more than kMaxChannels.
		WAVEFORMATEXTENSIBLE wfx = {};
		wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wfx.Format.nChannels = static_cast<WORD>(channels);
		wfx.Format.nSamplesPerSec = sample_rate;
		wfx.Format.wBitsPerSample = 32;
		wfx.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
		wfx.Format.nAvgBytesPerSec = sample_rate * wfx.Format.nBlockAlign;
		wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		wfx.Samples.wValidBitsPerSample = 32;
		wfx.dwChannelMask = defaultChannelMask(channels);
		wfx.SubFormat = kSubtypeIeeeFloat;

		// BetterFxSound: in the engine's private render session, hidden and not persisted, as the carrier's
		// (RenderSession.h). It was the default session, whose flags and stored level the first stream on the device set.
		const DWORD session_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | kRenderSessionFlags;
		const DWORD flags = session_flags
			| AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
			| AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

		hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBufferHns, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), &kRenderSessionGuid);
		if ((hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == E_INVALIDARG) && mix_mask != 0 && mix_channels <= kMaxChannels)
		{
			// BetterFxSound: an audio engine or APO that refuses the conversion flags (or the standard speaker mask) gets
			// its own mix format as is, which needs no conversion: same rate, channel count and speaker mask. Back and
			// side surrounds play the same role here (channelRole()), so the ring's layout still fits. Only when the
			// device has at most kMaxChannels. A fresh client, as the carrier does after a refused format.
			safeRelease(client_);
			hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client_);
			if (FAILED(hr) || client_ == nullptr)
				return openFailed(hr);
			wfx.dwChannelMask = mix_mask;
			hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, session_flags, kDeviceBufferHns, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), &kRenderSessionGuid);
		}
		if (FAILED(hr))
			return openFailed(hr);

		// BetterFxSound: the session's own level and mute at 100% and unmuted, so only the app's gain applies.
		resetRenderSessionVolume(client_);

		hr = client_->SetEventHandle(audio_event_);
		if (FAILED(hr))
			return openFailed(hr);

		hr = client_->GetBufferSize(&buffer_frames_);
		if (FAILED(hr) || buffer_frames_ == 0)
			return openFailed(hr);

		// BetterFxSound: the device queue counted in the latency target (render()). A fixed allowance, so the output's
		// latency does not move with the buffer size and engine period of each open; only a device that keeps a much
		// larger queue (its buffer less a period) gets a larger one, so the ring keeps its headroom for the bursty writes.
		// It is set at this output's first successful open and kept across reopens: recomputed at each open, a large-queue
		// device's latency followed the buffer size and period of every open (in seconds, the device rate may change).
		if (device_allowance_secs_ < 0.0)
		{
			REFERENCE_TIME default_period = 0;
			REFERENCE_TIME minimum_period = 0;
			if (FAILED(client_->GetDevicePeriod(&default_period, &minimum_period)) || default_period < 0)
				default_period = 0;
			const double period_secs = static_cast<double>(default_period) / 10000000.0;
			const double buffer_secs = static_cast<double>(buffer_frames_) / static_cast<double>(sample_rate);
			const double device_queue = buffer_secs > period_secs ? buffer_secs - period_secs : 0.0;
			const double headroom_use = static_cast<double>(kDeviceHeadroomUseMs) / 1000.0;
			const double allowance = static_cast<double>(kDeviceAllowanceMs) / 1000.0;
			device_allowance_secs_ = (device_queue > allowance + headroom_use) ? device_queue - headroom_use : allowance;
		}

		hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
		if (FAILED(hr) || render_ == nullptr)
			return openFailed(hr);

		// Start with a buffer of silence so the first period does not glitch.
		BYTE* buffer = nullptr;
		if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &buffer)))
			render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);

		channels_ = channels;
		// A mono device's ring is stereo: the channel mode picks left, right or both before the fold to mono (render()).
		ring_channels_ = (channels == 1) ? 2 : channels;
		sample_rate_ = sample_rate;
		running_ = false;
		primed_ = false;
		render_error_ = S_OK;
		padding_after_ = buffer_frames_;
		last_progress_tick_ = GetTickCount();

		// BetterFxSound: the device's own level and mute are watched while the stream is open (handleVolumeNoticeLocked()).
		// Without it the stream plays all the same.
		if (SUCCEEDED(device_->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&endpoint_volume_))) &&
			endpoint_volume_ != nullptr)
		{
			VolumeWatch* watch = new (std::nothrow) VolumeWatch(volume_notice_);
			if (watch != nullptr)
			{
				if (SUCCEEDED(endpoint_volume_->RegisterControlChangeNotify(watch)))
					volume_watch_ = watch;
				else
					watch->Release();
			}
		}

		// The device's own volume, once per stream open, never while rendering. While the engine drives the
		// outputs it records the device's own level and mute, then sets 100% and unmuted so they do not stack
		// on the app's gain. While it does not (FxSound off, the device may be the Windows default and play
		// the apps' sound directly), it puts back a setting still owed to the device, for example after the
		// device was unplugged while taken over and has just come back. An output being removed leaves it.
		// BetterFxSound: it did hand the device back when it was removed while this thread opened the stream, so a
		// thread that came back from the driver after the router stopped waiting for it undid a newer take-over of the
		// device. Its removal (~SecondaryOutput()) or the engine's sweep does it.
		{
			std::lock_guard<std::mutex> lock(volume_mutex_);
			stream_open_.store(true);
			if (!stopping_.load())
			{
				if (driven_ != nullptr && driven_->load())
					takeOverVolumeLocked(device_);
				else
					handBackVolumeLocked(device_);
			}
		}
		return true;
	}

	// Records the device's own level and mute (OwedVolume.h: unless a setting is already owed to it, then the
	// device is still at our 100% and the recorded setting is its own), then sets it to 100% and unmuted. If the
	// device's setting can't be read, the device is left alone. Caller holds volume_mutex_. device may be null,
	// then the device is looked up by its ID.
	// BetterFxSound: a level the user set on the device while the engine drove it is kept (user_level_, see
	// handleVolumeNoticeLocked()). The change is tagged as the engine's (kEngineVolumeEventContext): when the device is
	// also the carrier for a moment (right after a reinit made it the carrier), the carrier's callback recorded our 100%
	// as the user's own level. A take-over whose 100% could not be set (the endpoint not ready yet after a connect or a
	// resume) was marked done all the same and never tried again: it is tried again from the engine's timer
	// (serviceVolume()), kFullVolumeAttempts times at most. The take-over counts as running from before the device's own
	// setting is read until after 100% is set (owedVolumeBeginTakeOver()), so a hand-back of the device that overlaps it
	// never leaves it at its own level or with nothing owed. The setting is read only once the take-over started: a
	// hand-back that finished just before has put the device's own level back by then (it was read first, so our 100%
	// could be recorded as the device's own).
	void takeOverVolumeLocked(IMMDevice* device)
	{
		bool forced = false;
		bool full_volume_set = false;
		auto take_over = [this, &forced, &full_volume_set](IAudioEndpointVolume* volume) -> bool
		{
			float level = 1.0f;
			BOOL mute = FALSE;
			if (!owedVolumeBeginTakeOver(device_id_, &level, &mute))
			{
				if (!(SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)) && SUCCEEDED(volume->GetMute(&mute))))
				{
					owedVolumeEndTakeOver(device_id_, false, level, mute);
					return false;
				}
				owedVolumeRecord(device_id_, level, mute);
			}
			volume_taken_ = true;
			if (user_level_)
			{
				owedVolumeEndTakeOver(device_id_, false, level, mute);
				return true;
			}
			forced = true;
			full_volume_set = setFullVolume(volume, &kEngineVolumeEventContext);
			owedVolumeEndTakeOver(device_id_, true, level, mute);
			return true;
		};

		const bool done = (device != nullptr) ? withEndpointVolume(device, take_over) : withEndpointVolumeById(device_id_, take_over);
		if (done && (!forced || full_volume_set))
		{
			full_volume_pending_ = false;
			full_volume_attempts_ = 0;
			volume_retry_.store(false);
			if (forced)
			{
				took_over_tick_ = GetTickCount();
				if (took_over_tick_ == 0)
					took_over_tick_ = 1;
				reasserts_ = 0;
			}
			return;
		}

		full_volume_attempts_++;
		full_volume_pending_ = (full_volume_attempts_ < kFullVolumeAttempts);
		volume_retry_tick_ = GetTickCount();
		volume_retry_.store(full_volume_pending_);
	}

	// Puts the device's own level and mute back if they are owed (whoever took the device over: this output, an
	// earlier one for the same device, or a run of the app that crashed). If the device is not there, the setting
	// stays owed and is put back later (next stream open while not driven, power off, removal, or the engine's
	// sweep of devices it does not use). Caller holds volume_mutex_. device may be null, then the device is looked
	// up by its ID. BetterFxSound: a newer take-over of the device that ran meanwhile wins (restoreOwedVolume()).
	void handBackVolumeLocked(IMMDevice* device)
	{
		volume_taken_ = false;
		full_volume_pending_ = false;
		full_volume_attempts_ = 0;
		volume_retry_.store(false);
		took_over_tick_ = 0;
		restoreOwedVolume(device_id_, device);
	}

	void closeClient()
	{
		stream_open_.store(false);
		// BetterFxSound: the device's own setting is watched only while the stream is open (openClient()).
		if (endpoint_volume_ != nullptr && volume_watch_ != nullptr)
			endpoint_volume_->UnregisterControlChangeNotify(volume_watch_);
		safeRelease(volume_watch_);
		safeRelease(endpoint_volume_);
		if (client_ != nullptr && running_)
			client_->Stop();
		running_ = false;
		safeRelease(render_);
		safeRelease(client_);
		safeRelease(device_);
		buffer_frames_ = 0;
	}

	// The audio starts again after a discontinuity (priming, a skip): from silence, faded in by the gain ramp.
	void resetResampler()
	{
		std::memset(history_, 0, sizeof(history_));
		frac_ = 3.0; // Pull three frames before the first output frame.
		ratio_ = 1.0;
		error_avg_ = 0.0;
		hold_frames_ = 0;
		fill_avg_valid_ = false;
		current_gain_ = 0.0f;	// BetterFxSound: it resumed at full gain, a click
	}

	// BetterFxSound: the write position with the time of that write and the mean spacing of the writes, read together
	// (write() publishes them under write_seq_). Render thread.
	void readWriteStamp(UINT32* pos, LONGLONG* qpc, LONGLONG* period)
	{
		for (int attempt = 0; attempt < 4; attempt++)
		{
			const UINT32 seq = write_seq_.load(std::memory_order_acquire);
			*pos = write_pos_.load(std::memory_order_acquire);
			*qpc = last_write_qpc_.load(std::memory_order_relaxed);
			*period = write_period_qpc_.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if ((seq & 1u) == 0 && write_seq_.load(std::memory_order_relaxed) == seq)
				return;
		}
	}

	bool pullFrame()
	{
		const UINT32 r = read_pos_.load(std::memory_order_relaxed);
		if (write_pos_.load(std::memory_order_acquire) == r)
			return false;

		std::memmove(history_[0], history_[1], sizeof(float) * kMaxChannels * 3);
		std::memcpy(history_[3], &ring_[static_cast<size_t>(r & kRingMask) * kMaxChannels], sizeof(float) * ring_channels_);
		read_pos_.store(r + 1, std::memory_order_release);
		if (lowpass_)
			lowPassFrame(history_[3]);
		return true;
	}

	// Fills whatever space the device has. Returns false if the device failed.
	bool render()
	{
		UINT32 padding = 0;
		const HRESULT hr_padding = client_->GetCurrentPadding(&padding);
		if (FAILED(hr_padding))
			return renderFailed(hr_padding);

		const UINT32 space = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
		const DWORD now = GetTickCount();

		// BetterFxSound: stall watchdog. A running stream signals every device period and its queue goes down between
		// two of them. A device that stops consuming (its padding stays where the last write left it) or whose stream
		// stops signalling (this thread then only wakes on its 200 ms timeout) returned no error: the output stayed
		// silent or choppy until the outputs changed. After kStallMs without progress it fails, so run() closes the
		// stream and opens it again.
		if (!running_ || (woke_on_event_ && padding < padding_after_))
			last_progress_tick_ = now;
		else if (now - last_progress_tick_ > kStallMs)
			return renderFailed(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		padding_after_ = padding;

		// BetterFxSound: the ring is being set up for a new processing rate (tryResetRing()): silence meanwhile.
		if (ring_reset_pending_ || play_rate_ == 0)
			return !running_ || writeSilence(space);

		// BetterFxSound: the queue, the target and the offsets are in ring frames (at the processing rate, play_rate_);
		// the device plays at its own rate. hold_frames_ counts device frames, holdQueued() is it in ring frames.
		const double rate = static_cast<double>(play_rate_);
		const double device_rate = static_cast<double>(sample_rate_);
		const double nominal = rate / device_rate;	// Ring frames per device frame, before the drift correction
		auto holdQueued = [this, nominal]() -> UINT32 { return static_cast<UINT32>(hold_frames_ * nominal + 0.5); };
		const UINT32 target = static_cast<UINT32>((kBaseLatencyMs + delay_ms_.load(std::memory_order_relaxed)) * rate / 1000.0);
		const UINT32 slack = static_cast<UINT32>(rate * 0.15); // 150 ms

		// BetterFxSound: the level held at the target is the latency of the newest queued frame, in ring frames: the queue
		// (silence still to be played included), the device's own queue (padding) and the time since that frame was
		// written. It was the queue alone, sampled at this output's wake-ups: the writes come in blocks, so it depended on
		// the phase between the two clocks (0 to one write period, new at every start, creeping with the clock drift),
		// and the device queue (its buffer size and engine period, per open) was left out. Half the mean write spacing
		// is taken off, so the level averages what the queue did. level_target adds the device allowance (openClient()).
		// The stall and starvation checks still use the queue itself (fill).
		UINT32 write_pos = 0;
		LONGLONG write_qpc = 0;
		LONGLONG write_period_qpc = 0;
		readWriteStamp(&write_pos, &write_qpc, &write_period_qpc);
		LARGE_INTEGER qpc_now;
		QueryPerformanceCounter(&qpc_now);
		const LONGLONG since_write_qpc = (write_qpc != 0) ? qpc_now.QuadPart - write_qpc : 0;
		const double since_write = since_write_qpc > 0 ? static_cast<double>(since_write_qpc) / qpc_freq_ : 0.0; // seconds
		const double write_period = write_period_qpc > 0 ? static_cast<double>(write_period_qpc) / qpc_freq_ : 0.0;
		UINT32 ring_queued = write_pos - read_pos_.load(std::memory_order_relaxed);
		// While audio is queued, the whole time since the write counts: as the queue drains after the writes stopped (the
		// end of a sound), its level stays the latency of its newest frame, so a re-prime or flush plays the tail on time.
		// Capped, a tail queued for longer than the cap was primed with that much silence in front of it, played late.
		// The cap is only for an empty queue, where the time since the write measures nothing.
		const double counted_since = (ring_queued > 0 || since_write < kPhaseCapSecs) ? since_write : kPhaseCapSecs;
		const double phase_frames = (counted_since - 0.5 * write_period) * rate;
		const double device_frames = static_cast<double>(padding) * nominal;
		const double level_target = static_cast<double>(target) + (device_allowance_secs_ > 0.0 ? device_allowance_secs_ : 0.0) * rate;
		auto levelOf = [phase_frames, device_frames](UINT32 queue_frames) -> double
		{
			return static_cast<double>(queue_frames) + device_frames + phase_frames;
		};

		// Silence still to be played before the queued audio (a raised delay) counts as queued.
		UINT32 fill = ring_queued + holdQueued();
		double level = levelOf(fill);

		if (primed_)
		{
			// The delay was changed: from a few ms on, the output moves by the change at once, as the carrier
			// does, instead of leaving it to the drift correction below, which moves at most 0.5% and would take
			// up to a minute to get there. The output fades out first (realign_pending_), then the queued audio
			// is cut by the change or that much silence is played first, then it fades back in.
			const UINT32 moved = target > aligned_target_ ? target - aligned_target_ : aligned_target_ - target;
			if (!realign_pending_ && moved >= static_cast<UINT32>(rate * kRealignMs / 1000.0))
			{
				realign_pending_ = true;
				realign_reason_ = kRealignDelay;
			}

			// BetterFxSound: a stall and a starved queue fade out first too (realign_pending_); they cut at full gain.
			if (fill > target + slack && space == 0)
			{
				// Far too much audio queued and the device still takes none (stalled): jump ahead now, by the level's
				// offset (whatever realignment was pending: the level is measured against the current target). Nothing
				// is produced while it stalls, so there is nothing to fade out; waiting for space, as the faded jump
				// below does, let the ring fill up until write() dropped the newest audio, and the jump then landed in
				// the stale audio. The output fades back in (current_gain_).
				shiftOutput(static_cast<INT64>(level + 0.5) - static_cast<INT64>(level_target + 0.5), nominal);
				fill_avg_valid_ = false;
				error_avg_ = 0.0;
				current_gain_ = 0.0f;
				aligned_target_ = target;
				realign_pending_ = false;
			}
			else if (realign_pending_)
			{
				// Handled after this block's fade out.
			}
			else if (fill > target + slack)
			{
				// Far too much audio queued (the device stalled): jump ahead, by the level's offset.
				realign_pending_ = true;
				realign_reason_ = kRealignJump;
			}
			else if (fill + slack < target && since_write * 1000.0 < static_cast<double>(kFlushMs))
			{
				// Far too little: build the buffer up again before playing. BetterFxSound: only while the writes go on;
				// once they stopped (the end of a sound), the queue is its tail, at the right level: it drains and
				// underflows instead of fading out mid-tail.
				realign_pending_ = true;
				realign_reason_ = kRealignReprime;
			}
		}

		if (!primed_)
		{
			// BetterFxSound: write() wakes this thread when it queues more (awaiting_prime_).
			awaiting_prime_.store(true, std::memory_order_relaxed);

			// BetterFxSound: a sound shorter than the target after silence (nothing more came for kFlushMs) is played
			// too, at the moment more audio would have filled the queue to the target, as the carrier does
			// (sndDevicesDoCapture()). It used to wait for the next sound to fill the rest, and then play late.
			// The time since the last write is measured with QPC, as the carrier does: GetTickCount() is ~15.6 ms coarse.
			// Priming looks at the queued audio alone: silence left over from before (hold_frames_) is not audio.
			const double prime_level = levelOf(ring_queued);
			const bool flush = ring_queued > 0 && prime_level < level_target && since_write * 1000.0 >= static_cast<double>(kFlushMs);
			if ((ring_queued > 0 && prime_level >= level_target) || flush)
			{
				primed_ = true;
				awaiting_prime_.store(false, std::memory_order_relaxed);
				resetResampler();
				aligned_target_ = target;
				realign_pending_ = false;

				// BetterFxSound: the output starts at the target level. Audio queued beyond it (the capture's burst after a
				// pause, a late wake-up) is skipped: it played that much late, until the drift correction (about 10 s) or
				// a realignment with a dropout took it back. A flush plays silence first, for what is missing.
				const double excess = prime_level - level_target;
				if (excess >= 1.0)
				{
					const UINT32 skip = (excess < static_cast<double>(ring_queued)) ? static_cast<UINT32>(excess) : ring_queued;
					read_pos_.store(read_pos_.load(std::memory_order_relaxed) + skip, std::memory_order_release);
					ring_queued -= skip;
				}
				else if (excess <= -1.0)
				{
					hold_frames_ = static_cast<UINT32>(-excess / nominal);
				}
				fill = ring_queued + holdQueued();
				level = levelOf(fill);

				if (!running_)
				{
					const HRESULT hr_start = client_->Start();
					if (FAILED(hr_start))
						return renderFailed(hr_start);
					running_ = true;
				}
			}
			else
			{
				if (running_)
				{
					if (!writeSilence(space))
						return false;

					// Nothing to play for a while: stop the stream so the PC can sleep.
					if (now - idle_since_ > kIdleStopMs)
					{
						client_->Stop();
						running_ = false;
					}
				}
				return true;
			}
		}

		if (space == 0)
			return true;

		// Steer the resampling ratio so the queued audio stays at the target. Correcting
		// 10% of the (smoothed) error per second keeps the pitch change far below audibility.
		const double error_secs = (level - level_target) / rate;
		error_avg_ += (error_secs - error_avg_) * 0.02;
		// Short average of the level (a few device periods); the lasting-offset realign below moves by this average.
		if (!fill_avg_valid_)
		{
			fill_avg_ = level;
			fill_avg_valid_ = true;
		}
		else
		{
			fill_avg_ += (level - fill_avg_) * 0.25;
		}
		// A lasting offset is lined up at once too: typically a Bluetooth stream that was idle and took ~0.1 s
		// to start consuming after Start() while the queue kept filling. Left to the drift correction, the
		// output stayed that much late, out of sync with the others, for about a minute after every pause in
		// the audio. Clock drift alone keeps the smoothed error near 1 ms.
		if (!realign_pending_ && (error_avg_ > kRealignErrorSecs || error_avg_ < -kRealignErrorSecs))
		{
			realign_pending_ = true;
			realign_reason_ = kRealignOffset;
		}
		double adjust = error_avg_ * 0.1;
		if (adjust > 0.005) adjust = 0.005;
		if (adjust < -0.005) adjust = -0.005;
		ratio_ = nominal * (1.0 + adjust);

		BYTE* buffer = nullptr;
		const HRESULT hr_buffer = render_->GetBuffer(space, &buffer);
		if (FAILED(hr_buffer) || buffer == nullptr)
			return renderFailed(hr_buffer);

		float* out = reinterpret_cast<float*>(buffer);
		UINT32 produced = 0;
		// This output's own trim times the Windows master volume (level and mute), see MasterVolume.h. Before a
		// realignment, a fast fade out (~2 ms) instead.
		const float gain_target = realign_pending_ ? 0.0f : target_gain_.load(std::memory_order_relaxed) * masterVolumeGain();
		const float gain_step = static_cast<float>(1.0 / ((realign_pending_ ? 0.002 : 0.01) * device_rate)); // One-pole ramp
		// The channel mode, for this whole period. The ring has the device's channel count (stereo for a mono device),
		// in the WAVEFORMATEXTENSIBLE order of defaultChannelMask() (front left, front right, ...).
		const OutputChannelMode mode = static_cast<OutputChannelMode>(channel_mode_.load(std::memory_order_relaxed));
		bool underflow = false;

		for (; produced < space; produced++)
		{
			if (hold_frames_ > 0)
			{
				// Silence first, for a raised delay (see below). Nothing is pulled meanwhile.
				std::memset(out + static_cast<size_t>(produced) * channels_, 0, channels_ * sizeof(float));
				hold_frames_--;
				continue;
			}

			while (frac_ >= 1.0)
			{
				if (!pullFrame())
				{
					underflow = true;
					break;
				}
				frac_ -= 1.0;
			}
			if (underflow)
				break;

			// Volume changes are ramped over about 10 ms so they do not click.
			current_gain_ = rampGain(current_gain_, gain_target, gain_step);
			const float gain = current_gain_;

			const float t = static_cast<float>(frac_);
			float value[kMaxChannels];
			for (UINT32 c = 0; c < ring_channels_; c++)
			{
				const float xm1 = history_[0][c];
				const float x0 = history_[1][c];
				const float x1 = history_[2][c];
				const float x2 = history_[3][c];
				const float c1 = 0.5f * (x1 - xm1);
				const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
				const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
				value[c] = (((c3 * t + c2) * t + c1) * t + x0) * gain;
			}
			applyOutputChannelMode(value, 1, ring_channels_, mode);
			float* frame = out + static_cast<size_t>(produced) * channels_;
			if (channels_ == ring_channels_)
				std::memcpy(frame, value, channels_ * sizeof(float));
			else
				frame[0] = 0.5f * (value[0] + value[1]);	// Mono device: its stereo ring folded down, after the channel mode
			frac_ += ratio_;
		}

		if (underflow)
		{
			// Ran dry (the carrier stopped, or audio was paused). Pad with silence and
			// wait for the buffer to fill to the target again. BetterFxSound: the audio produced fades out over its
			// last ~2 ms instead of stopping mid-waveform, and the next start fades in (current_gain_).
			const UINT32 fade_max = static_cast<UINT32>(0.002 * device_rate);
			const UINT32 fade = produced < fade_max ? produced : fade_max;
			for (UINT32 i = 0; i < fade; i++)
			{
				const float scale = static_cast<float>(fade - i) / static_cast<float>(fade + 1);
				float* frame = out + static_cast<size_t>(produced - fade + i) * channels_;
				for (UINT32 c = 0; c < channels_; c++)
					frame[c] *= scale;
			}
			std::memset(out + static_cast<size_t>(produced) * channels_, 0, static_cast<size_t>(space - produced) * channels_ * sizeof(float));
			primed_ = false;
			idle_since_ = now;
			hold_frames_ = 0;
			current_gain_ = 0.0f;
		}
		else if (realign_pending_ && (current_gain_ < 0.01f || hold_frames_ > 0) && realign_reason_ == kRealignReprime)
		{
			// BetterFxSound: faded out, the starved queue builds up again before playing. Silence still to be played
			// is dropped (it used to count as queued audio when priming); the queued audio stays and is counted.
			primed_ = false;
			idle_since_ = now;
			hold_frames_ = 0;
			current_gain_ = 0.0f;
			realign_pending_ = false;
		}
		else if (realign_pending_ && (current_gain_ < 0.01f || hold_frames_ > 0))
		{
			// Faded out: move the output. For a delay change, by exactly the change, so the queue keeps its
			// level relative to the target (the writes are bursty, the instantaneous level is not the mean). For
			// a lasting offset, by the offset of the averaged level. Later: skip that much queued audio. Earlier:
			// play that much silence first. Either way the output fades back in from silence.
			// BetterFxSound: the shift follows why the realignment was asked for. It used to be the delay change
			// whenever the target had moved at all since the last alignment (a 1-2 ms delay tweak), so a lasting
			// offset was not corrected, and realigned again (a second dropout) a few hundred ms later. The level is
			// measured against the current target, so a pending delay change is part of an offset or a stall's shift.
			// A stall jumps by the level now, silence still to be played first; it skipped the queue by the level
			// with that silence counted, then dropped the silence too.
			const INT64 level_target_frames = static_cast<INT64>(level_target + 0.5);
			INT64 shift = 0;
			if (realign_reason_ == kRealignDelay)
				shift = static_cast<INT64>(aligned_target_) - static_cast<INT64>(target);
			else if (realign_reason_ == kRealignJump)
				shift = static_cast<INT64>(level + 0.5) - level_target_frames;
			else
				shift = static_cast<INT64>(fill_avg_ + 0.5) - level_target_frames;
			shiftOutput(shift, nominal);
			fill_avg_ -= static_cast<double>(shift);	// Re-seeded instead if resetResampler() ran
			error_avg_ = 0.0;
			current_gain_ = 0.0f;
			aligned_target_ = target;
			realign_pending_ = false;
		}

		const HRESULT hr_release = render_->ReleaseBuffer(space, 0);
		if (FAILED(hr_release))
			return renderFailed(hr_release);
		padding_after_ += space;
		return true;
	}

	// BetterFxSound: records why render() failed (run() reports it, lastError()). Render thread.
	bool renderFailed(HRESULT hr)
	{
		render_error_ = FAILED(hr) ? hr : E_FAIL;
		return false;
	}

	// BetterFxSound: moves the output by shift ring frames (render()). Later (shift > 0): silence not played yet goes
	// first, then queued audio is skipped, and the audio restarts from silence (resetResampler()). Earlier (shift < 0):
	// that much silence is played first. nominal is ring frames per device frame. Render thread.
	void shiftOutput(INT64 shift, double nominal)
	{
		if (shift > 0)
		{
			const UINT32 r = read_pos_.load(std::memory_order_relaxed);
			const UINT32 queued = write_pos_.load(std::memory_order_acquire) - r;
			const UINT32 hold_queued = static_cast<UINT32>(hold_frames_ * nominal + 0.5);
			const UINT32 from_hold = static_cast<UINT64>(shift) < hold_queued ? static_cast<UINT32>(shift) : hold_queued;
			const UINT32 from_hold_frames = static_cast<UINT32>(from_hold / nominal + 0.5);
			hold_frames_ -= (from_hold_frames < hold_frames_) ? from_hold_frames : hold_frames_;
			const UINT64 rest = static_cast<UINT64>(shift) - from_hold;
			if (rest > 0)
			{
				read_pos_.store(r + (rest < queued ? static_cast<UINT32>(rest) : queued), std::memory_order_release);
				resetResampler();
			}
		}
		else if (shift < 0)
		{
			hold_frames_ += static_cast<UINT32>(static_cast<double>(-shift) / nominal + 0.5);
		}
	}

	bool writeSilence(UINT32 frames)
	{
		if (frames == 0)
			return true;
		BYTE* buffer = nullptr;
		HRESULT hr = render_->GetBuffer(frames, &buffer);
		if (SUCCEEDED(hr))
			hr = render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
		if (FAILED(hr))
			return renderFailed(hr);
		padding_after_ += frames;
		return true;
	}

	SecondaryOutput(const SecondaryOutput&) = delete;
	SecondaryOutput& operator=(const SecondaryOutput&) = delete;

	// BetterFxSound: why a realignment is pending (render()).
	enum RealignReason
	{
		kRealignDelay,		// The delay moved: by the change
		kRealignOffset,		// A lasting offset: by the averaged level's offset
		kRealignJump,		// The device stalled, far too much queued: by the level's offset now
		kRealignReprime		// Far too little queued: build the queue up again
	};

	const std::wstring device_id_;
	const std::shared_ptr<std::atomic<bool>> driven_;	// The router's flag
	const std::shared_ptr<std::atomic<bool>> late_exits_;	// BetterFxSound: the router's, see ~SecondaryOutput()
	const std::shared_ptr<std::atomic<bool>> volume_notice_;	// BetterFxSound: raised by VolumeWatch, shared with it
	DWORD thread_id_ = 0;	// BetterFxSound: the render thread's, see ~SecondaryOutput()
	std::atomic<bool> volume_retry_{ false };	// BetterFxSound: a take-over is to be tried again (serviceVolume())
	std::atomic<bool> stream_open_{ false };	// Written by the render thread
	std::atomic<bool> stopping_{ false };		// requestStop() was called: the device is not taken over any more
	std::atomic<HRESULT> last_error_{ S_OK };	// BetterFxSound: last failed open or render, S_OK once a stream opened (lastError())
	std::atomic<bool> volume_recheck_{ false };	// BetterFxSound: setDriven() left the change to the render thread (applyDeferredDriven())

	// The device's own volume is recorded in the owed volume record (OwedVolume.h) when the engine takes the
	// device over (100% and unmuted), and put back when it stops using it. Stream open (render thread), power
	// on/off and removal (UI thread), always under volume_mutex_ and never while rendering.
	std::mutex volume_mutex_;
	bool volume_taken_ = false;		// This output set the device to 100% since it last handed it back
	// BetterFxSound, also under volume_mutex_:
	bool user_level_ = false;		// The user set the device's own level while driven: it keeps it (handleVolumeNoticeLocked())
	bool full_volume_pending_ = false;	// Setting 100% failed, it is tried again (takeOverVolumeLocked())
	int full_volume_attempts_ = 0;	// Failed attempts since the take-over started
	DWORD volume_retry_tick_ = 0;	// When the last attempt failed
	DWORD took_over_tick_ = 0;		// When the device was last set to 100% by a take-over, 0 = not held there now
	int reasserts_ = 0;				// 100% set again since then (handleVolumeNoticeLocked())
	std::atomic<int> delay_ms_{ 0 };
	std::atomic<int> channel_mode_{ 0 };
	std::atomic<float> target_gain_{ 1.0f };

	// Ring buffer, frames are stored with a fixed stride of kMaxChannels floats.
	std::vector<float> ring_;
	std::atomic<UINT32> write_pos_{ 0 };   // Written by the audio thread only
	std::atomic<UINT32> read_pos_{ 0 };    // Written by the render thread only
	std::atomic<UINT32> active_format_{ 0 };  // Format (ring channels, processing rate) the ring is set up for, 0 = none
	std::atomic<bool> writing_{ false };      // write() is copying into the ring (see tryResetRing())
	std::atomic<UINT32> pending_rate_{ 0 };   // Rate the audio thread is producing
	std::atomic<LONGLONG> last_write_qpc_{ 0 }; // QPC of the last block queued by the audio thread, stored before write_pos_
	std::atomic<LONGLONG> write_period_qpc_{ 0 }; // Mean spacing of the writes (QPC ticks), 0 = not known yet
	std::atomic<UINT32> write_seq_{ 0 };        // Odd while write() publishes a block and its time (readWriteStamp())
	std::atomic<bool> awaiting_prime_{ false }; // The render thread waits to prime: write() wakes it
	double write_period_avg_ = 0.0;             // Audio thread only: write_period_qpc_ before it is published
	double qpc_freq_ = 1.0;                     // QueryPerformanceFrequency(), set once in the constructor

	HANDLE stop_event_ = NULL;
	HANDLE audio_event_ = NULL;
	HANDLE thread_ = NULL;

	// Render thread only.
	IMMDevice* device_ = nullptr;
	IAudioEndpointVolume* endpoint_volume_ = nullptr;	// BetterFxSound: while the stream is open, with volume_watch_ registered
	VolumeWatch* volume_watch_ = nullptr;
	IAudioClient* client_ = nullptr;
	IAudioRenderClient* render_ = nullptr;
	UINT32 buffer_frames_ = 0;
	UINT32 channels_ = 0;			// The device's (its mix format's, at most kMaxChannels)
	UINT32 ring_channels_ = 2;		// The ring's: the device's, or 2 for a mono device
	UINT32 sample_rate_ = 0;		// The device's mix rate
	UINT32 ring_rate_ = 0;			// Processing rate the ring is set up (or being set up) for
	UINT32 play_rate_ = 0;			// Processing rate of the audio in the ring now, 0 = none yet
	bool ring_reset_pending_ = false;	// The ring is to be emptied for ring_rate_ (tryResetRing())
	bool lowpass_ = false;			// The device is slower than the processing rate: anti-alias filter (setUpLowPass())
	float lowpass_b0_[2] = {};
	float lowpass_a1_[2] = {};
	float lowpass_a2_[2] = {};
	float lowpass_state_[2][2][kMaxChannels] = {};
	bool running_ = false;
	bool primed_ = false;
	DWORD idle_since_ = 0;
	float history_[4][kMaxChannels] = {};
	double frac_ = 3.0;
	double ratio_ = 1.0;
	double error_avg_ = 0.0;
	float current_gain_ = 0.0f;
	UINT32 aligned_target_ = 0;		// Queue target (frames) the output was last lined up with
	UINT32 hold_frames_ = 0;		// Silence to play before pulling from the queue again (a raised delay)
	double fill_avg_ = 0.0;			// Short average of the level (render()), silence to play included (ring frames)
	bool fill_avg_valid_ = false;
	bool realign_pending_ = false;	// Fading out before the output is moved (realign_reason_)
	RealignReason realign_reason_ = kRealignDelay;	// Why realign_pending_ was set: what the output moves by
	double device_allowance_secs_ = -1.0;	// Device queue counted in the latency target (seconds, first openClient()), < 0 = not set
	HRESULT render_error_ = S_OK;	// BetterFxSound: why render() failed last (renderFailed())
	bool woke_on_event_ = false;	// BetterFxSound: run()'s last wake-up was audio_event_ (stall watchdog, render())
	UINT32 padding_after_ = 0;		// BetterFxSound: the device's queue right after the last write (stall watchdog)
	DWORD last_progress_tick_ = 0;	// BetterFxSound: when the device was last seen consuming (stall watchdog)
};

bool setDeviceFullVolumeById(const std::wstring& device_id, const GUID* event_context)
{
	// BetterFxSound: a take-over (the carrier's): a hand-back of the device running at the same time leaves it at 100%. The
	// record (written by the caller) is kept while 100% is set, see owedVolumeBeginTakeOver().
	float level = 1.0f;
	BOOL mute = FALSE;
	const bool owed = owedVolumeBeginTakeOver(device_id, &level, &mute);
	const bool result = withEndpointVolumeById(device_id, [event_context](IAudioEndpointVolume* volume) -> bool
	{
		return setFullVolume(volume, event_context);
	});
	owedVolumeEndTakeOver(device_id, owed, level, mute);
	return result;
}

bool getDeviceVolumeById(const std::wstring& device_id, float* level, BOOL* mute)
{
	return withEndpointVolumeById(device_id, [level, mute](IAudioEndpointVolume* volume) -> bool
	{
		return SUCCEEDED(volume->GetMasterVolumeLevelScalar(level)) && SUCCEEDED(volume->GetMute(mute));
	});
}

bool handBackOwedVolumeById(const std::wstring& device_id)
{
	return restoreOwedVolume(device_id, nullptr);
}

void applyOutputChannelMode(float* buffer, UINT32 frames, UINT32 channels, OutputChannelMode mode)
{
	if (buffer == nullptr || mode == OutputChannelMode::Both || channels < 2)
		return;

	for (UINT32 i = 0; i < frames; i++)
	{
		float* frame = buffer + static_cast<size_t>(i) * channels;
		// BetterFxSound: from the stereo fold-down, so the center (dialogue) and the surrounds of a 5.1 or 7.1 frame are
		// kept. Only the two front channels were used.
		float left;
		float right;
		foldToStereo(frame, channels, &left, &right);
		float value;
		if (mode == OutputChannelMode::Left)
			value = left;
		else if (mode == OutputChannelMode::Right)
			value = right;
		else
			value = 0.5f * (left + right);

		// BetterFxSound: only front left and front right play it, as when a stereo stream was upmixed by Windows. Every
		// channel used to get it, so the LFE (a subwoofer) got the full-band signal at 0 dB, and the center and the
		// surrounds played it too.
		for (UINT32 c = 0; c < channels; c++)
		{
			bool front;
			if (channels > kRoleMaxChannels)
				front = (c < 2);
			else
			{
				const ChannelRole role = channelRole(channels, c);
				front = (role == kRoleLeft || role == kRoleRight);
			}
			frame[c] = front ? value : 0.0f;
		}
	}
}

namespace
{
	// How long removing outputs waits, in all, for their render threads to exit. A thread still busy after that
	// (stuck in a driver call) frees its output itself when it is done, so this only bounds the UI thread's wait.
	// BetterFxSound: such an output leaves its device's owed volume to the engine's sweep (takeLateExits()) while the router
	// drives its outputs, and hands it back itself otherwise (~SecondaryOutput()).
	constexpr DWORD kStopWaitMs = 2000;

	void stopOutputs(std::vector<std::shared_ptr<SecondaryOutput>>& outputs)
	{
		for (auto& output : outputs)
			output->requestStop();

		const DWORD start = GetTickCount();
		for (auto& output : outputs)
		{
			const DWORD elapsed = GetTickCount() - start;
			output->waitStopped(elapsed < kStopWaitMs ? kStopWaitMs - elapsed : 0);
		}
	}
}

MultiOutputRouter::MultiOutputRouter()
	: driven_(std::make_shared<std::atomic<bool>>(false)),
	  late_exits_(std::make_shared<std::atomic<bool>>(false))
{
	InitializeSRWLock(&lock_);
}

MultiOutputRouter::~MultiOutputRouter()
{
	stopAll();
}

void MultiOutputRouter::configure(const std::vector<AdditionalOutputConfig>& outputs)
{
	std::lock_guard<std::mutex> configure_lock(configure_mutex_);
	configs_ = outputs;
	configureLocked(outputs);
}

void MultiOutputRouter::configureLocked(const std::vector<AdditionalOutputConfig>& outputs)
{
	// Fast path: same devices as now, only settings (delay, volume, channels) changed. Those
	// are atomics, so a shared lock is enough and the audio thread keeps running untouched.
	// This is what happens while a slider is being dragged. BetterFxSound: an output without its render thread
	// (create()) is not the same: it is created again.
	{
		AcquireSRWLockShared(&lock_);
		bool same_devices = (outputs.size() == outputs_.size());
		for (size_t i = 0; same_devices && i < outputs.size(); i++)
		{
			if (outputs_[i] == nullptr || !outputs_[i]->started() || outputs_[i]->deviceId() != outputs[i].device_id)
				same_devices = false;
		}
		if (same_devices)
		{
			for (size_t i = 0; i < outputs.size(); i++)
				outputs_[i]->applyConfig(outputs[i]);
		}
		ReleaseSRWLockShared(&lock_);

		if (same_devices)
			return;
	}

	// BetterFxSound: the new list is built without lock_: kept outputs are shared, new ones are created here (a 2 MB
	// ring, two events and a thread each). It used to be built under the exclusive lock, and the audio thread, which
	// never waits for it, dropped its blocks for every output meanwhile: a skip in the audio of the outputs that stayed,
	// which then played about one block early. configure_mutex_ keeps outputs_ from changing meanwhile.
	std::vector<std::shared_ptr<SecondaryOutput>> current;
	AcquireSRWLockShared(&lock_);
	current = outputs_;
	ReleaseSRWLockShared(&lock_);

	std::vector<std::shared_ptr<SecondaryOutput>> next;
	std::vector<std::wstring> next_ids;	// Device of each entry of next (an entry may be empty)
	for (const auto& config : outputs)
	{
		if (config.device_id.empty())
			continue;

		bool duplicate = false;
		for (const auto& id : next_ids)
		{
			if (id == config.device_id)
				duplicate = true;
		}
		if (duplicate)
			continue;

		std::shared_ptr<SecondaryOutput> output;
		for (const auto& existing : current)
		{
			if (existing != nullptr && existing->started() && existing->deviceId() == config.device_id)
			{
				output = existing;
				break;
			}
		}

		if (output != nullptr)
		{
			output->applyConfig(config);
		}
		else
		{
			// BetterFxSound: out of memory, the output is left empty at its index (the delay measurement relies on the
			// indexes) and created again by retryFailedOutputs(). The exception used to leave the exclusive lock held.
			try
			{
				output = SecondaryOutput::create(config, driven_, late_exits_);
			}
			catch (const std::bad_alloc&)
			{
				output.reset();
			}
		}

		next.push_back(std::move(output));
		next_ids.push_back(config.device_id);
	}

	// Only the swap is done under the exclusive lock. next then holds the previous list.
	AcquireSRWLockExclusive(&lock_);
	outputs_.swap(next);
	ReleaseSRWLockExclusive(&lock_);

	std::vector<std::shared_ptr<SecondaryOutput>> removed;
	for (auto& previous : next)
	{
		if (previous == nullptr)
			continue;
		bool kept = false;
		for (const auto& output : outputs_)
		{
			if (output == previous)
				kept = true;
		}
		if (!kept)
			removed.push_back(std::move(previous));
	}
	// Only removed holds the removed outputs from here on.
	next.clear();
	current.clear();

	// Stopping the render threads of removed outputs happens outside the lock so the audio thread is never held
	// up by it. Once a thread has exited, dropping the last reference here hands the device back.
	stopOutputs(removed);
	removed.clear();
}

void MultiOutputRouter::stopAll()
{
	configure(std::vector<AdditionalOutputConfig>());
}

void MultiOutputRouter::retryFailedOutputs()
{
	std::lock_guard<std::mutex> configure_lock(configure_mutex_);

	bool any_failed = false;
	AcquireSRWLockShared(&lock_);
	for (const auto& output : outputs_)
	{
		if (output == nullptr || !output->started())
			any_failed = true;
	}
	ReleaseSRWLockShared(&lock_);
	if (!any_failed)
		return;

	const DWORD now = GetTickCount();
	if (now - last_retry_tick_ < 1000)
		return;
	last_retry_tick_ = now;

	// configs_ itself is not changed by configureLocked().
	configureLocked(configs_);
}

std::vector<AdditionalOutputStatus> MultiOutputRouter::getStatus() const
{
	// Read under the shared lock rather than from copies of the outputs: a copy could end up holding the last
	// reference to a removed output, and free it (handing its device back) on the calling thread.
	std::vector<AdditionalOutputStatus> status;
	AcquireSRWLockShared(&lock_);
	for (const auto& output : outputs_)
	{
		if (output == nullptr)
			continue;
		AdditionalOutputStatus entry;
		entry.device_id = output->deviceId();
		entry.open = output->isOpen();
		const HRESULT error = output->lastError();
		entry.failed = !entry.open && FAILED(error);
		entry.last_error = error;
		status.push_back(entry);
	}
	ReleaseSRWLockShared(&lock_);
	return status;
}

std::vector<std::wstring> MultiOutputRouter::pendingVolumeDevices() const
{
	// Under the shared lock, not from copies of the outputs (see getStatus()).
	std::vector<std::wstring> devices;
	AcquireSRWLockShared(&lock_);
	for (const auto& output : outputs_)
	{
		if (output != nullptr && output->volumeChangePending())
			devices.push_back(output->deviceId());
	}
	ReleaseSRWLockShared(&lock_);
	return devices;
}

void MultiOutputRouter::setDriven(bool driven)
{
	driven_->store(driven);

	// The copies keep the outputs alive, so no lock is held during the device volume calls.
	std::vector<std::shared_ptr<SecondaryOutput>> outputs;
	AcquireSRWLockShared(&lock_);
	outputs = outputs_;
	ReleaseSRWLockShared(&lock_);

	for (const auto& output : outputs)
	{
		if (output != nullptr)
			output->setDriven(driven);
	}
}

bool MultiOutputRouter::isDriven() const
{
	return driven_->load();
}

void MultiOutputRouter::serviceVolume()
{
	// The copies keep the outputs alive, so no lock is held during the device volume calls (as setDriven()).
	std::vector<std::shared_ptr<SecondaryOutput>> outputs;
	AcquireSRWLockShared(&lock_);
	outputs = outputs_;
	ReleaseSRWLockShared(&lock_);

	const DWORD now = GetTickCount();
	for (const auto& output : outputs)
	{
		if (output != nullptr)
			output->serviceVolume(now);
	}
}

bool MultiOutputRouter::takeLateExits()
{
	return late_exits_->load() && late_exits_->exchange(false);
}

void MultiOutputRouter::write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, int solo_index)
{
	// Never wait on the audio thread: if the list is being changed right now, skip this block. BetterFxSound: its
	// length is queued as silence with the next block, so the outputs that stay keep their level and alignment
	// (a skip used to leave them playing one block early until the drift correction caught up, seconds later).
	const DWORD now = GetTickCount();
	if (!TryAcquireSRWLockShared(&lock_))
	{
		if (skipped_rate_ != sample_rate || now - skipped_tick_ > 100)
			skipped_frames_ = 0;
		skipped_rate_ = sample_rate;
		skipped_tick_ = now;
		const UINT32 cap = sample_rate / 10;
		const UINT32 total = skipped_frames_ + frames;
		skipped_frames_ = total < cap ? total : cap;
		return;
	}

	// Only blocks skipped just before this one, at this rate: after a pause or a new processing thread they are stale.
	const UINT32 lead_silence = (skipped_frames_ > 0 && skipped_rate_ == sample_rate && now - skipped_tick_ <= 100) ? skipped_frames_ : 0;
	skipped_frames_ = 0;

	const bool solo_active = (solo_index >= -1);
	for (size_t i = 0; i < outputs_.size(); i++)
	{
		const bool silent = solo_active && static_cast<int>(i) != solo_index;
		if (outputs_[i] != nullptr)
			outputs_[i]->write(data, frames, channels, sample_rate, silent, lead_silence);
	}

	ReleaseSRWLockShared(&lock_);
}
