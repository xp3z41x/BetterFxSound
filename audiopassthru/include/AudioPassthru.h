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

// BetterFxSound: which part of the stereo image an output plays.
enum class OutputChannelMode {
	Both = 0,	// All channels, unchanged
	Left = 1,	// Front left on every channel of this output
	Right = 2,	// Front right on every channel of this output
	Mono = 3	// (Left + Right) / 2 on every channel of this output
};

// BetterFxSound, engine internal: one output played through MultiOutputRouter, that is every
// enabled output other than the carrier. The engine builds these from the setOutputs() list.
struct AdditionalOutputConfig {
	std::wstring device_id;	// Endpoint ID string, same as SoundDevice::pwszID
	int delay_ms = 0;		// Extra delay for this output, 0 to 500 ms, used to line it up with the others
	OutputChannelMode channel_mode = OutputChannelMode::Both;
	int volume_percent = 100;	// Volume of this output, 0 to 100
};

// BetterFxSound: the state of one additional output (AudioPassthru::getAdditionalOutputStatus()).
struct AdditionalOutputStatus {
	std::wstring device_id;
	bool open = false;		// Its stream is open (it opens when audio first comes)
	bool failed = false;	// Not open, and its device could not be opened or stopped working; the engine keeps retrying it
	long last_error = 0;	// HRESULT of the last failed open or render, 0 (S_OK) once a stream opened
};

// BetterFxSound: one output in the set passed to AudioPassthru::setOutputs(), carrier included.
// The level actually played is master_volume * (master_mute ? 0 : 1) * trim(volume_percent), where
// the master volume and mute are the FxSound virtual device's (the Windows volume).
struct OutputConfig {
	std::wstring device_id;      // SoundDevice::pwszID
	bool enabled = false;
	int delay_ms = 0;            // 0..500
	int volume_percent = 100;    // 0..100, trim on top of the master volume
	OutputChannelMode channel_mode = OutputChannelMode::Both;
};

// BetterFxSound: result of the microphone delay measurement for one output.
struct DelayCalibrationResult {
	std::wstring device_id;
	bool measured = false;		// False if the test sound was not heard clearly from this output
	bool unavailable = false;	// BetterFxSound: not measured because its device could not be opened (measured is false)
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
	// BetterFxSound: true while a carrier switch asked for with setAsPlaybackDevice() waits for the
	// reinit that takes it. Until then the carrier reported by getSoundDevices() is still the old one.
	bool isCarrierSwitchPending();
	// BetterFxSound: the carrier the engine could not open at its last reinits (held by an exclusive-mode app, its
	// format not supported or not readable), while it keeps retrying it. Empty when none: once a reinit succeeds, at
	// power off, or before a few attempts failed. The controller elects another enabled output instead.
	std::wstring getFailedCarrierId();
	void restoreDefaultPlaybackDevice();
	// BetterFxSound: the whole desired set of outputs, carrier included. The carrier is still
	// chosen with setAsPlaybackDevice(). If the carrier is missing from the list or is disabled,
	// the carrier path plays silence; no reinit is needed. Every other enabled entry whose device
	// is present plays through the additional outputs. The FxSound virtual device is always
	// ignored. Zero enabled outputs means silence; processing keeps running.
	// When the engine starts using an enabled output's device (stream opened, reinit, power on,
	// output enabled), it saves that device's own level and mute, then sets it to 100% and unmuted,
	// once, so it does not stack on the app's gain. When it stops using the device (output removed,
	// carrier switch, power off, exit), it puts the saved level and mute back. Nothing is kept
	// across a crash. The FxSound virtual device's volume is never changed.
	void setOutputs(const std::vector<OutputConfig>& outputs);
	// BetterFxSound: the state of every output playing through the additional outputs (every enabled, present output
	// other than the carrier), so an output whose device cannot be opened (held by an exclusive-mode app, a driver
	// failing after a resume) can be shown as such. The engine keeps retrying it meanwhile.
	std::vector<AdditionalOutputStatus> getAdditionalOutputStatus();

	// BetterFxSound: measures the delay of every enabled output that is present (the carrier
	// only if it is enabled) with the default microphone. Runs in the background; poll
	// getDelayCalibrationState(). State: 0 idle (also after a cancel), 1 running, 2 done, 3 failed.
	// Results have one entry per measured output, keyed by device_id, in no particular order.
	// BetterFxSound: an output whose device could not be opened is not measured; it has an entry with unavailable set.
	// With nothing to measure (no output turned on, or none of them connected), or when the test
	// sound could not be heard (FxSound not playing, Windows volume muted) the measurement fails at
	// once and getDelayCalibrationError() says why. A carrier switch while it runs makes it fail too.
	bool startDelayCalibration();
	void cancelDelayCalibration();
	int getDelayCalibrationState(int* progress_percent);
	std::vector<DelayCalibrationResult> getDelayCalibrationResults();
	std::wstring getDelayCalibrationError();

private:
	AudioPassthruPrivate *data_;
};

#endif
