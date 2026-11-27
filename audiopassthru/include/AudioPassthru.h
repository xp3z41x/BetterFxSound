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
/*
*  Public defines for the AudioPassthru class
*/

#ifndef _AUDIOPASSTHRU_H_
#define _AUDIOPASSTHRU_H_ 

#ifndef __ANDROID__
#include <Windows.h>
#endif //WIN32
#include <Mmdeviceapi.h>
#include <vector> 
#include <string>
#include "DfxDsp.h"

struct SoundDevice {
	IMMDevice *pAllDevices = NULL; // Object pointers for each device.
	bool isCaptureDevice = false;
	bool isPlaybackDevice = false;
	bool isTargetedRealPlaybackDevice = false;
	bool isRealDevice = false;
	bool isDFXDevice = false;
	bool isUserSelectedPlaybackDevice = false;
	bool isDefaultDevice = false;
    bool isActive = false;

	std::wstring pwszID; // For the GUID ID strings for each device, all devices combined.
	std::wstring pwszIDRealDevices; // For the GUID ID strings for each real playback device.
	WCHAR pwszIDPreviousRealDevices[512]; // To detect when a new devices is added.

	std::wstring deviceFriendlyName;  // Friendly name, ie "DFX Audio Enhancer 10.5", all devices.
	std::wstring deviceDescription;  // Descriptive name, ie "Speakers", all devices.
	std::wstring deviceFriendlyNameRealDevices;  // Friendly names, just the real devices.
	std::wstring deviceDescriptionRealDevices;  // Descriptive names, just the real devices.
	std::wstring deviceFormFactor;  // Form factor type string, eg "HDMI", "Speakers", etc.
	int deviceNumChannel; // Number of channels for all devices.
};

// BetterFxSound: an additional output device that plays the processed audio at the same
// time as the main output.
// Which part of the stereo image an additional output plays.
enum class OutputChannelMode {
	Both = 0,	// All channels, unchanged
	Left = 1,	// Front left on every channel of this output
	Right = 2,	// Front right on every channel of this output
	Mono = 3	// (Left + Right) / 2 on every channel of this output
};

struct AdditionalOutputConfig {
	std::wstring device_id;	// Endpoint ID string, same as SoundDevice::pwszID
	int delay_ms = 0;		// Extra delay for this output, 0 to 500 ms, used to line it up with the others
	OutputChannelMode channel_mode = OutputChannelMode::Both;
	int volume_percent = 100;	// Volume of this output, 0 to 100
};

// BetterFxSound: result of the microphone delay measurement for one output.
struct DelayCalibrationResult {
	std::wstring device_id;
	bool measured = false;		// False if the test sound was not heard clearly from this output
	double latency_ms = 0.0;	// Time from the test sound until it was heard, including the output's current delay
	double confidence = 0.0;	// Correlation peak / RMS, higher is better
};

class AudioPassthruCallback
{
public:
	virtual void onSoundDeviceChange(bool processing) = 0;
};

class AudioPassthruPrivate;
class AudioPassthru
{
public:
	AudioPassthru();
	~AudioPassthru();
	int init();
	void mute(bool mute);
	std::vector<SoundDevice> getSoundDevices(bool active_devices = true);
	int setBufferLength(int i_buffer_length_msecs);
	int processTimer();
	void setDspProcessingModule(DfxDsp* pDspProcessingModule);
	void setAsPlaybackDevice(const SoundDevice sound_device);
	void registerCallback(AudioPassthruCallback *callback);
    bool isPlaybackDeviceAvailable();
	bool checkDeviceChanges();
	void restoreDefaultPlaybackDevice();
	// BetterFxSound: sets the additional output devices. The main output and the FxSound
	// virtual device are ignored if they appear in the list.
	void setAdditionalOutputs(const std::vector<AdditionalOutputConfig>& outputs);
	// BetterFxSound: channel mode of the main output (Both keeps the original behaviour).
	void setMainOutputChannelMode(OutputChannelMode mode);
	// BetterFxSound: extra delay of the main output, 0 to 500 ms.
	void setMainOutputDelay(int delay_ms);

	// BetterFxSound: measures the delay of the main and the additional outputs with the
	// default microphone. Runs in the background; poll getDelayCalibrationState().
	// State: 0 idle, 1 running, 2 done, 3 failed.
	bool startDelayCalibration();
	void cancelDelayCalibration();
	int getDelayCalibrationState(int* progress_percent);
	std::vector<DelayCalibrationResult> getDelayCalibrationResults();
	std::wstring getDelayCalibrationError();

private:
	AudioPassthruPrivate *data_;
};

#endif
