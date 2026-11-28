/*
BetterFxSound
Copyright (C) 2026  BetterFxSound contributors

This file is part of BetterFxSound, a modified version of FxSound
(Copyright (C) 2025 FxSound LLC).

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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "DelayCalibration.h"
#include "MasterVolume.h"
#include "RenderSession.h"

namespace
{
	const GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

	constexpr UINT32 kRate = 48000;
	constexpr double kPi = 3.14159265358979323846;

	// Test signal: exponential sweep from 500 Hz to 6 kHz, 100 ms, with 5 ms fades.
	constexpr double kChirpSeconds = 0.1;
	constexpr double kChirpStartHz = 500.0;
	constexpr double kChirpEndHz = 6000.0;
	constexpr double kChirpFadeSeconds = 0.005;
	constexpr float kChirpLevel = 0.5f;
	// Every output plays the sweep scaled by the Windows volume (gain = slider squared). When that gain is
	// below kChirpBoostBelowGain (slider 50%) the sweep is made louder, by at most kChirpMaxBoost (+6 dB,
	// full scale), so it stays audible at low volume. It is never louder than an unboosted sweep at 50%.
	constexpr float kChirpBoostBelowGain = 0.25f;
	constexpr float kChirpMaxBoost = 2.0f;

	// Schedule of one measurement.
	constexpr int kRepetitions = 3;
	constexpr DWORD kWarmUpMs = 1500;		// Silence played to every output before the first sweep: streams that were
											// idle start again, outputs fill their delay, devices that wake up slowly
											// (HDMI, Bluetooth) do not swallow the first sweep
	constexpr DWORD kSettleMs = 300;		// Silence before the sweep, lets the previous one die out
	constexpr DWORD kListenMs = 1300;		// Recording after the sweep is queued (it plays ~100 ms later), plus the output's delay
	constexpr DWORD kTailMs = 200;			// Extra recording after the last sweep, so its whole search window is recorded
	constexpr double kMaxLatencySeconds = 1.0; // Search window for the arrival, plus the output's delay

	// The correlation at the chosen arrival must stand this far above the correlation's RMS to count.
	constexpr double kMinConfidence = 8.0;
	// The direct sound is the earliest strong peak, but no earlier than this before the strongest one:
	// a reflection comes a few ms after the direct sound, while noise can be anywhere.
	constexpr double kLookBackSeconds = 0.03;
	// Repetitions of one output must agree within this to be trusted, at least two of them. Noise picks
	// land anywhere in the window, real arrivals stay together (Bluetooth can drift ~30 ms per repetition).
	constexpr double kAgreeMs = 40.0;

	constexpr REFERENCE_TIME kStreamBufferHns = 1000000; // 100 ms

	const wchar_t* const kNoOutputsError = L"No output is turned on. Turn on at least one output, then synchronize again.";

	template <class T> void safeRelease(T*& p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	// BetterFxSound: property keys written out here (PKEY_Device_ContainerId, PKEY_AudioEndpoint_FormFactor), so no
	// property key definition has to be linked in.
	const PROPERTYKEY kContainerIdKey = { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
	const PROPERTYKEY kFormFactorKey = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };
	// Every built-in device of the computer is in this container; no container reads as all zeros.
	const GUID kComputerContainerId = { 0x00000000, 0x0000, 0x0000, { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	const GUID kNoContainerId = { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

	bool readContainerId(IMMDevice* device, GUID* container)
	{
		IPropertyStore* props = nullptr;
		if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &props)) || props == nullptr)
			return false;

		bool read = false;
		PROPVARIANT value;
		PropVariantInit(&value);
		if (SUCCEEDED(props->GetValue(kContainerIdKey, &value)) && value.vt == VT_CLSID && value.puuid != nullptr)
		{
			*container = *value.puuid;
			read = !IsEqualGUID(*container, kNoContainerId) && !IsEqualGUID(*container, kComputerContainerId);
		}
		PropVariantClear(&value);
		props->Release();
		return read;
	}

	bool isHeadsetMicrophone(IMMDevice* device)
	{
		IPropertyStore* props = nullptr;
		if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &props)) || props == nullptr)
			return false;

		bool headset = false;
		PROPVARIANT value;
		PropVariantInit(&value);
		if (SUCCEEDED(props->GetValue(kFormFactorKey, &value)) && value.vt == VT_UI4)
			headset = (value.ulVal == static_cast<ULONG>(Headset)) || (value.ulVal == static_cast<ULONG>(Handset));
		PropVariantClear(&value);
		props->Release();
		return headset;
	}

	/*
	 * BetterFxSound: true when the microphone is the hands-free microphone of headphones (or a headset) that are one
	 * of the outputs being measured. Opening it switches Bluetooth headphones to their hands-free mode, which degrades
	 * or interrupts their playback during the measurement, and with a wired headset it only hears its own earcups.
	 */
	bool microphoneIsPartOfOutput(IMMDeviceEnumerator* enumerator, IMMDevice* microphone, const std::vector<std::wstring>& output_ids)
	{
		GUID microphone_container;
		if (enumerator == nullptr || !isHeadsetMicrophone(microphone) || !readContainerId(microphone, &microphone_container))
			return false;

		for (const auto& id : output_ids)
		{
			IMMDevice* output = nullptr;
			if (id.empty() || FAILED(enumerator->GetDevice(id.c_str(), &output)) || output == nullptr)
				continue;

			GUID output_container;
			const bool same = readContainerId(output, &output_container) && IsEqualGUID(output_container, microphone_container);
			output->Release();
			if (same)
				return true;
		}
		return false;
	}

	std::vector<float> makeChirp()
	{
		const size_t length = static_cast<size_t>(kChirpSeconds * kRate);
		const size_t fade = static_cast<size_t>(kChirpFadeSeconds * kRate);
		const double k = std::log(kChirpEndHz / kChirpStartHz);
		std::vector<float> chirp(length);

		for (size_t i = 0; i < length; i++)
		{
			const double t = static_cast<double>(i) / kRate;
			const double phase = 2.0 * kPi * kChirpStartHz * kChirpSeconds / k * (std::exp(t * k / kChirpSeconds) - 1.0);
			double gain = 1.0;
			if (i < fade)
				gain = 0.5 - 0.5 * std::cos(kPi * i / fade);
			else if (i >= length - fade)
				gain = 0.5 - 0.5 * std::cos(kPi * (length - 1 - i) / fade);
			chirp[i] = static_cast<float>(std::sin(phase) * gain);
		}
		return chirp;
	}

	void fillFloatFormat(WAVEFORMATEXTENSIBLE& wfx, WORD channels)
	{
		std::memset(&wfx, 0, sizeof(wfx));
		wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wfx.Format.nChannels = channels;
		wfx.Format.nSamplesPerSec = kRate;
		wfx.Format.wBitsPerSample = 32;
		wfx.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
		wfx.Format.nAvgBytesPerSec = kRate * wfx.Format.nBlockAlign;
		wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		wfx.Samples.wValidBitsPerSample = 32;
		wfx.dwChannelMask = (channels == 1) ? 0x4 : 0x3;
		wfx.SubFormat = kSubtypeIeeeFloat;
	}

	double median(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		const size_t n = values.size();
		return (n % 2) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
	}

	// A point on a stream's timeline: sample index (or stream seconds) at a QPC time in seconds.
	struct Anchor
	{
		double stream;
		double qpc_seconds;
	};

	// Puts all outputs back on when the measurement ends, however it ends.
	struct SoloGuard
	{
		const DelayCalibration::SoloFunction& solo;
		~SoloGuard()
		{
			if (solo)
				solo(DelayCalibration::kSoloNone);
		}
	};

	// The two WASAPI streams used for a measurement.
	class Streams
	{
	public:
		~Streams()
		{
			if (render_client_ != nullptr) render_client_->Stop();
			if (capture_client_ != nullptr) capture_client_->Stop();
			safeRelease(clock_);
			safeRelease(render_);
			safeRelease(render_client_);
			safeRelease(capture_);
			safeRelease(capture_client_);
		}

		// Returns an empty string on success, otherwise what went wrong.
		std::wstring open(const std::wstring& virtual_device_id, const std::vector<std::wstring>& output_ids)
		{
			IMMDeviceEnumerator* enumerator = nullptr;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
				return L"Windows audio is not available.";

			IMMDevice* render_device = nullptr;
			IMMDevice* capture_device = nullptr;
			std::wstring error;

			if (FAILED(enumerator->GetDevice(virtual_device_id.c_str(), &render_device)))
				error = L"The FxSound audio device was not found.";
			else if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &capture_device)))
				error = L"No microphone was found. Connect one and set it as the default recording device in Windows.";
			else if (microphoneIsPartOfOutput(enumerator, capture_device, output_ids))
				error = L"The default microphone is part of headphones that are being synchronized. Set another microphone as the default recording device in Windows, then synchronize again.";

			safeRelease(enumerator);

			if (error.empty())
			{
				// BetterFxSound: in the engine's private render session (RenderSession.h), at 100% and unmuted.
				const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | kRenderSessionFlags;
				WAVEFORMATEXTENSIBLE wfx;

				fillFloatFormat(wfx, 2);
				if (FAILED(render_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&render_client_)) ||
					FAILED(render_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kStreamBufferHns, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), &kRenderSessionGuid)) ||
					FAILED(render_client_->GetBufferSize(&render_frames_)) ||
					FAILED(render_client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_)) ||
					FAILED(render_client_->GetService(__uuidof(IAudioClock), (void**)&clock_)) ||
					FAILED(clock_->GetFrequency(&clock_frequency_)) || clock_frequency_ == 0)
				{
					error = L"The test sound could not be played.";
				}
				else
				{
					resetRenderSessionVolume(render_client_);
				}
			}

			if (error.empty())
			{
				const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
				WAVEFORMATEXTENSIBLE wfx;

				fillFloatFormat(wfx, 1);
				if (FAILED(capture_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&capture_client_)) ||
					FAILED(capture_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2 * kStreamBufferHns, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), NULL)) ||
					FAILED(capture_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_)))
				{
					error = L"The microphone could not be opened. Check that apps are allowed to use it in the Windows privacy settings.";
				}
			}

			safeRelease(render_device);
			safeRelease(capture_device);

			if (!error.empty())
				return error;

			if (FAILED(capture_client_->Start()))
				return L"The microphone could not be started.";

			if (!pumpRender() || FAILED(render_client_->Start()))
				return L"The test sound could not be played.";

			return std::wstring();
		}

		// Plays the sweep from the next frame that is written, at this peak level.
		void scheduleChirp(const std::vector<float>* chirp, float level)
		{
			chirp_ = chirp;
			chirp_level_ = level;
			chirp_start_ = written_;
		}

		UINT64 lastChirpStart() const { return chirp_start_; }

		bool pump()
		{
			return pumpRender() && pumpCapture();
		}

		// QPC time (seconds) at which render frame 'frame' was played by the virtual device.
		bool renderFrameTime(UINT64 frame, double* qpc_seconds) const
		{
			if (render_anchors_.empty())
				return false;

			const double frame_seconds = static_cast<double>(frame) / kRate;
			const Anchor* best = &render_anchors_.front();
			for (const auto& anchor : render_anchors_)
			{
				if (std::fabs(anchor.stream - frame_seconds) < std::fabs(best->stream - frame_seconds))
					best = &anchor;
			}
			*qpc_seconds = best->qpc_seconds + (frame_seconds - best->stream);
			return true;
		}

		// Microphone sample index recorded at QPC time 'qpc_seconds'.
		bool captureIndexAt(double qpc_seconds, double* index) const
		{
			if (capture_anchors_.empty())
				return false;

			const Anchor* best = &capture_anchors_.front();
			for (const auto& anchor : capture_anchors_)
			{
				if (anchor.qpc_seconds <= qpc_seconds)
					best = &anchor;
			}
			*index = best->stream + (qpc_seconds - best->qpc_seconds) * kRate;
			return true;
		}

		const std::vector<float>& recording() const { return recording_; }

	private:
		bool pumpRender()
		{
			UINT32 padding = 0;
			if (FAILED(render_client_->GetCurrentPadding(&padding)))
				return false;

			const UINT32 space = render_frames_ - padding;
			if (space > 0)
			{
				BYTE* buffer = nullptr;
				if (FAILED(render_->GetBuffer(space, &buffer)))
					return false;

				float* out = reinterpret_cast<float*>(buffer);
				for (UINT32 i = 0; i < space; i++)
				{
					const UINT64 frame = written_ + i;
					float sample = 0.0f;
					if (chirp_ != nullptr && frame >= chirp_start_ && frame - chirp_start_ < chirp_->size())
						sample = (*chirp_)[static_cast<size_t>(frame - chirp_start_)] * chirp_level_;
					out[2 * i] = sample;
					out[2 * i + 1] = sample;
				}

				if (FAILED(render_->ReleaseBuffer(space, 0)))
					return false;
				written_ += space;
			}

			UINT64 position = 0;
			UINT64 qpc = 0;
			if (SUCCEEDED(clock_->GetPosition(&position, &qpc)) && qpc != 0)
			{
				Anchor anchor = { static_cast<double>(position) / static_cast<double>(clock_frequency_), static_cast<double>(qpc) * 1e-7 };
				render_anchors_.push_back(anchor);
			}
			return true;
		}

		bool pumpCapture()
		{
			UINT32 packet = 0;
			if (FAILED(capture_->GetNextPacketSize(&packet)))
				return false;

			while (packet > 0)
			{
				BYTE* data = nullptr;
				UINT32 frames = 0;
				DWORD flags = 0;
				UINT64 device_position = 0;
				UINT64 qpc = 0;

				if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &device_position, &qpc)))
					return false;

				if (qpc != 0)
				{
					Anchor anchor = { static_cast<double>(recording_.size()), static_cast<double>(qpc) * 1e-7 };
					capture_anchors_.push_back(anchor);
				}

				const float* samples = reinterpret_cast<const float*>(data);
				for (UINT32 i = 0; i < frames; i++)
					recording_.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : samples[i]);

				if (FAILED(capture_->ReleaseBuffer(frames)))
					return false;
				if (FAILED(capture_->GetNextPacketSize(&packet)))
					return false;
			}
			return true;
		}

		IAudioClient* render_client_ = nullptr;
		IAudioRenderClient* render_ = nullptr;
		IAudioClock* clock_ = nullptr;
		UINT64 clock_frequency_ = 0;
		UINT32 render_frames_ = 0;
		UINT64 written_ = 0;
		const std::vector<float>* chirp_ = nullptr;
		float chirp_level_ = kChirpLevel;
		UINT64 chirp_start_ = 0;
		std::vector<Anchor> render_anchors_;

		IAudioClient* capture_client_ = nullptr;
		IAudioCaptureClient* capture_ = nullptr;
		std::vector<float> recording_;
		std::vector<Anchor> capture_anchors_;
	};

	// Finds the sweep in the recording, within 'max_lag' samples after 'start'. Returns the delay in
	// samples and the confidence of that arrival (its correlation / RMS of the correlation), or false
	// if the recording does not cover the whole window.
	bool findArrival(const std::vector<float>& recording, const std::vector<float>& chirp, size_t start, size_t max_lag,
		size_t* lag_out, double* confidence_out)
	{
		if (max_lag == 0 || start + max_lag + chirp.size() > recording.size())
			return false;

		std::vector<double> correlation(max_lag);
		double peak = 0.0;
		size_t peak_lag = 0;
		double sum_squares = 0.0;

		for (size_t lag = 0; lag < max_lag; lag++)
		{
			const float* x = &recording[start + lag];
			double acc = 0.0;
			for (size_t j = 0; j < chirp.size(); j++)
				acc += static_cast<double>(chirp[j]) * x[j];
			const double magnitude = std::fabs(acc);
			correlation[lag] = magnitude;
			sum_squares += magnitude * magnitude;
			if (magnitude > peak)
			{
				peak = magnitude;
				peak_lag = lag;
			}
		}

		const double rms = std::sqrt(sum_squares / max_lag);
		if (peak <= 0.0 || rms <= 0.0)
			return false;

		// Take the earliest strong peak rather than the strongest one: a reflection off a wall
		// can be louder than the direct sound but always arrives later. Only look a little before
		// the strongest peak, otherwise a noise peak long before a weak sweep would be taken.
		const size_t look_back = static_cast<size_t>(kLookBackSeconds * kRate);
		size_t lag = (peak_lag > look_back) ? peak_lag - look_back : 0;
		while (lag < peak_lag && correlation[lag] < 0.5 * peak)
			lag++;
		const size_t refine_end = (std::min)(max_lag, lag + kRate / 500); // within 2 ms
		size_t best = lag;
		for (size_t i = lag; i < refine_end; i++)
		{
			if (correlation[i] > correlation[best])
				best = i;
		}

		*lag_out = best;
		*confidence_out = correlation[best] / rms;
		return true;
	}

	// One output's result from its repetitions: the median of the largest group that agrees within
	// kAgreeMs, if it has at least two of them. On a tie the group with the higher total confidence wins.
	bool combineRepetitions(const std::vector<double>& latencies, const std::vector<double>& confidences,
		double* latency_out, double* confidence_out)
	{
		size_t best_count = 0;
		double best_weight = 0.0;
		std::vector<double> best_latencies;
		std::vector<double> best_confidences;

		for (size_t i = 0; i < latencies.size(); i++)
		{
			std::vector<double> group_latencies;
			std::vector<double> group_confidences;
			double weight = 0.0;
			for (size_t j = 0; j < latencies.size(); j++)
			{
				if (std::fabs(latencies[j] - latencies[i]) <= kAgreeMs)
				{
					group_latencies.push_back(latencies[j]);
					group_confidences.push_back(confidences[j]);
					weight += confidences[j];
				}
			}
			if (group_latencies.size() > best_count || (group_latencies.size() == best_count && weight > best_weight))
			{
				best_count = group_latencies.size();
				best_weight = weight;
				best_latencies.swap(group_latencies);
				best_confidences.swap(group_confidences);
			}
		}

		if (best_count < 2)
			return false;

		*latency_out = median(best_latencies);
		*confidence_out = median(best_confidences);
		return true;
	}
}

DelayCalibration::DelayCalibration()
{
}

DelayCalibration::~DelayCalibration()
{
	cancel();
	if (thread_ != NULL)
	{
		WaitForSingleObject(thread_, 10000);
		CloseHandle(thread_);
	}
}

bool DelayCalibration::start(const std::wstring& virtual_device_id, const std::vector<Target>& targets, SoloFunction solo,
	const std::wstring& no_targets_error, const std::atomic<unsigned int>* stream_generation, unsigned int start_generation)
{
	if (state_.load() == static_cast<int>(State::Running))
		return false;

	if (thread_ != NULL)
	{
		WaitForSingleObject(thread_, INFINITE); // Already finished, state is not Running
		CloseHandle(thread_);
		thread_ = NULL;
	}

	virtual_device_id_ = virtual_device_id;
	targets_ = targets;
	solo_ = solo;
	stream_generation_ = stream_generation;
	start_generation_ = start_generation;
	cancel_.store(false);
	progress_.store(0);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		results_.clear();
		error_.clear();
		abort_reason_.clear();
	}

	// Nothing to measure: fail at once with the reason the caller gave, without a measurement thread.
	if (targets_.empty())
	{
		fail(no_targets_error.empty() ? std::wstring(kNoOutputsError) : no_targets_error);
		return true;
	}

	state_.store(static_cast<int>(State::Running));
	thread_ = CreateThread(NULL, 0, threadProc, this, 0, NULL);
	if (thread_ == NULL)
	{
		state_.store(static_cast<int>(State::Failed));
		return false;
	}
	return true;
}

void DelayCalibration::cancel()
{
	cancel_.store(true);
}

void DelayCalibration::abort(const std::wstring& reason)
{
	if (state_.load() != static_cast<int>(State::Running))
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (abort_reason_.empty())
			abort_reason_ = reason;
	}
	cancel_.store(true);
}

bool DelayCalibration::isStopping() const
{
	return state_.load() == static_cast<int>(State::Running) && cancel_.load();
}

DelayCalibration::State DelayCalibration::getState(int* progress_percent) const
{
	if (progress_percent != nullptr)
		*progress_percent = progress_.load();
	return static_cast<State>(state_.load());
}

std::vector<DelayCalibrationResult> DelayCalibration::getResults() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return results_;
}

std::wstring DelayCalibration::getError() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return error_;
}

DWORD WINAPI DelayCalibration::threadProc(LPVOID param)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	static_cast<DelayCalibration*>(param)->run();
	if (SUCCEEDED(hr))
		CoUninitialize();
	return 0;
}

void DelayCalibration::fail(const std::wstring& message)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		error_ = message;
	}
	state_.store(static_cast<int>(State::Failed));
}

void DelayCalibration::run()
{
	if (targets_.empty())
	{
		fail(kNoOutputsError);
		return;
	}

	const std::vector<float> chirp = makeChirp();

	struct Measurement
	{
		size_t target;
		UINT64 chirp_frame;
	};
	std::vector<Measurement> measurements;

	{
		SoloGuard guard = { solo_ };
		Streams streams;

		std::vector<std::wstring> output_ids;
		for (const auto& target : targets_)
			output_ids.push_back(target.device_id);

		std::wstring error = streams.open(virtual_device_id_, output_ids);
		if (!error.empty())
		{
			fail(error);
			return;
		}

		const size_t total = targets_.size() * kRepetitions;

		// Runs the streams for 'ms' milliseconds. Returns false on error or cancel.
		auto pumpFor = [&](DWORD ms) -> bool
		{
			const DWORD begin = GetTickCount();
			while (GetTickCount() - begin < ms)
			{
				if (cancel_.load())
					return false;
				// BetterFxSound: the processing stopped (a reinit), so the capture and every output were interrupted
				// and restart muted: the sweeps around it are lost or partial, with or without a carrier change.
				if (stream_generation_ != nullptr && stream_generation_->load() != start_generation_)
				{
					this->abort(L"The audio devices changed during the measurement. Synchronize again.");
					return false;
				}
				if (!streams.pump())
					return false;
				Sleep(5);
			}
			return true;
		};

		// Why the streams stopped: a cancel by the user (back to Idle), an abort with its reason, or a device error.
		auto stopped = [&]()
		{
			std::wstring reason;
			if (cancel_.load())
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (abort_reason_.empty())
				{
					error_ = L"Cancelled.";
					state_.store(static_cast<int>(State::Idle));
					return;
				}
				reason = abort_reason_;
			}
			else
			{
				reason = L"An audio device stopped working during the measurement.";
			}
			fail(reason);
		};

		// How much later an output's sweep arrives because of its own delay (0 to 500 ms).
		auto extraMs = [](const Target& target) -> DWORD
		{
			return static_cast<DWORD>((std::max)(0, (std::min)(target.delay_ms, 500)));
		};

		// The sweep follows the Windows volume like any other sound. Make it louder at low volume, see kChirpBoostBelowGain.
		const float master_gain = masterVolumeGain();
		float boost = 1.0f;
		if (master_gain < kChirpBoostBelowGain)
			boost = (master_gain > 0.0f) ? (std::min)(kChirpMaxBoost, std::sqrt(kChirpBoostBelowGain / master_gain)) : kChirpMaxBoost;
		const float chirp_level = kChirpLevel * boost;

		// Wake every output before the first sweep (they all get silence, the first target is soloed).
		if (solo_)
			solo_(targets_.front().solo_index);
		if (!pumpFor(kWarmUpMs))
		{
			stopped();
			return;
		}

		for (int repetition = 0; repetition < kRepetitions; repetition++)
		{
			for (size_t t = 0; t < targets_.size(); t++)
			{
				if (solo_)
					solo_(targets_[t].solo_index);

				if (!pumpFor(kSettleMs))
				{
					stopped();
					return;
				}

				streams.scheduleChirp(&chirp, chirp_level);
				Measurement measurement = { t, streams.lastChirpStart() };
				measurements.push_back(measurement);

				if (!pumpFor(kListenMs + extraMs(targets_[t])))
				{
					stopped();
					return;
				}

				progress_.store(static_cast<int>(90 * measurements.size() / total));
			}
		}

		// The last sweep's search window ends a little after its listening time.
		if (!pumpFor(kTailMs))
		{
			stopped();
			return;
		}

		// Analysis. The streams object still holds the recording and the timing anchors.
		const std::vector<float>& recording = streams.recording();

		double level = 0.0;
		for (float sample : recording)
			level = (std::max)(level, static_cast<double>(std::fabs(sample)));
		if (level < 1e-4)
		{
			fail(L"The microphone recorded only silence. Check that it is on, not muted and set as the default recording device.");
			return;
		}

		std::vector<std::vector<double>> latencies(targets_.size());
		std::vector<std::vector<double>> confidences(targets_.size());

		for (const auto& measurement : measurements)
		{
			double emit_time = 0.0;
			double start_index = 0.0;
			if (!streams.renderFrameTime(measurement.chirp_frame, &emit_time) ||
				!streams.captureIndexAt(emit_time, &start_index) || start_index < 0.0)
				continue;

			const double window_seconds = kMaxLatencySeconds + extraMs(targets_[measurement.target]) / 1000.0;
			const size_t max_lag = static_cast<size_t>(window_seconds * kRate);

			size_t lag = 0;
			double confidence = 0.0;
			if (!findArrival(recording, chirp, static_cast<size_t>(start_index), max_lag, &lag, &confidence))
				continue;

			if (confidence >= kMinConfidence)
			{
				latencies[measurement.target].push_back(1000.0 * lag / kRate);
				confidences[measurement.target].push_back(confidence);
			}
		}

		std::vector<DelayCalibrationResult> results;
		for (size_t t = 0; t < targets_.size(); t++)
		{
			DelayCalibrationResult result;
			result.device_id = targets_[t].device_id;
			// Need at least two of the three measurements to agree to trust the result.
			double latency = 0.0;
			double confidence = 0.0;
			if (combineRepetitions(latencies[t], confidences[t], &latency, &confidence))
			{
				result.measured = true;
				result.latency_ms = latency;
				result.confidence = confidence;
			}
			results.push_back(result);
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			results_ = results;
		}
	}

	progress_.store(100);
	state_.store(static_cast<int>(State::Done));
}
