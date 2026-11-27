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
	int init();
	void mute(bool mute);
	std::vector<SoundDevice> getSoundDevices(bool active_devices = true);
	int killProcessingThread(int *ip_timed_out);
	int setBufferLength(int i_buffer_length_msecs);
	int processTimer();
	void setDspProcessingModule(DfxDsp* p_dfx_dsp);
	static DWORD WINAPI processingThread(LPVOID lpParam);
	DWORD threadWorker(void); // Needs to be public to be called from static thread starter function
	int setTargetedRealPlaybackDevice(const std::wstring sound_device_guid);
	void registerCallback(AudioPassthruCallback *callback);
    bool isPlaybackDeviceAvailable();
	bool checkDeviceChanges();
	void restoreDefaultPlaybackDevice();
	void setOutputs(const std::vector<OutputConfig>& outputs);
	bool startDelayCalibration();
	void cancelDelayCalibration();
	int getDelayCalibrationState(int* progress_percent);
	std::vector<DelayCalibrationResult> getDelayCalibrationResults();
	std::wstring getDelayCalibrationError();

private:
	void applyMainDelay(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate);
	void applyCarrierGain(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate);

	// Recomputes the carrier settings and the additional outputs from the requested outputs and the
	// current carrier. Called after every change and after every reinit (the carrier may have changed).
	void applyAdditionalOutputs();
	bool isDevicePresent(const std::wstring& device_id) const;
	bool isProcessing() const;

	int sndDeviceHandleToSoundDevices(bool active_devices = true);
	static void onDeviceChange();

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
	bool mute_;
	DfxDsp *p_dfx_dsp_;
	static AudioPassthruCallback *s_callback_;

	// BetterFxSound: requested outputs (UI/timer threads only, guarded by config_mutex_, never
	// touched by the audio threads).
	std::mutex config_mutex_;
	std::vector<OutputConfig> outputs_;			// As passed to setOutputs(), carrier included
	std::wstring pending_carrier_id_;			// Asked for by setAsPlaybackDevice(), until the next reinit
	bool apply_pending_ = false;				// A change arrived during a measurement, apply it afterwards
	std::wstring carrier_full_volume_id_;		// Carrier whose device was set to 100% and unmuted since the last reinit
	bool handed_back_ = false;					// restoreDefaultPlaybackDevice() called, until processing starts again

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
	std::vector<float> main_delay_buffer_;
	UINT32 main_delay_write_ = 0;
	UINT32 main_delay_format_ = 0;

	// Delay measurement. While it runs only one output plays: -1 = carrier, k = additional output k.
	// solo_ is declared before calibration_ so it outlives the measurement thread that sets it.
	std::atomic<int> solo_{ DelayCalibration::kSoloNone };
	std::vector<AdditionalOutputConfig> effective_outputs_;	// As passed to multi_output_ (guarded by config_mutex_)
	DelayCalibration calibration_;
};

