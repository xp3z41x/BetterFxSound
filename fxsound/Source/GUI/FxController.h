/*
FxSound
Copyright (C) 2025  FxSound LLC

Contributors:
	www.theremino.com (2025)

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

#include <JuceHeader.h>
#include <functional>
#include <atomic>
#include <map>
#include "FxModel.h"
#include "FxTheme.h"
#include "FxAudioControls.h"
#include "../Source/Utils/Settings/Settings.h"
#include "../Source/Utils/Settings/DeviceConfig.h"
#include "AudioPassthru.h"
#include "DfxDsp.h"
#include <wtsapi32.h>

using namespace FxSound;

class FxMainWindow;
class FxWindow;
class FxSystemTrayView;

enum ViewType { Lite = 1, Pro = 2 };

class FxController : public Timer, public DeletedAtShutdown, private AudioPassthruCallback
{
public:
    static constexpr int NUM_SPECTRUM_BANDS = 10;
	static constexpr int DEFAULT_NUM_EQ_BANDS = 10;
	static constexpr float DEFAULT_NORMALIZATION = 0.0f;
	static constexpr float DEFAULT_VOLUME_LEVELING = 0.0f;
	static constexpr float DEFAULT_BALANCE = 0.0f;
	static constexpr float DEFAULT_FILTER_Q = 1.0f;
	static constexpr float DEFAULT_MASTER_GAIN = 0.0f;
	static constexpr float MIN_GAIN = -12.0f;
	static constexpr float MAX_GAIN = 12.0f;
	static constexpr char HK_CMD_ON_OFF[] = "cmd_on_off";
	static constexpr char HK_CMD_OPEN_CLOSE[] = "cmd_open_close";
	static constexpr char HK_CMD_NEXT_PRESET[] = "cmd_next_preset";
	static constexpr char HK_CMD_PREVIOUS_PRESET[] = "cmd_previous_preset";
	static constexpr char HK_CMD_NEXT_OUTPUT[] = "cmd_change_output";
	
	static FxController& getInstance()
	{
		static FxController* controller = new FxController();
		return *controller;
	}
	~FxController();

	FxController(const FxController&) = delete;
	void operator=(FxController&) = delete;

    void initConfig(const String& commandline);
	void applyConfig(const String& commandline);
	void init(FxMainWindow* main_window, FxSystemTrayView* system_tray_view, AudioPassthru* audio_passthru);
	// BetterFxSound: called by Main::shutdown() before the engine is destroyed. Hands the engine the last
	// outputs, stops the timer, unregisters the device change callback and forgets the engine, so nothing
	// queued or delivered later (device notifications, window messages) reaches a freed engine.
	void detachAudioPassthru();
	void initPresets();
	void printStatus();
	static File getStatusFile();

	void showView();
	void switchView();
	ViewType getCurrentView();
	void hideMainWindow();
	void showMainWindow();
    bool isMainWindowVisible();
	void setMenuClicked(bool clicked);
	FxWindow* getMainWindow();
	Point<int> getSystemTrayWindowPosition(int width, int height);
	bool exit();

	void setPowerState(bool power_state);
    bool setPreset(const String& preset_name, bool notify = true);
	bool setPreset(int preset_index, bool notify=true);

    bool isPlaybackDeviceAvailable();
	void checkDeviceChanges();

	void autoSaveModifiedPreset();
	void savePreset(const String& preset_name=L"");
	void renamePreset(const String& new_name);
	void deletePreset();
	void undoPreset();
	void resetPresets();
    void exportPresets(const Array< FxModel::Preset>& presets, std::function<void (bool)> onComplete);
    bool importPresets(const Array<File>& preset_files, StringArray& imported_presets, StringArray& skipped_presets);

	float getEffectValue(FxEffects::EffectType effect);
	void setEffectValue(FxEffects::EffectType effect, float value);

	int getNumEqBands();
	void setNumEqBands(int num_bands);
	float getVolumeLeveling();
	void setVolumeLeveling(float gain_db);
	void setBalance(float gain_db);
	float getBalance();
	void setMasterGain(float gain_db);
	float getMasterGain();
	void setFilterQ(float q_multiplier);
	float getFilterQ();

    bool isAudioProcessing();
	float getEqBandFrequency(int band_num);
    void setEqBandFrequency(int band_num, float freq);
    void getEqBandFrequencyRange(int band_num, float* min_freq, float* max_freq);
    bool getDefaultEqBandFrequency(int band_num, float* freq);
	float getEqBandBoostCut(int band_num);
	void setEqBandBoostCut(int band_num, float boost);
    void getSpectrumBandValues(Array<float>& band_values);

	void enableHotkeys(bool enable);
	bool getHotkey(String cmdKey, int& mod, int& vk);
	bool setHotkey(const String& command, int new_mod, int new_vk);
	bool isValidHotkey(int mod, int new_vk);

	juce::Array<DeviceConfig> getDeviceConfigs();
    void saveDeviceConfigs(const juce::Array<DeviceConfig>& device_configs);
	// BetterFxSound: a device row (Output preferences) is matched by endpoint id; by name only for a row with no id.
	bool isOutputDeviceConnected(const String& output_device_id, const String& output_device_name);
	bool isOutputDevicePresent(const String& output_device_id, const String& output_device_name);
	SoundDevice getPreferredOutput();
	int compareOutputDevicePriority(const SoundDevice& output_device1, const SoundDevice& output_device2, const juce::Array<DeviceConfig>& device_configs);
	// Re-reads the device priority (Output preferences) and elects the carrier again.
	void refreshOutputList();
	// Name of the only enabled output, or empty when zero or several outputs are enabled. A device's
	// preset (Output preferences) belongs to the outputs only while that device is the one output on.
	const String& getOutputName();
	// Endpoint id of the only enabled output, or empty, as getOutputName().
	String getOutputId();
    bool isNewOutputPrioritized();
    void setNewOutputPrioritized(bool prioritize_new_devices);

	// BetterFxSound outputs rework (docs/OUTPUTS_REWORK.md): one flat list of outputs, no "main"
	// output from the UI's point of view. Every output can be enabled or disabled; zero enabled
	// outputs means silence. These are the only output APIs the UI should use.
	struct OutputState {
		String id;			// Endpoint ID string (SoundDevice::pwszID)
		String name;		// Friendly name
		bool enabled = false;
		bool present = true;	// False for an enabled output whose device is not connected now
		// BetterFxSound: enabled and connected, but the audio engine cannot open its device (held by an exclusive-mode
		// app, a driver failing): it does not play. The engine keeps retrying it.
		bool failed = false;
		int volume_percent = 100;	// 0 to 100, fine trim on top of the Windows master volume
		int delay_ms = 0;			// 0 to 500
		OutputChannelMode channel_mode = OutputChannelMode::Both;
	};
	std::vector<OutputState> getOutputs();
	void setOutputEnabled(const String& id, bool enabled);
	void setOutputVolume(const String& id, int volume_percent);
	void setOutputDelay(const String& id, int delay_ms);
	void setOutputChannelMode(const String& id, OutputChannelMode channel_mode);
	int getEnabledOutputCount();
	// Enables the given present output and disables every other one (CLI --output, "next output" hotkey).
	// Returns false if no present output has this id.
	bool enableOnlyOutput(const String& id);

	// BetterFxSound: automatic delay sync with the microphone.
	bool startDelayCalibration();
	void cancelDelayCalibration();
	int getDelayCalibrationState(int* progress_percent); // 0 idle, 1 running, 2 done, 3 failed
	String getDelayCalibrationError();
	// BetterFxSound: why the running measurement was stopped by FxSound itself (turned off, sleep), empty if it was
	// not. The engine then reports it as cancelled (state 0).
	String getDelayCalibrationInterruption();
	// Turns the measured latencies into delays, saves and applies them. Returns a summary for the user.
	String applyDelayCalibrationResults();

	FxThemeMode getThemeMode();
	void setThemeMode(FxThemeMode mode);

	bool isAlwaysOnTop();
	void setAlwaysOnTop(bool always_on_top);

	bool isLaunchOnStartup();
	void setLaunchOnStartup(bool launch_on_startup);

    bool isHelpTooltipsHidden();
    void setHelpTooltipsHidden(bool status);

	bool isNotificationsHidden();
	void setNotificationsHidden(bool status);

    String getLanguage() const;
    void setLanguage(String language_code);
	int getMaxUserPresets() const;

	bool getAutoUpdates();
	void setAutoUpdates(bool enable);
	void checkUpdates();

	void saveWindowPosition(int x, int y);
	void getWindowPosition(int& x, int& y);

    void setRenderingEngine(juce::Component& component);

	// Saved preference, applied on the next start.
	bool isGpuRenderingEnabled();
	void setGpuRenderingEnabled(bool enable);
	// Engine this process is actually running with.
	bool isGpuRenderingActive() const;
	static bool canSupportGpuRendering();

	// Starts a fresh instance after a short delay, so it runs once this process has exited
	// and released the single instance lock. Used for crash recovery and user restarts.
	static bool launchNewInstance();
	// Saves state, launches a new instance and quits this one.
	void restartApplication();
	// BetterFxSound: writes pending settings changes to disk now (on quit, restart and crash relaunch).
	void flushSettings() { settings_.flush(); }

	void logMessage(const String& message)
	{
		file_logger_->logMessage(message);
	}

	FxSound::Settings& getSettings() { return settings_; }

private:
	class MessageWindow
	{
	public:
		MessageWindow(const WCHAR* const wnd_name, WNDPROC wnd_proc)
		{
			String class_name("FXSOUND_");
			class_name << String::toHexString(Time::getHighResolutionTicks());

			HMODULE h_module = (HMODULE)Process::getCurrentModuleInstanceHandle();

			WNDCLASSEXW wc = { 0 };
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = wnd_proc;
			wc.hInstance = h_module;
			wc.lpszClassName = class_name.toWideCharPointer();

			atom_ = ::RegisterClassExW(&wc);
			jassert(atom_ != 0);

			hwnd_ = ::CreateWindowW(getClassNameFromAtom(), wnd_name,
				0, 0, 0, 0, 0, 0, 0, h_module, 0);
			jassert(hwnd_ != 0);
		}

		~MessageWindow()
		{
			DestroyWindow(hwnd_);
			UnregisterClassW(getClassNameFromAtom(), 0);
		}

		inline HWND getHandle() const noexcept { return hwnd_; }

	private:
		ATOM atom_;
		HWND hwnd_;

		LPCWSTR getClassNameFromAtom() noexcept { return (LPCWSTR)(pointer_sized_uint)atom_; }
	};

	static constexpr UINT CMD_ON_OFF = 1001;
	static constexpr UINT CMD_OPEN_CLOSE = 1002;
	static constexpr UINT CMD_NEXT_PRESET = 1003;
	static constexpr UINT CMD_PREVIOUS_PRESET = 1004;
	static constexpr UINT CMD_NEXT_OUTPUT = 1005;

	FxController();

	static LRESULT CALLBACK eventCallback(HWND hwnd, const UINT message, const WPARAM w_param, const LPARAM l_param);
	void timerCallback() override;
	void onSoundDeviceChange(bool processing) override;
	void handleSoundDeviceChange(bool processing);
	void onSystemSuspend();
	void onSystemResume();
	// BetterFxSound: the rest of init() once the FxSound virtual device is present, and the bounded wait for it.
	void completeInit();
	void waitForVirtualDevice(uint32 wait_start_ms);
	
    void initOutputs(std::vector<SoundDevice>& sound_devices);
	void updateOutputs(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_ids);
	void clearOutputRestore();
    void selectProcessingOutput(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_ids);
	void syncPlaybackAvailability();
	void syncOutputsWhileOff(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_ids);
	StringArray findNewDeviceIds(const std::vector<SoundDevice>& sound_devices);

	void sortByDeviceConfigPriority(std::vector<SoundDevice>& devices);

	// BetterFxSound outputs rework: one entry per output that is enabled or has been configured.
	// Persisted as the "outputs" settings key. Guarded by lock_.
	struct OutputEntry {
		String id;
		String name;
		bool enabled = false;
		int delay_ms = 0;
		int volume_percent = 100;
		OutputChannelMode channel_mode = OutputChannelMode::Both;
	};
	void loadOutputSettings(const std::vector<SoundDevice>& sound_devices);
	void saveOutputSettings();
	OutputEntry* findOutputEntry(const String& id);
	OutputEntry& getOutputEntry(const String& id);
	const SoundDevice* findActiveOutput(const String& id) const;
	bool refreshActiveOutputs(const std::vector<SoundDevice>& sound_devices);
	bool migrateOutputEntries();
	bool isEndpointListed(const String& id) const;
	bool isRecentOutputArrival(const String& id) const;
	bool enableNewOutputs(const StringArray& new_device_ids);
	SoundDevice electCarrier(const String& keep_id);
	void applyCarrier(const String& keep_id);
	void requestCarrier(const SoundDevice& device);
	void pushOutputsToEngine();
	void flushOutputsToEngine();
	void notifyOutputsChanged();
	void enableNextOutput();
	bool enableOnlyOutput(const String& id, String* applied_preset);
	String getSoleEnabledOutputId();
	String applySoleOutputPreset(const String& previous_sole_id);
	void handleNoOutput();

	void powerOn(bool on);

	String getAutoSavePath() const;
    String getAutoSavePresetPath(const String& preset_name) const;
	void autoSavePreset(int preset_index);
	void deleteAutoSavedPreset(const String& preset_name);

	void registerHotkeys();
	void unregisterHotkeys();

    String FormatString(const String& format, const String& arg);

	typedef HPOWERNOTIFY(WINAPI* RegisterSuspendResumeNotificationFunc)(HANDLE, DWORD);
	typedef BOOL(WINAPI* UnregisterSuspendResumeNotificationFunc)(HPOWERNOTIFY);

	MessageWindow message_window_;
	bool hotkeys_registered_;
	HPOWERNOTIFY powerNotify_;
	UnregisterSuspendResumeNotificationFunc unregister_suspend_resume_notification_;

	FxMainWindow* main_window_;
	FxSystemTrayView* system_tray_view_;
	AudioPassthru* audio_passthru_;
	DfxDsp dfx_dsp_;
	FxSound::Settings settings_;
	uint32_t device_count_;
	std::unique_ptr<FileLogger> file_logger_;
	ViewType view_;
    String language_;
	bool dfx_enabled_;
	bool authenticated_;
    bool playback_device_available_;
	bool system_sleeping_;
	// BetterFxSound: restoreDefaultPlaybackDevice() done and processing not started since. Another hand-back
	// (a session event while off) would set the Windows default again over the user's own choice.
	// True from the start: a new process has not processed yet, so starting (or logging on) with FxSound off
	// does not make the device the engine last processed on the Windows default again over the user's choice.
	// powerOn(false) still hands back when the virtual device is the live Windows default (left by a crash).
	bool engine_handed_back_ = true;
	// BetterFxSound: at logon the virtual endpoint can come up after FxSound starts. init() waits this long for
	// it, checking every VIRTUAL_DEVICE_POLL_MS, before the device error.
	static constexpr uint32 VIRTUAL_DEVICE_WAIT_MS = 15000;
	static constexpr int VIRTUAL_DEVICE_POLL_MS = 500;
	String output_device_name_;		// See getOutputName()

	// BetterFxSound outputs rework (guarded by lock_).
	std::vector<OutputEntry> output_entries_;
	String carrier_id_;				// Carrier the engine uses, or will use once a requested switch is done
	String requested_carrier_id_;	// Asked for with setAsPlaybackDevice(), not confirmed by a reinit yet
	bool no_enabled_output_present_ = false;	// Logged once: every enabled output is disconnected, silence
	StringArray failed_carrier_ids_;	// Asked for but not taken by the engine; skipped until the devices change
	// BetterFxSound: a switch the engine did not take after a reinit is asked for again; only one missed
	// MAX_CARRIER_MISSES times in a row, with nothing else to explain it, goes to failed_carrier_ids_.
	static constexpr int MAX_CARRIER_MISSES = 2;
	String missed_carrier_id_;
	int missed_carrier_count_ = 0;
	// BetterFxSound: a requested switch with no reinit after CARRIER_REQUEST_TIMEOUT ticks (the engine's stop
	// request can be lost) is asked for again, up to MAX_CARRIER_REQUEST_RETRIES times, then given up on.
	static constexpr int CARRIER_REQUEST_TIMEOUT = 10;	// 10 ticks * 100 ms = 1 s
	// BetterFxSound: the additional outputs the engine could not open (AudioPassthru::getAdditionalOutputStatus()),
	// sorted, read by the timer every OUTPUT_STATUS_TICKS (getOutputs() reports them as failed).
	StringArray failed_output_ids_;
	int output_status_ticks_ = 0;
	static constexpr int OUTPUT_STATUS_TICKS = 10;	// 10 ticks * 100 ms = 1 s
	static constexpr int MAX_CARRIER_REQUEST_RETRIES = 3;
	int carrier_request_ticks_ = 0;
	int carrier_request_retries_ = 0;
	// BetterFxSound: when each present output appeared (Time::getMillisecondCounter()). Windows and drivers often
	// make an arriving device the default, and the engine's rules take a device just added: for
	// NEW_OUTPUT_SETTLE_MS such a carrier change is not taken as the user's choice.
	static constexpr uint32 NEW_OUTPUT_SETTLE_MS = 10000;
	std::map<String, uint32> output_arrival_ms_;
	// pushOutputsToEngine() stores the outputs here and flushOutputsToEngine() hands them to the engine with
	// lock_ released (setOutputs() may wait for removed outputs' render threads to stop, on the message thread).
	std::vector< ::OutputConfig> pending_engine_outputs_;
	bool engine_outputs_pending_ = false;
	// BetterFxSound: the delay each output had when the running (or last) delay measurement started, by id. Those are
	// the delays it was measured with: applyDelayCalibrationResults() removes them, whatever the delays are by then.
	std::map<String, int> calibration_delays_;
	// The carrier when the measurement started, and the delay the engine actually played it with: it delays the
	// carrier by at most MAIN_DELAY_FRAMES - 1 frames at the processing rate, less the room kept for the path offset
	// (about 141 ms at 192 kHz, 482 ms at 96 kHz), so a larger
	// delay is played shorter. calibration_carrier_max_delay_ms_ is the largest whole delay it plays in full.
	String calibration_carrier_id_;
	double calibration_carrier_delay_ms_ = 0.0;
	int calibration_carrier_max_delay_ms_ = 500;
	// Why FxSound itself stopped the measurement (see getDelayCalibrationInterruption()), empty if it did not.
	String calibration_interruption_;
	void interruptDelayCalibration(const String& reason);
	bool no_output_;				// Powered off because no real output device was present
	String startup_output_name_;	// --output given on the command line before init

	// HDMI outputs disappear when the PC goes to sleep. The device removal notification arrives
	// before the system suspend notification, so a removal can only be recognized as sleep-related
	// after the fact. After removal of the HDMI output in use, the failover is held for
	// HDMI_FAILOVER_INTERVAL timer ticks. The count restarts on suspend (the timer does not tick while
	// asleep) so HDMI also gets that time after wake. If HDMI is back in time it is reused, otherwise
	// it is treated as a physical disconnect and the preferred output is selected.
	// BetterFxSound: the muted hold only when no other enabled output is present. Otherwise the other outputs
	// keep playing: a carrier is elected among them at once, and the HDMI output, if back within the same
	// time, is the carrier again (hdmi_return_pending_, not muted).
	static constexpr int HDMI_FAILOVER_INTERVAL = 30;	// 30 ticks * 100 ms = 3 s
	String failover_output_id_;
	int hdmi_failover_counter_;
	bool hdmi_reconnect_pending_;	// HDMI output lost, waiting for it to reconnect
	bool hdmi_return_pending_ = false;	// HDMI output lost while another enabled output plays, it is the carrier again if back in time
	std::vector<SoundDevice> active_output_devices_;
	std::vector<SoundDevice> output_devices_;
	bool always_on_top_;
    bool hide_help_tooltips_;
	bool hide_notifications_;
	bool auto_updates_;
    bool gpu_render_;

	unsigned long audio_process_time_;
	int audio_process_on_counter_;
	int audio_process_off_counter_;
	bool audio_process_on_;
	std::time_t audio_process_start_time_;

	bool preset_dirty_;
	int auto_save_counter_;

	bool minimize_tip_;
	bool survey_tip_;
	int max_user_presets_;

	DWORD session_id_;

	CriticalSection lock_;
	CriticalSection save_lock_;

	// BetterFxSound: a device notification is queued for the message thread (onSoundDeviceChange). Set on
	// the IMMNotificationClient thread, so repeated notifications are handled once.
	std::atomic<bool> device_change_posted_ { false };
};
