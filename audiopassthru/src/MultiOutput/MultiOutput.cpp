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
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MultiOutput.h"
#include "MasterVolume.h"
#include "OwedVolume.h"

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
	// level change.
	bool restoreVolume(IAudioEndpointVolume* volume, float level, BOOL mute)
	{
		const bool level_ok = SUCCEEDED(volume->SetMasterVolumeLevelScalar(level, NULL));
		const bool mute_ok = SUCCEEDED(volume->SetMute(mute, NULL));
		return level_ok && mute_ok;
	}

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
	static constexpr REFERENCE_TIME kDeviceBufferHns = 200000; // 20 ms WASAPI buffer
	static constexpr DWORD kRetryMs = 1000;                   // Retry interval when the device is missing
	static constexpr DWORD kIdleStopMs = 2000;                // Stop the stream after this much silence
	static constexpr int kRealignMs = 3;                      // Delay changes from this size on are applied at once
	static constexpr double kRealignErrorSecs = 0.020;        // A lasting offset from the target this big is too
	static constexpr DWORD kFlushMs = 30;                     // Less than the target queued and nothing more for this long: play it

	// Creates the output and starts its render thread. The thread holds its own reference to the output, so the
	// output is freed only once the thread no longer uses it (see requestStop()).
	static std::shared_ptr<SecondaryOutput> create(const AdditionalOutputConfig& config, const std::shared_ptr<std::atomic<bool>>& driven)
	{
		std::shared_ptr<SecondaryOutput> output(new SecondaryOutput(config, driven));
		if (output->stop_event_ != NULL && output->audio_event_ != NULL)
		{
			auto* thread_reference = new std::shared_ptr<SecondaryOutput>(output);
			output->thread_ = CreateThread(NULL, 0, threadProc, thread_reference, 0, NULL);
			if (output->thread_ == NULL)
				delete thread_reference;
		}
		return output;
	}

	// Runs when the last reference goes: on the UI thread once the render thread has exited, or on the render
	// thread itself, as its last step, if it was still busy when the output was removed. Either way nothing
	// uses this object any more.
	~SecondaryOutput()
	{
		if (thread_ != NULL)
			CloseHandle(thread_);

		// The engine stops using the device: its own level and mute go back on it.
		{
			std::lock_guard<std::mutex> lock(volume_mutex_);
			handBackVolumeLocked(nullptr);
		}

		if (stop_event_ != NULL) CloseHandle(stop_event_);
		if (audio_event_ != NULL) CloseHandle(audio_event_);
	}

	const std::wstring& deviceId() const { return device_id_; }

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
	void setDriven(bool driven)
	{
		std::lock_guard<std::mutex> lock(volume_mutex_);
		if (driven)
		{
			if (!volume_taken_ && stream_open_.load() && !stopping_.load())
				takeOverVolumeLocked(nullptr);
		}
		else
		{
			handBackVolumeLocked(nullptr);
		}
	}

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

	// Audio thread. Never blocks. 'silent' queues silence of the same length instead.
	void write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, bool silent)
	{
		if (data == nullptr || frames == 0 || channels == 0 || channels > kMaxChannels || sample_rate == 0)
			return;

		// BetterFxSound: marks the write in progress before active_format_ is read (both sequentially consistent), so
		// the render thread, after clearing active_format_, empties the ring only once no write that still saw the old
		// format is copying into it (see tryResetRing()).
		writing_.store(true);
		const UINT32 active = active_format_.load();
		if (active == 0 || formatRate(active) != sample_rate)
		{
			// The render thread opens the device (once) and sets the ring up for this rate, then publishes
			// the ring's format in active_format_. Until then the audio is simply not queued for this output.
			if (pending_rate_.load(std::memory_order_relaxed) != sample_rate)
			{
				pending_rate_.store(sample_rate, std::memory_order_release);
				SetEvent(audio_event_);
			}
			writing_.store(false, std::memory_order_release);
			return;
		}

		// BetterFxSound: the ring has the device's channel count (stereo for a mono device), the block is converted to it.
		const UINT32 out_channels = formatChannels(active);
		const bool convert = (out_channels != channels);
		float matrix[kRoleMaxChannels * kRoleMaxChannels];
		if (convert && !silent)
			buildChannelMatrix(channels, out_channels, matrix);

		const UINT32 w = write_pos_.load(std::memory_order_relaxed);
		const UINT32 r = read_pos_.load(std::memory_order_acquire);
		const UINT32 free_frames = kRingFrames - (w - r);
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
		// BetterFxSound: the write time is stored before the block is published, so render() never sees the new block
		// with the time of an older one (and flushes it at once as if nothing had come for a while).
		LARGE_INTEGER qpc_now;
		QueryPerformanceCounter(&qpc_now);
		last_write_qpc_.store(qpc_now.QuadPart, std::memory_order_relaxed);
		write_pos_.store(w + n, std::memory_order_release);
		writing_.store(false, std::memory_order_release);
	}

private:
	SecondaryOutput(const AdditionalOutputConfig& config, const std::shared_ptr<std::atomic<bool>>& driven)
		: device_id_(config.device_id), driven_(driven), ring_(static_cast<size_t>(kRingFrames) * kMaxChannels, 0.0f)
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

		DWORD next_retry = GetTickCount();
		HANDLE handles[2] = { stop_event_, audio_event_ };

		for (;;)
		{
			// While the stream is running WASAPI signals audio_event_ every period. Otherwise poll.
			const DWORD timeout = (client_ != nullptr && running_) ? 200 : 20;
			if (WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0)
				break;

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
					next_retry = now + kRetryMs;
					continue;
				}
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
				// format changed. Close it and reopen it later.
				active_format_.store(0, std::memory_order_release);
				closeClient();
				next_retry = now + kRetryMs;
			}
		}

		active_format_.store(0, std::memory_order_release);
		closeClient();
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

	bool openClient()
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
		if (FAILED(hr) || enumerator == nullptr)
			return false;

		hr = enumerator->GetDevice(device_id_.c_str(), &device_);
		safeRelease(enumerator);
		if (FAILED(hr) || device_ == nullptr)
			return false;

		DWORD state = 0;
		if (FAILED(device_->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
			return false;

		hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client_);
		if (FAILED(hr) || client_ == nullptr)
			return false;

		// BetterFxSound: the device's own mix format (channel count at most kMaxChannels), whatever the carrier's:
		// write() converts the processing format's channels to the ring's and render() its rate to the device's. It
		// used to be the processing format's, so every output was closed and opened again when a carrier with another
		// channel count or in the other rate family was chosen.
		UINT32 channels = 0;
		UINT32 sample_rate = 0;
		WAVEFORMATEX* mix = nullptr;
		if (FAILED(client_->GetMixFormat(&mix)) || mix == nullptr)
			return false;
		channels = mix->nChannels;
		sample_rate = mix->nSamplesPerSec;
		CoTaskMemFree(mix);
		if (channels == 0 || sample_rate == 0)
			return false;
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

		const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
			| AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
			| AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
			| AUDCLNT_STREAMFLAGS_NOPERSIST;

		hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBufferHns, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), NULL);
		if (FAILED(hr))
			return false;

		if (FAILED(client_->SetEventHandle(audio_event_)))
			return false;

		if (FAILED(client_->GetBufferSize(&buffer_frames_)) || buffer_frames_ == 0)
			return false;

		hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
		if (FAILED(hr) || render_ == nullptr)
			return false;

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

		// The device's own volume, once per stream open, never while rendering. While the engine drives the
		// outputs it records the device's own level and mute, then sets 100% and unmuted so they do not stack
		// on the app's gain. While it does not (FxSound off, the device may be the Windows default and play
		// the apps' sound directly), it puts back a setting still owed to the device, for example after the
		// device was unplugged while taken over and has just come back. An output being removed leaves it.
		{
			std::lock_guard<std::mutex> lock(volume_mutex_);
			stream_open_.store(true);
			if (driven_ != nullptr && driven_->load() && !stopping_.load())
				takeOverVolumeLocked(device_);
			else
				handBackVolumeLocked(device_);
		}
		return true;
	}

	// Records the device's own level and mute (OwedVolume.h: unless a setting is already owed to it, then the
	// device is still at our 100% and the recorded setting is its own), then sets it to 100% and unmuted. If the
	// device's setting can't be read, the device is left alone. Caller holds volume_mutex_. device may be null,
	// then the device is looked up by its ID.
	void takeOverVolumeLocked(IMMDevice* device)
	{
		auto take_over = [this](IAudioEndpointVolume* volume) -> bool
		{
			float level = 1.0f;
			BOOL mute = FALSE;
			if (!owedVolumeGet(device_id_, &level, &mute))
			{
				if (FAILED(volume->GetMasterVolumeLevelScalar(&level)) || FAILED(volume->GetMute(&mute)))
					return false;
				owedVolumeRecord(device_id_, level, mute);
			}
			setFullVolume(volume, NULL);
			volume_taken_ = true;
			return true;
		};

		if (device != nullptr)
			withEndpointVolume(device, take_over);
		else
			withEndpointVolumeById(device_id_, take_over);
	}

	// Puts the device's own level and mute back if they are owed (whoever took the device over: this output, an
	// earlier one for the same device, or a run of the app that crashed). If the device is not there, the setting
	// stays owed and is put back later (next stream open while not driven, power off, removal, or the engine's
	// sweep of devices it does not use). Caller holds volume_mutex_. device may be null, then the device is looked
	// up by its ID.
	void handBackVolumeLocked(IMMDevice* device)
	{
		volume_taken_ = false;

		float level = 1.0f;
		BOOL mute = FALSE;
		if (!owedVolumeGet(device_id_, &level, &mute))
			return;

		auto hand_back = [level, mute](IAudioEndpointVolume* volume) -> bool
		{
			return restoreVolume(volume, level, mute);
		};

		const bool done = (device != nullptr) ? withEndpointVolume(device, hand_back) : withEndpointVolumeById(device_id_, hand_back);
		if (done)
			owedVolumeErase(device_id_);
	}

	void closeClient()
	{
		stream_open_.store(false);
		if (client_ != nullptr && running_)
			client_->Stop();
		running_ = false;
		safeRelease(render_);
		safeRelease(client_);
		safeRelease(device_);
		buffer_frames_ = 0;
	}

	void resetResampler()
	{
		std::memset(history_, 0, sizeof(history_));
		frac_ = 3.0; // Pull three frames before the first output frame.
		ratio_ = 1.0;
		error_avg_ = 0.0;
		hold_frames_ = 0;
		fill_avg_valid_ = false;
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
		if (FAILED(client_->GetCurrentPadding(&padding)))
			return false;

		const UINT32 space = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
		const DWORD now = GetTickCount();

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
		// Silence still to be played before the queued audio (a raised delay) counts as queued.
		UINT32 fill = write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_relaxed) + holdQueued();

		if (primed_)
		{
			// The delay was changed: from a few ms on, the output moves by the change at once, as the carrier
			// does, instead of leaving it to the drift correction below, which moves at most 0.5% and would take
			// up to a minute to get there. The output fades out first (realign_pending_), then the queued audio
			// is cut by the change or that much silence is played first, then it fades back in.
			const UINT32 moved = target > aligned_target_ ? target - aligned_target_ : aligned_target_ - target;
			if (!realign_pending_ && moved >= static_cast<UINT32>(rate * kRealignMs / 1000.0))
				realign_pending_ = true;

			if (realign_pending_)
			{
				// Handled after this block's fade out.
			}
			else if (fill > target + slack)
			{
				// Far too much audio queued (the device stalled): jump ahead.
				read_pos_.store(read_pos_.load(std::memory_order_relaxed) + (fill - target), std::memory_order_release);
				resetResampler();
				fill = target;
			}
			else if (fill + slack < target)
			{
				// Far too little: build the buffer up again before playing.
				primed_ = false;
				idle_since_ = now;
			}
		}

		if (!primed_)
		{
			// BetterFxSound: a sound shorter than the target after silence (nothing more came for kFlushMs) is played
			// too, at the moment more audio would have filled the queue to the target, as the carrier does
			// (sndDevicesDoCapture()). It used to wait for the next sound to fill the rest, and then play late.
			// The time since the last write is measured with QPC, as the carrier does: GetTickCount() is ~15.6 ms coarse.
			// The write time is read after fill (write_pos_ acquire above), so it is the time of that block or newer.
			LARGE_INTEGER qpc_now;
			QueryPerformanceCounter(&qpc_now);
			const LONGLONG since_write_qpc = qpc_now.QuadPart - last_write_qpc_.load(std::memory_order_relaxed);
			const double since_write = since_write_qpc > 0 ? static_cast<double>(since_write_qpc) / qpc_freq_ : 0.0; // seconds
			const bool flush = fill > 0 && fill < target && since_write * 1000.0 >= static_cast<double>(kFlushMs);
			if ((fill >= target && fill > 0) || flush)
			{
				primed_ = true;
				resetResampler();
				aligned_target_ = target;
				realign_pending_ = false;

				// BetterFxSound: a flush plays silence first, for what is missing to the target less the time already
				// waited since the last write.
				const UINT32 queued = write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_relaxed);
				if (queued < target)
				{
					const double waited = since_write * rate;
					const double missing = static_cast<double>(target - queued) - waited;
					hold_frames_ = missing > 0.0 ? static_cast<UINT32>(missing / nominal) : 0;
				}
				fill = queued + holdQueued();

				if (!running_)
				{
					if (FAILED(client_->Start()))
						return false;
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
		const double error_secs = (static_cast<double>(fill) - static_cast<double>(target)) / rate;
		error_avg_ += (error_secs - error_avg_) * 0.02;
		// Short average of the queue level (a few device periods). The writes come in bursts, so the level at one
		// moment is not the level the output plays from; the lasting-offset realign below moves by this average.
		if (!fill_avg_valid_)
		{
			fill_avg_ = static_cast<double>(fill);
			fill_avg_valid_ = true;
		}
		else
		{
			fill_avg_ += (static_cast<double>(fill) - fill_avg_) * 0.25;
		}
		// A lasting offset is lined up at once too: typically a Bluetooth stream that was idle and took ~0.1 s
		// to start consuming after Start() while the queue kept filling. Left to the drift correction, the
		// output stayed that much late, out of sync with the others, for about a minute after every pause in
		// the audio. Clock drift alone keeps the smoothed error near 1 ms.
		if (!realign_pending_ && (error_avg_ > kRealignErrorSecs || error_avg_ < -kRealignErrorSecs))
			realign_pending_ = true;
		double adjust = error_avg_ * 0.1;
		if (adjust > 0.005) adjust = 0.005;
		if (adjust < -0.005) adjust = -0.005;
		ratio_ = nominal * (1.0 + adjust);

		BYTE* buffer = nullptr;
		if (FAILED(render_->GetBuffer(space, &buffer)) || buffer == nullptr)
			return false;

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
			// wait for the buffer to fill to the target again.
			std::memset(out + static_cast<size_t>(produced) * channels_, 0, static_cast<size_t>(space - produced) * channels_ * sizeof(float));
			primed_ = false;
			idle_since_ = now;
		}
		else if (realign_pending_ && (current_gain_ < 0.01f || hold_frames_ > 0))
		{
			// Faded out: move the output. For a delay change, by exactly the change, so the queue keeps its
			// level relative to the target (the writes are bursty, the instantaneous level is not the mean). For
			// a lasting offset, by the offset of the averaged level. Later: skip that much queued audio. Earlier:
			// play that much silence first. Either way the output fades back in from silence.
			const UINT32 r = read_pos_.load(std::memory_order_relaxed);
			const UINT32 queued = write_pos_.load(std::memory_order_acquire) - r;
			const INT64 shift = (target != aligned_target_)
				? static_cast<INT64>(aligned_target_) - static_cast<INT64>(target)
				: static_cast<INT64>(fill_avg_ + 0.5) - static_cast<INT64>(target);
			if (shift > 0)
			{
				// Silence not played yet goes first, then queued audio.
				const UINT32 hold_queued = holdQueued();
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
			fill_avg_ -= static_cast<double>(shift);	// Re-seeded instead if resetResampler() ran
			error_avg_ = 0.0;
			current_gain_ = 0.0f;
			aligned_target_ = target;
			realign_pending_ = false;
		}

		return SUCCEEDED(render_->ReleaseBuffer(space, 0));
	}

	bool writeSilence(UINT32 frames)
	{
		if (frames == 0)
			return true;
		BYTE* buffer = nullptr;
		if (FAILED(render_->GetBuffer(frames, &buffer)))
			return false;
		return SUCCEEDED(render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT));
	}

	SecondaryOutput(const SecondaryOutput&) = delete;
	SecondaryOutput& operator=(const SecondaryOutput&) = delete;

	const std::wstring device_id_;
	const std::shared_ptr<std::atomic<bool>> driven_;	// The router's flag
	std::atomic<bool> stream_open_{ false };	// Written by the render thread
	std::atomic<bool> stopping_{ false };		// requestStop() was called: the device is not taken over any more

	// The device's own volume is recorded in the owed volume record (OwedVolume.h) when the engine takes the
	// device over (100% and unmuted), and put back when it stops using it. Stream open (render thread), power
	// on/off and removal (UI thread), always under volume_mutex_ and never while rendering.
	std::mutex volume_mutex_;
	bool volume_taken_ = false;		// This output set the device to 100% since it last handed it back
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
	double qpc_freq_ = 1.0;                     // QueryPerformanceFrequency(), set once in the constructor

	HANDLE stop_event_ = NULL;
	HANDLE audio_event_ = NULL;
	HANDLE thread_ = NULL;

	// Render thread only.
	IMMDevice* device_ = nullptr;
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
	double fill_avg_ = 0.0;			// Short average of the queue level, silence to play included (frames)
	bool fill_avg_valid_ = false;
	bool realign_pending_ = false;	// The delay changed: fading out before lining up with the new target
};

bool setDeviceFullVolumeById(const std::wstring& device_id, const GUID* event_context)
{
	return withEndpointVolumeById(device_id, [event_context](IAudioEndpointVolume* volume) -> bool
	{
		return setFullVolume(volume, event_context);
	});
}

bool handBackOwedVolumeById(const std::wstring& device_id)
{
	float level = 1.0f;
	BOOL mute = FALSE;
	if (!owedVolumeGet(device_id, &level, &mute))
		return true;

	const bool done = withEndpointVolumeById(device_id, [level, mute](IAudioEndpointVolume* volume) -> bool
	{
		return restoreVolume(volume, level, mute);
	});
	if (done)
		owedVolumeErase(device_id);
	return done;
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
	: driven_(std::make_shared<std::atomic<bool>>(false))
{
	InitializeSRWLock(&lock_);
}

MultiOutputRouter::~MultiOutputRouter()
{
	stopAll();
}

void MultiOutputRouter::configure(const std::vector<AdditionalOutputConfig>& outputs)
{
	// Fast path: same devices as now, only settings (delay, volume, channels) changed. Those
	// are atomics, so a shared lock is enough and the audio thread keeps running untouched.
	// This is what happens while a slider is being dragged.
	{
		AcquireSRWLockShared(&lock_);
		bool same_devices = (outputs.size() == outputs_.size());
		for (size_t i = 0; same_devices && i < outputs.size(); i++)
		{
			if (outputs_[i] == nullptr || outputs_[i]->deviceId() != outputs[i].device_id)
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

	std::vector<std::shared_ptr<SecondaryOutput>> next;
	std::vector<std::shared_ptr<SecondaryOutput>> removed;

	AcquireSRWLockExclusive(&lock_);

	for (const auto& config : outputs)
	{
		if (config.device_id.empty())
			continue;

		bool duplicate = false;
		for (const auto& output : next)
		{
			if (output->deviceId() == config.device_id)
				duplicate = true;
		}
		if (duplicate)
			continue;

		std::shared_ptr<SecondaryOutput> output;
		for (auto& existing : outputs_)
		{
			if (existing != nullptr && existing->deviceId() == config.device_id)
			{
				output = std::move(existing);
				break;
			}
		}

		if (output != nullptr)
			output->applyConfig(config);
		else
			output = SecondaryOutput::create(config, driven_);

		next.push_back(std::move(output));
	}

	for (auto& existing : outputs_)
	{
		if (existing != nullptr)
			removed.push_back(std::move(existing));
	}

	outputs_.swap(next);

	ReleaseSRWLockExclusive(&lock_);

	// Stopping the render threads of removed outputs happens outside the lock so the audio thread is never held
	// up by it. Once a thread has exited, dropping the last reference here hands the device back.
	stopOutputs(removed);
	removed.clear();
}

void MultiOutputRouter::stopAll()
{
	configure(std::vector<AdditionalOutputConfig>());
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

void MultiOutputRouter::write(const float* data, UINT32 frames, UINT32 channels, UINT32 sample_rate, int solo_index)
{
	// Never wait on the audio thread: if the list is being changed right now, skip this block.
	if (!TryAcquireSRWLockShared(&lock_))
		return;

	const bool solo_active = (solo_index >= -1);
	for (size_t i = 0; i < outputs_.size(); i++)
	{
		const bool silent = solo_active && static_cast<int>(i) != solo_index;
		outputs_[i]->write(data, frames, channels, sample_rate, silent);
	}

	ReleaseSRWLockShared(&lock_);
}
