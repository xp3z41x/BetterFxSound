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
#pragma once

#ifndef __ANDROID__
#include <windows.h>
#endif //WIN32
#include "AudioPassthru.h"
#include "sndDevices.h"
#include "MultiOutput.h"
#include "DelayCalibration.h"
#include <Mmdeviceapi.h>
#include <vector> 
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class DspProcessingModule
{
public:
	virtual int setSignalFormat(int i_bps, int i_nch, int i_srate, int i_valid_bits) = 0;
	virtual int processAudio(short int *si_input_samples, short int *si_output_samples, int i_num_sample_sets, int i_check_for_duplicate_buffers) = 0;
};

class AudioPassthruPrivate
{
public:
	AudioPassthruPrivate();
	~AudioPassthruPrivate();
	// BetterFxSound: stops everything and hands the devices back (the destructor runs it if it was not called). False if
	// the processing thread may still be running: the object must then never be destroyed (~AudioPassthru()).
	bool shutdown();
	int init();
	void mute(bool mute);
	std::vector<SoundDevice> getSoundDevices(bool active_devices = true);
	int killProcessingThread(int *ip_timed_out);
	int setBufferLength(int i_buffer_length_msecs);
	int processTimer();
	void setDspProcessingModule(DfxDsp* p_dfx_dsp);
	static DWORD WINAPI processingThread(LPVOID lpParam);
	DWORD threadWorker(HANDLE h_wait_timer); // Needs to be public to be called from static thread starter function
	int setTargetedRealPlaybackDevice(const std::wstring sound_device_guid);
	void registerCallback(AudioPassthruCallback *callback);
    bool isPlaybackDeviceAvailable();
	bool checkDeviceChanges();
	bool isCarrierSwitchPending();
	std::wstring getFailedCarrierId();
	void restoreDefaultPlaybackDevice();
	void setOutputs(const std::vector<OutputConfig>& outputs);
	bool startDelayCalibration();
	void cancelDelayCalibration();
	int getDelayCalibrationState(int* progress_percent);
	std::vector<DelayCalibrationResult> getDelayCalibrationResults();
	std::wstring getDelayCalibrationError();
	std::vector<AdditionalOutputStatus> getAdditionalOutputStatus();
	EngineStatus getEngineStatus();

private:
	void applyMainDelay(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate, bool silence_fill);
	void applyCarrierGain(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate, bool enabled);

	// Recomputes the carrier settings and the additional outputs from the requested outputs and the
	// current carrier. Called after every change and after every reinit (the carrier may have changed).
	void applyAdditionalOutputs();
	// Puts the own level and mute back on every present device that is owed them (OwedVolume.h) and is not in
	// in_use, the devices the engine drives right now. UI/timer thread, never under config_mutex_ (COM calls).
	void handBackUnusedDevices(const std::vector<std::wstring>& in_use);
	// BetterFxSound: sets the carrier's device to 100% and unmuted again, after the device changed its own level right
	// after the take-over (a Bluetooth headset sending its stored volume). UI/timer thread.
	void reassertCarrierFullVolume();
	bool isDevicePresent(const std::wstring& device_id) const;
	// BetterFxSound: present and usable as an output (2 channels or more), the controller's own test.
	bool isOutputPresent(const std::wstring& device_id) const;
	bool isProcessing() const;

	int sndDeviceHandleToSoundDevices(bool active_devices = true);
	static void onDeviceChange();

	// BetterFxSound: counts the device notifications (onDeviceChange()). A reinit that fails parks the engine; it is
	// woken by a notification that came after the counter was read before that reinit, so one that arrived during the
	// reinit is not lost.
	static std::atomic<LONG> s_device_change_count_;
	LONG parked_change_count_ = 0;			// Counter read before the reinit that parked the engine
	// BetterFxSound: a reinit that failed with real devices present (device in use by an exclusive-mode app, not ready
	// after a connect or a resume, format being changed, virtual device not readable yet) is retried after a growing
	// delay, as no notification may follow. UI/timer thread only.
	bool park_retry_timed_ = false;
	DWORD park_retry_delay_ms_ = 0;			// 0 until a reinit fails, then 500 ms doubling up to the cap; reset by a success
	DWORD park_start_tick_ = 0;
	static constexpr DWORD kParkRetryFirstMs = 500;
	static constexpr DWORD kParkRetryMaxMs = 5000;
	// BetterFxSound: the FxSound virtual device is listed but its name could not be read for a second (its property
	// store not ready yet, after a resume): no notification may follow either. Each attempt blocks for that second,
	// so these retries are capped at 30 s.
	static constexpr DWORD kParkRetryVirtualMaxMs = 30000;
	// BetterFxSound: consecutive reinits that failed on the carrier itself (sndDevicesHdlType::playbackSetupFailed),
	// and which carrier. After kCarrierFailuresToReport of them (about 3.5 s with the delays above) the carrier is
	// reported as failed (getFailedCarrierId()), so the controller elects another enabled output. UI/timer thread only.
	std::wstring carrier_failure_id_;
	int carrier_failure_count_ = 0;
	static constexpr int kCarrierFailuresToReport = 4;

	PT_HANDLE *hp_sndDevices_;
	static sndDevicesHdlType s_sndDevices_;

	/* Processing Thread Info */
	HANDLE hProcessingThread_;
	DWORD ProcessingThreadID_;
	int i_kill_processing_thread_; /* Flag set from the outside telling processing thread to end */
	wchar_t wcp_playback_device_guid_[PT_MAX_GENERIC_STRLEN];
	bool b_no_valid_snd_device_dialog_shown_; /* Flag stating whether we have shown the user a message to select a valid snd device.  We only want it shown once per session. */
	int debug_;
	std::vector<SoundDevice> sound_devices_;
	bool mute_ = false;
	DfxDsp *p_dfx_dsp_;
	static AudioPassthruCallback *s_callback_;

	// BetterFxSound: requested outputs (UI/timer threads only, guarded by config_mutex_, never
	// touched by the audio threads).
	std::mutex config_mutex_;
	// BetterFxSound: held by applyAdditionalOutputs() from building the router's list until the router has it, which it
	// gets after config_mutex_ is released; taken before config_mutex_. UI/timer threads only.
	std::mutex router_mutex_;
	std::vector<OutputConfig> outputs_;			// As passed to setOutputs(), carrier included
	std::wstring pending_carrier_id_;			// Asked for by setAsPlaybackDevice(), until the next reinit
	bool apply_pending_ = false;				// A change arrived during a measurement, apply it afterwards
	std::wstring carrier_full_volume_id_;		// Carrier whose device was set to 100% and unmuted since the last reinit
	// BetterFxSound: a carrier that starts playing is held silent until its device is at 100% (applyAdditionalOutputs()),
	// so it does not play at its own level first and then jump. Which carrier, until then.
	std::wstring deferred_carrier_enable_id_;
	// BetterFxSound: setting the carrier to 100% failed (endpoint not ready yet after a connect or a resume). It is tried
	// again from processTimer() after kCarrierVolumeRetryMs, kCarrierVolumeRetries times at most per reinit.
	bool carrier_volume_retry_ = false;
	DWORD carrier_volume_retry_tick_ = 0;
	int carrier_volume_failures_ = 0;
	static constexpr int kCarrierVolumeRetries = 5;
	static constexpr DWORD kCarrierVolumeRetryMs = 1000;
	// BetterFxSound: times the carrier was set to 100% again after its device changed its own level right after the
	// take-over (sndDevicesHdlType::playbackFullVolumeReassert), since that take-over. UI/timer thread only.
	int carrier_reasserts_ = 0;
	static constexpr int kCarrierReasserts = 3;
	// BetterFxSound: handBackUnusedDevices() left an owed device because a hand-back of it was already running elsewhere
	// (it does not wait for one): processTimer() runs the sweep again after kHandBackRetryMs. UI/timer thread only.
	bool hand_back_retry_ = false;
	DWORD hand_back_retry_tick_ = 0;
	static constexpr DWORD kHandBackRetryMs = 1000;
	static constexpr DWORD kCalibrationExitWaitMs = 5000;	// BetterFxSound: ~AudioPassthruPrivate() waits this long for a delay measurement
	bool shut_down_ = false;					// BetterFxSound: shutdown() ran
	bool shutdown_safe_ = true;					// BetterFxSound: and the processing thread is known to have ended
	std::atomic<bool> dsp_released_{ false };	// BetterFxSound: shutdown() gave up on the processing thread: it must no longer use p_dfx_dsp_
	bool handed_back_ = false;					// restoreDefaultPlaybackDevice() called, until processing starts again
	bool parked_ = false;						// The last reinit failed, until one succeeds or power off
	std::wstring failed_carrier_id_;			// Carrier reported as failed (getFailedCarrierId()), until a reinit succeeds
	std::wstring routed_carrier_id_;			// BetterFxSound: carrier effective_outputs_ was built around (getEngineStatus())

	MultiOutputRouter multi_output_;

	// Carrier output, the device on the original low latency path. Written by the UI/timer threads,
	// read by the processing thread. Silent until setOutputs() enables it, like an empty set of outputs.
	std::atomic<bool> carrier_enabled_{ false };
	std::atomic<float> carrier_trim_{ 1.0f };
	std::atomic<int> main_channel_mode_{ 0 };	// OutputChannelMode of the carrier
	float carrier_gain_ = 0.0f;					// Processing thread only: current, ramped gain

	// Carrier delay line (processing thread only, except main_delay_ms_).
	static constexpr UINT32 kMainDelayFrames = 1u << 16;	// ~1.36 s at 48 kHz
	static constexpr UINT32 kMainDelayChannels = 8;
	std::atomic<int> main_delay_ms_{ 0 };
	int carrier_path_offset_ms_ = 0;			// BetterFxSound: added to main_delay_ms_, see MultiOutputRouter::alignPaths()
	std::vector<float> main_delay_buffer_;
	UINT32 main_delay_write_ = 0;
	UINT32 main_delay_format_ = 0;
	UINT32 main_delay_applied_frames_ = 0;		// Delay the line reads with (frames), follows main_delay_ms_ through a fade
	UINT32 main_delay_silent_frames_ = 0;		// BetterFxSound: frames written last in a row that are silent (at most kMainDelayFrames)
	float main_delay_fade_ = 1.0f;				// Gain of the line's output during that fade
	bool main_delay_switching_ = false;			// Fading out to move to a new delay

	// Delay measurement. While it runs only one output plays: -1 = carrier, k = additional output k.
	// solo_ is declared before calibration_ so it outlives the measurement thread that sets it.
	std::atomic<int> solo_{ DelayCalibration::kSoloNone };
	// Incremented each time a processing thread ends, so the capture, the carrier and the additional outputs' audio
	// were interrupted: a delay measurement that spans it is no longer valid. Declared before calibration_ too.
	std::atomic<unsigned int> stream_generation_{ 0 };
	std::vector<AdditionalOutputConfig> effective_outputs_;	// As passed to multi_output_ (guarded by config_mutex_)
	std::wstring calibration_carrier_id_;	// Carrier when the measurement started, a switch aborts it (guarded by config_mutex_)
	std::vector<std::wstring> calibration_unavailable_;	// BetterFxSound: outputs left out of it, device not opened (guarded by config_mutex_)
	// BetterFxSound: on the heap, so ~AudioPassthruPrivate() can leave it allocated under a measurement thread that a
	// device driver holds (DelayCalibration::shutdown()). Declared last, so it goes before what its functions use.
	std::unique_ptr<DelayCalibration> calibration_{ new DelayCalibration() };
};

