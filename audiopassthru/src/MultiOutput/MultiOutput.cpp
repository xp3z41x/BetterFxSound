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
	DWORD defaultChannelMask(UINT32 channels)
	{
		switch (channels)
		{
		case 1: return 0x4;     // FRONT_CENTER
		case 2: return 0x3;     // STEREO
		case 4: return 0x33;    // QUAD
		case 6: return 0x3F;    // 5.1
		case 8: return 0x63F;   // 7.1 surround
		default: return 0;
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
 * apart. Sample rate and channel count conversion to the device's own mix format is done by
 * the Windows audio engine (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM).
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

		const UINT32 format = packFormat(channels, sample_rate);
		if (active_format_.load(std::memory_order_acquire) != format)
		{
			// The render thread (re)opens the device for this format, then publishes it in
			// active_format_. Until then the audio is simply not queued for this output.
			if (pending_format_.load(std::memory_order_relaxed) != format)
			{
				pending_format_.store(format, std::memory_order_release);
				SetEvent(audio_event_);
			}
			return;
		}

		const UINT32 w = write_pos_.load(std::memory_order_relaxed);
		const UINT32 r = read_pos_.load(std::memory_order_acquire);
		const UINT32 free_frames = kRingFrames - (w - r);
		const UINT32 n = frames < free_frames ? frames : free_frames;

		// Every channel is queued as is. The channel mode is applied by render(), on what is played now, so a
		// change is heard within one device period rather than after the queued audio (latency + delay).
		for (UINT32 i = 0; i < n; i++)
		{
			float* dst = &ring_[static_cast<size_t>((w + i) & kRingMask) * kMaxChannels];
			const float* src = data + static_cast<size_t>(i) * channels;

			if (silent)
				std::memset(dst, 0, channels * sizeof(float));
			else
				std::memcpy(dst, src, channels * sizeof(float));
		}
		// BetterFxSound: the write time is stored before the block is published, so render() never sees the new block
		// with the time of an older one (and flushes it at once as if nothing had come for a while).
		LARGE_INTEGER qpc_now;
		QueryPerformanceCounter(&qpc_now);
		last_write_qpc_.store(qpc_now.QuadPart, std::memory_order_relaxed);
		write_pos_.store(w + n, std::memory_order_release);
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

			const UINT32 pending = pending_format_.load(std::memory_order_acquire);
			const DWORD now = GetTickCount();

			if (pending != 0 && (pending != open_format_ || client_ == nullptr))
			{
				if (pending == open_format_ && static_cast<LONG>(now - next_retry) < 0)
					continue; // Same format failed recently, wait before retrying.

				active_format_.store(0, std::memory_order_release);
				closeClient();
				open_format_ = pending;

				if (openClient(formatChannels(pending), formatRate(pending)))
				{
					// Nothing is being written while active_format_ is 0, so the reader can
					// safely discard anything left in the ring.
					read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
					primed_ = false;
					idle_since_ = now;
					active_format_.store(pending, std::memory_order_release);
				}
				else
				{
					closeClient();
					next_retry = now + kRetryMs;
				}
				continue;
			}

			if (client_ != nullptr && !render())
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

	bool openClient(UINT32 channels, UINT32 sample_rate)
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

		// We hand the audio engine our own format (the processing format) and let it convert
		// sample rate and channel layout to the device's mix format.
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
		sample_rate_ = sample_rate;
		running_ = false;

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
		std::memcpy(history_[3], &ring_[static_cast<size_t>(r & kRingMask) * kMaxChannels], sizeof(float) * channels_);
		read_pos_.store(r + 1, std::memory_order_release);
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
		const double rate = static_cast<double>(sample_rate_);
		const UINT32 target = static_cast<UINT32>((kBaseLatencyMs + delay_ms_.load(std::memory_order_relaxed)) * rate / 1000.0);
		const UINT32 slack = static_cast<UINT32>(rate * 0.15); // 150 ms
		// Silence still to be played before the queued audio (a raised delay) counts as queued.
		UINT32 fill = write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_relaxed) + hold_frames_;

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
					hold_frames_ = missing > 0.0 ? static_cast<UINT32>(missing) : 0;
				}
				fill = queued + hold_frames_;

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
		ratio_ = 1.0 + adjust;

		BYTE* buffer = nullptr;
		if (FAILED(render_->GetBuffer(space, &buffer)) || buffer == nullptr)
			return false;

		float* out = reinterpret_cast<float*>(buffer);
		UINT32 produced = 0;
		// This output's own trim times the Windows master volume (level and mute), see MasterVolume.h. Before a
		// realignment, a fast fade out (~2 ms) instead.
		const float gain_target = realign_pending_ ? 0.0f : target_gain_.load(std::memory_order_relaxed) * masterVolumeGain();
		const float gain_step = static_cast<float>(1.0 / ((realign_pending_ ? 0.002 : 0.01) * rate)); // One-pole ramp
		// The channel mode, for this whole period. The stream has the processing format's channels
		// (WAVEFORMATEXTENSIBLE order: front left, front right, ...); Windows converts it to the device.
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
			float* frame = out + static_cast<size_t>(produced) * channels_;
			for (UINT32 c = 0; c < channels_; c++)
			{
				const float xm1 = history_[0][c];
				const float x0 = history_[1][c];
				const float x1 = history_[2][c];
				const float x2 = history_[3][c];
				const float c1 = 0.5f * (x1 - xm1);
				const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
				const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
				frame[c] = (((c3 * t + c2) * t + c1) * t + x0) * gain;
			}
			applyOutputChannelMode(frame, 1, channels_, mode);
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
				const UINT32 from_hold = static_cast<UINT64>(shift) < hold_frames_ ? static_cast<UINT32>(shift) : hold_frames_;
				hold_frames_ -= from_hold;
				const UINT64 rest = static_cast<UINT64>(shift) - from_hold;
				if (rest > 0)
				{
					read_pos_.store(r + (rest < queued ? static_cast<UINT32>(rest) : queued), std::memory_order_release);
					resetResampler();
				}
			}
			else if (shift < 0)
			{
				hold_frames_ += static_cast<UINT32>(-shift);
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
	std::atomic<UINT32> active_format_{ 0 };  // Format the ring and the device are set up for, 0 = none
	std::atomic<UINT32> pending_format_{ 0 }; // Format the audio thread is producing
	std::atomic<LONGLONG> last_write_qpc_{ 0 }; // QPC of the last block queued by the audio thread, stored before write_pos_
	double qpc_freq_ = 1.0;                     // QueryPerformanceFrequency(), set once in the constructor

	HANDLE stop_event_ = NULL;
	HANDLE audio_event_ = NULL;
	HANDLE thread_ = NULL;

	// Render thread only.
	IMMDevice* device_ = nullptr;
	IAudioClient* client_ = nullptr;
	IAudioRenderClient* render_ = nullptr;
	UINT32 open_format_ = 0;
	UINT32 buffer_frames_ = 0;
	UINT32 channels_ = 0;
	UINT32 sample_rate_ = 0;
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
		float value;
		if (mode == OutputChannelMode::Left)
			value = frame[0];
		else if (mode == OutputChannelMode::Right)
			value = frame[1];
		else
			value = 0.5f * (frame[0] + frame[1]);

		for (UINT32 c = 0; c < channels; c++)
			frame[c] = value;
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
