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

//#include "stdafx.h"
#include "u_AudioPassthru.h"
#include "sndDevices.h"
#include "MasterVolume.h"
#include "OwedVolume.h"
#include <algorithm>
#include <cstring>

#define DFXG_SND_SERVER_KILL_THREAD_TIMEOUT_MSECS		  3000
#define DFXG_SND_SERVER_KILL_THREAD_WAIT_PER_LOOP_MSECS   50
#define DFXG_TRUNCATED_DRIVER_TEXT_LENGTH			      50

// See https://stackoverflow.com/questions/6472948/converting-member-function-pointer-to-timerproc
//std::map<UINT_PTR, AudioPassthru*> AudioPassthru::m_AudioPassthruClassMap;  //definition

// According to Paul's note, sndDevice handle must be declared statically.
// He didn't explain fully why (something to do with device event callbacks) 
// but without doing this I was getting access violiation excpeiotn in MMDevApi.dll so 
// there is some truth to it.
sndDevicesHdlType AudioPassthruPrivate::s_sndDevices_;
AudioPassthruCallback* AudioPassthruPrivate::s_callback_ = nullptr;

AudioPassthruPrivate::AudioPassthruPrivate()
{
	hp_sndDevices_ = (PT_HANDLE *)&(s_sndDevices_);
	main_delay_buffer_.assign((size_t)kMainDelayFrames * kMainDelayChannels, 0.0f);
	//hp_sndDevices_ = hp_sndDevices;
	hProcessingThread_ = NULL;
	ProcessingThreadID_ = (DWORD)0;
	i_kill_processing_thread_ = IS_FALSE;
	swprintf(wcp_playback_device_guid_, PT_MAX_GENERIC_STRLEN, L"");
	b_no_valid_snd_device_dialog_shown_ = false;
	debug_ = IS_TRUE;
}

AudioPassthruPrivate::~AudioPassthruPrivate()
{
	int i_result_flag;
	int i_timed_out;

	if (hp_sndDevices_ == NULL)
		return;

	/* BetterFxSound: stop a running delay measurement and close the additional outputs first */
	calibration_.cancel();
	multi_output_.stopAll();

	/* If the processing thread is running, kill it */
	if (killProcessingThread(&i_timed_out) != OKAY)
		return;

	/* If the killing of processing attempt timed out, then we can't do anything else */
	if (i_timed_out)
		return;

	/* Disable volume callbacks before switching devices to prevent the DFX driver's
	   volume state reset from propagating to the real speakers during shutdown. */
	s_sndDevices_.ignoreVolumeCallbacks = TRUE;

	/* Change the default soundcard to not be the DFX virtual one but instead the proper real one */
	if (sndDevicesRestoreDefaultDevice(hp_sndDevices_, &i_result_flag) != OKAY)
		return;

	/* BetterFxSound: anything still owed to a present device goes back on it at exit */
	handBackUnusedDevices(std::vector<std::wstring>());

	/*
	* Disable the virtual soundcard
	* NOTE: FOR NOW WE DON'T DO THE DISABLE BECAUSE THIS CAN CAUSE PROBLEMS
	*/
	/*
	if (sndDevicesSetDeviceEnabledStatus(cast_handle->snd_server.hp_sndDevices,
	SND_DEVICES_VIRTUAL_PLAYBACK_DFX, FALSE,  &i_result_flag) != OKAY)
	return(NOT_OKAY);
	*/

	/* Free the data allocated inside the sndDevices_hdl (NOTE: Does not free up structure) */
	if (sndDevicesFree(hp_sndDevices_) != OKAY)
		return;

	hp_sndDevices_ = NULL;
}

int AudioPassthruPrivate::init()
{
	int status_flag;
	
	/* Initialize the handle */
	if (sndDevicesInit(this->hp_sndDevices_, NULL, SND_DEVICES_INIT_FOR_PROCESSING, debug_, &status_flag) != OKAY)
		return(NOT_OKAY);

	s_sndDevices_.deviceChangeCallback = onDeviceChange;

	if (status_flag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
	{
		// Its possible that the audio system was in transition and returned a not ready status that caused us to arrive here.
		// The timer will force a call to sndDevicesReInit on first play attempt so there is no need to call it here, this section should only deal with fatal errors.
		if (debug_)
		{
			//swprintf(cast_handle->wcp_msg1, 2048, L"dfxg_SndServerInit(): sndDevicesInit failed, status_flag = %d", status_flag);
			//(cast_handle->slout1)->Message_Wide(FIRST_LINE, cast_handle->wcp_msg1);
		}

		// Fatal errors.
		if (status_flag == SND_DEVICES_MP3_DLL_LOAD_FAILED)
		{
			// We were unable to load the MP3 decoding dll (only applies to MR), in addition to putting trace statement here we may want to add message box to user here.
		}
	}

	/* Initialize code for fixing the case of HDMI monitors being plugged in */
	//if (dfxg_SndServerHdmiFixInit(hp_dfxg) != OKAY)
		//return(NOT_OKAY);

	//pDspProcessingModule_->init();

	return(OKAY);
}

void AudioPassthruPrivate::setDspProcessingModule(DfxDsp* p_dfx_dsp)
{
	p_dfx_dsp_ = p_dfx_dsp;
}


std::vector<SoundDevice> AudioPassthruPrivate::getSoundDevices(bool active_devices)
{
	if (checkDeviceChanges())
	{
		sndDevices_GetAll(hp_sndDevices_, &(s_sndDevices_.totalNumDevices));
	}

	sndDeviceHandleToSoundDevices(active_devices);
	return sound_devices_;
}

int AudioPassthruPrivate::sndDeviceHandleToSoundDevices(bool active_devices)
{
	int i_resultFlag;
	// BetterFxSound: start empty. The user selected device is never read, and was compared uninitialized.
	wchar_t wcp_user_seleted_playback_device_guid[PT_MAX_GENERIC_STRLEN] = { 0 };
	wchar_t wcp_targeted_real_playback_device_guid[PT_MAX_GENERIC_STRLEN] = { 0 };
	wchar_t wcp_capture_device_guid[PT_MAX_GENERIC_STRLEN] = { 0 };
	wchar_t wcp_dfx_device_guid[PT_MAX_GENERIC_STRLEN] = { 0 };
	wchar_t wcp_default_device_guid[PT_MAX_GENERIC_STRLEN] = { 0 };

	// Cast the handle so we can access its data.
	struct sndDevicesHdlType *cast_handle;
	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices_;

	// Clear SoundDevices
	sound_devices_.clear();

	// Get the guid of the currently selected real playback device
	if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_TARGETED_REAL_PLAYBACK, wcp_targeted_real_playback_device_guid, &i_resultFlag) != OKAY ||
		sndDevicesGetID(hp_sndDevices_, SND_DEVICES_CAPTURE, wcp_capture_device_guid, &i_resultFlag) != OKAY ||
		sndDevicesGetID(hp_sndDevices_, SND_DEVICES_VIRTUAL_PLAYBACK_DFX, wcp_dfx_device_guid, &i_resultFlag) != OKAY ||
		sndDevicesGetID(hp_sndDevices_, SND_DEVICES_DEFAULT, wcp_default_device_guid, &i_resultFlag) != OKAY)
	{
		return(NOT_OKAY);
	}

	for (int index = 0; index < cast_handle->totalNumDevices; index++) 
	{
		if (cast_handle->pwszID[index][0] == L'\0' || cast_handle->deviceFriendlyName[index] == NULL)
		{
			continue;
		}

		if (active_devices && cast_handle->deviceState[index] != DEVICE_STATE_ACTIVE)
		{
			continue;
		}

		SoundDevice sound_device;
		sound_device.pwszID = std::wstring(cast_handle->pwszID[index]);
		sound_device.deviceFriendlyName = std::wstring(cast_handle->deviceFriendlyName[index]);
		sound_device.deviceDescription = std::wstring(cast_handle->deviceDescription[index] != NULL ? cast_handle->deviceDescription[index] : L"");
		sound_device.deviceNumChannel = cast_handle->deviceNumChannel[index];
		sound_device.deviceFormFactor = std::wstring(cast_handle->deviceFormFactor[index]);
		if (cast_handle->deviceState[index] == DEVICE_STATE_ACTIVE)
		{
			sound_device.isActive = true;
		}
		else
		{
			sound_device.isActive = false;
        }

		// Skip mono devices if SND_DEVICES_MONO_BUG_SKIP_MONO_DEVICES is IS_TRUE
		if (SND_DEVICES_MONO_BUG_SKIP_MONO_DEVICES && sound_device.deviceNumChannel == 1)
		{
			continue;
		}

		// Figure out if this is a real device or not
		sound_device.isRealDevice = false;
		for (int index2 = 0; index2 < cast_handle->numRealDevices; index2++) 
		{
			if (sound_device.pwszID == cast_handle->pwszIDRealDevices[index2]) 
			{
				sound_device.isRealDevice = true;
			}
		}

		if (sound_device.pwszID == wcp_user_seleted_playback_device_guid)
		{
			sound_device.isUserSelectedPlaybackDevice = true;
		}
		if (sound_device.pwszID == wcp_targeted_real_playback_device_guid &&
			sound_device.pwszID != wcp_dfx_device_guid)
		{
			sound_device.isTargetedRealPlaybackDevice = true;
		}
		if (sound_device.pwszID == wcp_capture_device_guid)
		{
			sound_device.isCaptureDevice = true;
		}
		if (sound_device.pwszID == wcp_dfx_device_guid)
		{
			sound_device.isDFXDevice = true;
		}
		if (sound_device.pwszID == wcp_default_device_guid)
		{
			sound_device.isDefaultDevice = true;
		}

		sound_devices_.push_back(sound_device);
	}	

	return(OKAY);
}

void AudioPassthruPrivate::onDeviceChange()
{
	// Clear the unavailability flag so processTimer() can retry after this device change.
	s_sndDevices_.playbackDeviceIsUnavailable = FALSE;

	if (s_callback_ != nullptr)
	{
		s_callback_->onSoundDeviceChange(false);
	}
}

/*
* FUNCTION: killProcessingThread()
* DESCRIPTION:
*
*  If the processing thread is running, kill it.
*  This function does not return until the thread has been killed or it times out
*  trying to kill the thread.
*
*/
int AudioPassthruPrivate::killProcessingThread(int *ip_timed_out)
{
	BOOL b_need_to_kill_thread;
	BOOL bReturn;
	DWORD d_ExitCode;
	int i_thread_has_died;
	long l_total_msecs_waited;

	*ip_timed_out = IS_FALSE;

	/* Check if we should kill the processing thread */
	b_need_to_kill_thread = FALSE;

	if (hProcessingThread_ != NULL && hProcessingThread_ != INVALID_HANDLE_VALUE)
	{
		bReturn = GetExitCodeThread(hProcessingThread_, &d_ExitCode);
		if (d_ExitCode == STILL_ACTIVE)
		{
			b_need_to_kill_thread = TRUE;
		}
	}
	else if (hProcessingThread_ == INVALID_HANDLE_VALUE)
	{
		hProcessingThread_ = NULL;  // clear sentinel — no thread is running
	}

	/* Kill the thread */
	if (b_need_to_kill_thread)
	{
		// Stop capture.  This will cause sndDevicesDoCapture() to exit and then the thread will die.
		if (sndDevicesStartStopCapture(hp_sndDevices_, SND_DEVICES_STOP_CAPTURE) != OKAY)
			return(NOT_OKAY);

		i_kill_processing_thread_ = IS_TRUE;

		/* Wait until thread has died or we have timed out */
		i_thread_has_died = IS_FALSE;
		l_total_msecs_waited = 0L;

		while ((!i_thread_has_died) && (!(*ip_timed_out)))
		{
			bReturn = GetExitCodeThread(hProcessingThread_, &d_ExitCode);
			if (d_ExitCode != STILL_ACTIVE)
			{
				i_thread_has_died = IS_TRUE;
				CloseHandle(hProcessingThread_);
				hProcessingThread_ = NULL;
			}
			else
			{
				Sleep(DFXG_SND_SERVER_KILL_THREAD_WAIT_PER_LOOP_MSECS);
				l_total_msecs_waited = l_total_msecs_waited + DFXG_SND_SERVER_KILL_THREAD_WAIT_PER_LOOP_MSECS;
				if (l_total_msecs_waited >= DFXG_SND_SERVER_KILL_THREAD_TIMEOUT_MSECS)
					*ip_timed_out = IS_TRUE;
			}
		}
	}

	return(OKAY);
}

/*
* FUNCTION: setBufferLength()
* DESCRIPTION:
*
*  Sets the buffer length.
*
*/
int AudioPassthruPrivate::setBufferLength(int i_buffer_length_msecs)
{
	int i_timed_out;

	/*
	* Set the new buffer setting.
	* This just sets registry value which will be picked up during reinit.
	*/
	if (sndDevicesSetBufferSizeMilliSecs(hp_sndDevices_, i_buffer_length_msecs) != OKAY)
		return(NOT_OKAY);

	/* Kill the processing thread, so that the timer will then restart it with the new buffer setting */
	if (killProcessingThread(&i_timed_out) != OKAY)
		return(NOT_OKAY);

	/* If the killing of processing attempt timed out, then we can't do anything else */
	if (i_timed_out)
		return(OKAY);

	return(OKAY);
}

/*
* FUNCTION: processTimer()
* DESCRIPTION:
*
*  Process the snd server timer.
*/
int AudioPassthruPrivate::processTimer()
{
	BOOL bReturn;
	BOOL b_need_to_start_thread;
	DWORD d_ExitCode;
	int numRealDevices;
	int DfxDeviceEnabledFlag;
	int statusFlag;

	b_need_to_start_thread = FALSE;

	/*
	* Check if we need to start the thread for the first time or restart it.
	* Keep in mind that the main thread will kill itself if anything changes in terms of active soundcards.
	* This is kind of a brute force way of reinitializing and taking care of changes.
	*/
	if (hProcessingThread_ == NULL)
		b_need_to_start_thread = TRUE;
	else if (hProcessingThread_ == INVALID_HANDLE_VALUE)
	{
		// Playback device was unavailable on the last attempt. Wait for onDeviceChange() to
		// clear playbackDeviceIsUnavailable before retrying, to avoid hammering sndDevicesReInit.
		if (!s_sndDevices_.playbackDeviceIsUnavailable)
			hProcessingThread_ = NULL;  // flag cleared — allow retry on next tick
	}
	else
	{
		bReturn = GetExitCodeThread(hProcessingThread_, &d_ExitCode);
		if (d_ExitCode != STILL_ACTIVE)
		{
			b_need_to_start_thread = TRUE;
			CloseHandle(hProcessingThread_);
			hProcessingThread_ = NULL;
		}
	}

	/* BetterFxSound: an output change that arrived during a delay measurement is applied once it is over */
	if (!b_need_to_start_thread && calibration_.getState(NULL) != DelayCalibration::State::Running)
	{
		bool apply_pending;
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			apply_pending = apply_pending_;
		}
		if (apply_pending)
			applyAdditionalOutputs();
	}

	/*
	* BetterFxSound: a playback device other than the virtual device and the carrier changed (an additional output
	* plugged in or gone, an unused HDMI monitor going to sleep). That no longer stops the processing, which interrupted
	* every output, so the reinit no longer reads the device list again and tells the controller. It is done here: the
	* list is read again, the additional outputs follow it, and the controller updates its outputs (it may still switch
	* the carrier, which requests a reinit).
	*/
	if (!b_need_to_start_thread && (hProcessingThread_ != NULL) && (hProcessingThread_ != INVALID_HANDLE_VALUE) &&
		(InterlockedExchange(&(s_sndDevices_.deviceListIsStale), 0) != 0))
	{
		/*
		* BetterFxSound: sndCheckDeviceChanges() marks the list stale again for the difference this refresh is about to
		* read, which made every change refresh twice. The flag is cleared once more before the list is read: a change
		* notified from here on (during the read) still marks it, and is refreshed on a later tick.
		*/
		bool list_changed = checkDeviceChanges();
		InterlockedExchange(&(s_sndDevices_.deviceListIsStale), 0);
		if (list_changed)
			sndDevices_GetAll(hp_sndDevices_, &(s_sndDevices_.totalNumDevices));

		applyAdditionalOutputs();

		if (s_callback_ != nullptr)
			s_callback_->onSoundDeviceChange(true);
	}

	if (b_need_to_start_thread)
	{
		/* Initialize flag which can be set by the outside telling thread to end */
		i_kill_processing_thread_ = IS_FALSE;

		/* Reinit the sndDevices module */
		if (sndDevicesReInit(hp_sndDevices_, SND_DEVICES_INIT_FOR_PROCESSING, &numRealDevices, &DfxDeviceEnabledFlag, &statusFlag) != OKAY)
			return(NOT_OKAY);

		/*
		* BetterFxSound: the reinit settled which device is the carrier, a requested switch is no longer pending.
		* The carrier's stream was opened again, so the engine starts using its device again: its volume is set
		* to 100% and unmuted once more by applyAdditionalOutputs() below.
		*/
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			pending_carrier_id_.clear();
			carrier_full_volume_id_.clear();
		}

		if (statusFlag != SND_DEVICES_DEVICE_OPERATION_COMPLETED)
		{
			/*
			if (cast_handle->trace.mode)
			{
				swprintf(cast_handle->wcp_msg1, 2048, L"dfxg_ProcessSndServerTimer(): sndDevicesReInit failed, statusFlag = %d", statusFlag);
				(cast_handle->slout1)->Message_Wide(FIRST_LINE, cast_handle->wcp_msg1);
			}

			if (statusFlag == SND_DEVICES_NO_VALID_PLAYBACK_DEVICE)
			{
				if (dfxg_GetTranslatedString(hp_dfxg, IDS_DFX_MSG_ASK_EXIT_DFX_TO_HEAR_SOUNDS,
					cast_handle->wcp_translated_string) != OKAY)
					return(NOT_OKAY);
			}

			if (statusFlag == SND_DEVICES_ASK_USER_SELECT_PLAYBACK_DEVICE)
			{
				if (dfxg_GetTranslatedString(hp_dfxg, IDS_DFX_MSG_SELECT_AUDIO_PLAYBACK_FROM_MENU,
					cast_handle->wcp_translated_string) != OKAY)
					return(NOT_OKAY);
			}

			if (cast_handle->snd_server.b_no_valid_snd_device_dialog_shown == false)
			{
				cast_handle->snd_server.b_no_valid_snd_device_dialog_shown = true;
				if (dfxg_BringUpMsgModelessDlg(hp_dfxg, cast_handle->wcp_translated_string, IS_FALSE, NULL, IS_FALSE, 0) != OKAY)
					return(NOT_OKAY);
			}
			*/
		}
		else
		{
			b_no_valid_snd_device_dialog_shown_ = false;
		}

		/* PTNOTE - added check on DfxDeviceEnabledFlag status, may need to take additional steps if no DFX device is preset. */
		if ((numRealDevices > 0) && (DfxDeviceEnabledFlag == IS_TRUE))
		{
			/*
			* BetterFxSound: the reinit may have changed the carrier, and its own settings are only published by
			* applyAdditionalOutputs() below. Until then the new thread plays the carrier silent (then fades it in),
			* instead of with the previous carrier's channel mode, delay and trim. Its delay line starts empty, so
			* it does not replay audio from before the reinit either. The previous thread has exited, so these
			* processing thread variables are not in use.
			*/
			carrier_enabled_.store(false, std::memory_order_relaxed);
			carrier_gain_ = 0.0f;
			main_delay_format_ = 0;

			hProcessingThread_ = CreateThread(NULL, 0, processingThread, (LPVOID)this, 0L, &ProcessingThreadID_);

			/* BetterFxSound: processing runs again, the real devices are FxSound's again */
			{
				std::lock_guard<std::mutex> lock(config_mutex_);
				handed_back_ = false;
			}
			multi_output_.setDriven(true);

			/* BetterFxSound: the carrier may have changed, recompute the carrier settings and the additional outputs */
			applyAdditionalOutputs();
		}
		else
		{
			// No usable playback path right now:
			//  - No real (physical) playback device is present (numRealDevices == 0), or
			//  - The DFX virtual device is not enabled/found, or
			//  - The playback device is unavailable (e.g. AUDCLNT_E_UNSUPPORTED_FORMAT /
			//    AUDCLNT_E_DEVICE_IN_USE, signaled by playbackDeviceIsUnavailable).
			// In every case there is nothing to be gained from re-running sndDevicesReInit
			// every 100ms, which would both burn CPU and leak COM objects on every retry
			// (each reinit re-enumerates all devices and re-activates the audio clients).
			// Use INVALID_HANDLE_VALUE as a sentinel to pause retries; onDeviceChange()
			// clears playbackDeviceIsUnavailable when the device situation changes, which
			// then allows a retry.
			if ((numRealDevices <= 0) || (DfxDeviceEnabledFlag != IS_TRUE))
				s_sndDevices_.playbackDeviceIsUnavailable = TRUE;
			hProcessingThread_ = INVALID_HANDLE_VALUE;
		}

		if (s_callback_ != nullptr)
			s_callback_->onSoundDeviceChange(true);

		/* Check if a new playback device has been selected */
		/*
		if (dfxg_SndServerHdmiFixCheckForNewPlaybackDevice(hp_dfxg) != OKAY)
			return(NOT_OKAY);
		*/
	}

	return(OKAY);
}

/*
* FUNCTION: threadWorker()
* DESCRIPTION:
*/
DWORD AudioPassthruPrivate::threadWorker(void)
{
	float *fp_buffer;
	int numSampleSets;
	WAVEFORMATEX *pwfx;
	int i_check_for_duplicate_buffers;
	int i_valid_bits;
	int resultFlag;
	DWORD setReturn;

	// Raise the priority of this tread to improve performance. GetCurrentThread() is a call that
	// returns the current thread ID from within the thread itself.
	// A return of 0 means set failed. Not sure what option to use, MS doc is confusing, THREAD_PRIORITY_HIGHEST is another option.
	setReturn = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	// Start capture.
	if (sndDevicesStartStopCapture(hp_sndDevices_, SND_DEVICES_START_CAPTURE) != OKAY)
		return(NOT_OKAY);

	/*
	* Keep looping until a goto statement is reached to kill the thread
	* Also, constantly check to see the kill_thread flag has been set from the outside.
	*/
	while (1)
	{
		/* Check if thread has been signaled to end */
		if (i_kill_processing_thread_)
			goto KillProcessingThread;

		// Does the data capture and returns a pointer to the data and signal info to be used for in place audio processing.
		// NOT_OKAY is only returned for catastrophic errors but if resultFlag != SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS
		// then typically a change in a device property has caused the capture or playback operation to fail, in this
		// case we need to exit this thread so a reinitialization can be done.
		if (sndDevicesDoCapture(hp_sndDevices_, &fp_buffer, &numSampleSets, &pwfx, &resultFlag) != OKAY)
			return(NOT_OKAY);

		// A non-successful flag will typically be due to a change in the playback devices properties.
		if (resultFlag != SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS)
			goto KillProcessingThread;

		/* Check if thread has been signaled to end */
		if (i_kill_processing_thread_)
			goto KillProcessingThread;

		if (numSampleSets > 0)
		{
			/* Set additional processing settings */
			i_valid_bits = pwfx->wBitsPerSample;
			i_check_for_duplicate_buffers = IS_FALSE;

			/* Make sure processing module has the current format settings */
			//if (dfxpUniversalSetSignalFormat(cast_handle->dfxp_hdl, pwfx->wBitsPerSample, pwfx->nChannels, pwfx->nSamplesPerSec, i_valid_bits) != OKAY)
				//return(NOT_OKAY);
			if (p_dfx_dsp_->setSignalFormat(pwfx->wBitsPerSample, pwfx->nChannels, pwfx->nSamplesPerSec, i_valid_bits) != OKAY)
			{
				// In Release build, initially setSignalFormat() will return NOT OKAY so execution will hit here and throws up message box due to return NOT_OAKY.
				// However, it doesn't happen in Debug build. Workaround is just comment out return(NOT_OKAY) so it won't throw up message boxes in Release build
				// and eventually it will start to return OKAY.
				//return(NOT_OKAY);
			}
				
			

			// 2016-08-25: Workaround for crashing when playback device has only one channel (mono). For example, a Bleutooth headset is mono. When such
			// device is selected as the playback device, we do not apply DFX/DSP processing to the buffer for now. Otherwise it will crash.
			// NOTE: There will also be no sound with a mono playback device until we add code to fill mono playback buffer in sndDevicesDoCapture.cpp, line 279.
			if (pwfx->nChannels != 1 || (pwfx->nChannels == 1 && !SND_DEVICES_MONO_BUG_DO_NOT_PROCESS))
			{
				// Apply DFX processing here using data and format vars above. Format will always be 32 bit floating point.
			//	if (dfxpUniversalModifySamples(cast_handle->dfxp_hdl, (short int *)fp_buffer, (short int *)fp_buffer, numSampleSets, i_check_for_duplicate_buffers) != OKAY)
				//	return(NOT_OKAY);
				p_dfx_dsp_->processAudio((short int *)fp_buffer, (short int *)fp_buffer, numSampleSets, i_check_for_duplicate_buffers);
			}

			/*
			* BetterFxSound: queue the processed block for the additional outputs. This copies the
			* data and never blocks, so it has to happen here, before sndDevicesDoPlayback() reuses
			* the buffer for upsampling.
			*/
			if (!mute_)
			{
				multi_output_.write(fp_buffer, (UINT32)numSampleSets, pwfx->nChannels, pwfx->nSamplesPerSec, solo_.load(std::memory_order_relaxed));
			}

			/*
			* BetterFxSound: the carrier output's settings, read once for the whole block. carrier_enabled_ is
			* read first (acquire) and published last (release) by applyAdditionalOutputs(), and every new
			* processing thread starts with it off. So an enabled carrier is always played with its own
			* channel mode, never with the one of the previous carrier.
			*/
			const bool carrier_enabled = carrier_enabled_.load(std::memory_order_acquire);
			const OutputChannelMode carrier_channel_mode = static_cast<OutputChannelMode>(main_channel_mode_.load(std::memory_order_relaxed));

			/* BetterFxSound: optional delay for the carrier. It keeps every channel, so a new channel mode is heard at once. */
			applyMainDelay(fp_buffer, (UINT32)numSampleSets, pwfx->nChannels, pwfx->nSamplesPerSec);

			/* BetterFxSound: optional channel mode for the carrier (left only, right only or mono), on what is played now */
			applyOutputChannelMode(fp_buffer, (UINT32)numSampleSets, pwfx->nChannels, carrier_channel_mode);

			/* BetterFxSound: Windows master volume x this output's trim, or silence if the carrier output is disabled */
			applyCarrierGain(fp_buffer, (UINT32)numSampleSets, pwfx->nChannels, pwfx->nSamplesPerSec, carrier_enabled);

			/* BetterFxSound: while an additional output is being measured, the carrier is silent */
			{
				int solo = solo_.load(std::memory_order_relaxed);
				if (solo >= 0)
					memset(fp_buffer, 0, sizeof(float) * (size_t)numSampleSets * pwfx->nChannels);
			}

			/* Check if thread has been signaled to end */
			if (i_kill_processing_thread_)
				goto KillProcessingThread;

			/* A non-successful flag will typically be due to a change in the playback devices properties. */
			if (resultFlag != SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS)
				goto KillProcessingThread;
		}

		/*
		* Plays processed buffer.
		* NOTE: This needs to outside of the if block so that unplayed buffers can still be played.
		*       It is okay to call this even when all buffers have already been played.
		*/
		if (!mute_)
		{
			if (sndDevicesDoPlayback(hp_sndDevices_, &resultFlag) != OKAY)
				return(NOT_OKAY);
		}
		

		/* Make sure the playback succeeded */
		if (resultFlag != SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS)
			goto KillProcessingThread;
	}

KillProcessingThread:

	// Stop capture.
	if (sndDevicesStartStopCapture(hp_sndDevices_, SND_DEVICES_STOP_CAPTURE) != OKAY)
		return(NOT_OKAY);

	/* Simply returning from this function stops the thread */
	return(OKAY);
}

/*
* FUNCTION: processingThread()
* DESCRIPTION:
*/
DWORD WINAPI AudioPassthruPrivate::processingThread(LPVOID lpParam)
{
	// Make sure COM is initialized
	HRESULT hr;
	hr = CoInitialize(NULL);

	AudioPassthruPrivate * callerClass = (AudioPassthruPrivate*)lpParam;
	auto ret = callerClass->threadWorker();

	CoUninitialize();

	return ret;
}

void AudioPassthruPrivate::mute(bool mute)
{
	mute_ = mute;
}

void AudioPassthruPrivate::registerCallback(AudioPassthruCallback* callback)
{
	s_callback_ = callback;
}

bool AudioPassthruPrivate::isPlaybackDeviceAvailable()
{
    BOOL availability;

    sndDevicesGetPlaybackDeviceAvialblility(hp_sndDevices_, &availability);
    if (availability == TRUE)
        return true;
    else
        return false;
}

bool AudioPassthruPrivate::checkDeviceChanges()
{
    BOOL deviceChanged;

	sndCheckDeviceChanges(hp_sndDevices_, &deviceChanged);
	if (deviceChanged == TRUE)
		return true;
	else
        return false;
}

void AudioPassthruPrivate::restoreDefaultPlaybackDevice()
{
	int i_resultFlag;

	/*
	* BetterFxSound: the real device becomes the Windows default again and plays the apps' sound directly at
	* its own level. Until processing starts again, the engine must not set any device to 100%.
	*/
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		handed_back_ = true;
	}
	multi_output_.setDriven(false);

	/* Change the default soundcard to not be the DFX virtual one but instead the proper real one */
	if (sndDevicesRestoreDefaultDevice(hp_sndDevices_, &i_resultFlag) != OKAY)
		return;

	/* BetterFxSound: anything still owed (a device that was away when it was handed back, or left by a crash) */
	handBackUnusedDevices(std::vector<std::wstring>());
}

int AudioPassthruPrivate::setTargetedRealPlaybackDevice(const std::wstring sound_device_guid)
{
	int i_resultFlag;
	wchar_t wcp_new_targeted_real_playback_guid[PT_MAX_GENERIC_STRLEN];
	
	/* Set the guid of the newly selected playback device */
	swprintf(wcp_new_targeted_real_playback_guid, PT_MAX_GENERIC_STRLEN, L"%s", sound_device_guid.c_str());

	/*
	* BetterFxSound: while processing, with the FxSound virtual device as the Windows default, the switch goes through
	* the device rules (sndDevicesRequestTargetedPlaybackDevice()) and the Windows default is left alone. Making the
	* real device the default for the switch let every app play straight to it, unprocessed and at its own level,
	* until the reinit made the virtual device the default again (a third of a second per carrier switch).
	* Otherwise (FxSound off, or starting): set the newly targeted playback device as the default. It will then
	* automatically become the targeted device.
	*/
	bool handed_back;
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		handed_back = handed_back_;
	}
	/*
	* BetterFxSound: Windows is asked which device is the default now. The cached defaultDeviceNum only follows the
	* device notifications, which arrive some time after a change.
	*/
	BOOL virtual_is_default = FALSE;
	if (sndDevicesGetVirtualDeviceIsDefault(hp_sndDevices_, &virtual_is_default) != OKAY)
		virtual_is_default = FALSE;
	if (isProcessing() && !handed_back && virtual_is_default)
	{
		if (sndDevicesRequestTargetedPlaybackDevice(hp_sndDevices_, wcp_new_targeted_real_playback_guid, &i_resultFlag) != OKAY)
			return(NOT_OKAY);
	}
	else
	{
		if (sndDevicesSetDeviceType(hp_sndDevices_, SND_DEVICES_DEFAULT, wcp_new_targeted_real_playback_guid, &i_resultFlag) != OKAY)
			return(NOT_OKAY);

		/*
		* BetterFxSound: while processing, the switch only takes effect at a reinit. It used to rely on the notification
		* of the new Windows default to stop the processing thread, and nothing happened when that notification did
		* not come (the default did not change, or setting it failed), leaving the new carrier pending and silent.
		*/
		if (i_resultFlag != SND_DEVICES_DEVICE_NOT_PRESENT && isProcessing() && !handed_back)
		{
			if (sndDevicesRequestTargetedPlaybackDevice(hp_sndDevices_, wcp_new_targeted_real_playback_guid, &i_resultFlag) != OKAY)
				return(NOT_OKAY);
		}
	}

	/*
	* BetterFxSound: the device is not in the device list, so no switch was requested and no reinit will follow to
	* clear a pending carrier. Marking it pending would keep it out of the additional outputs for good.
	*/
	if (i_resultFlag == SND_DEVICES_DEVICE_NOT_PRESENT)
		return(OKAY);

	/*
	* BetterFxSound: the switch takes effect at the next reinit. Until then the new carrier is kept out of the
	* additional outputs, so it is not opened twice (once there, once as the carrier) for a moment.
	*/
	{
		wchar_t wcp_targeted_guid[PT_MAX_GENERIC_STRLEN];
		if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_TARGETED_REAL_PLAYBACK, wcp_targeted_guid, &i_resultFlag) != OKAY)
			wcp_targeted_guid[0] = L'\0';

		bool changed = false;
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			if (!sound_device_guid.empty() && sound_device_guid != wcp_targeted_guid && sound_device_guid != pending_carrier_id_)
			{
				pending_carrier_id_ = sound_device_guid;
				changed = true;
			}
		}
		if (changed)
			applyAdditionalOutputs();
	}

	return(OKAY);
}
/*
* FUNCTION: setOutputs()
* DESCRIPTION: BetterFxSound. Stores the whole requested set of outputs, carrier included, and applies it.
*/
void AudioPassthruPrivate::setOutputs(const std::vector<OutputConfig>& outputs)
{
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		outputs_ = outputs;
	}
	applyAdditionalOutputs();
}

/*
* FUNCTION: isDevicePresent()
* DESCRIPTION: BetterFxSound. True if the device is an active playback device in the list read at the last
* device enumeration. A device arrival or removal either causes a reinit or, for a device other than the virtual
* device and the carrier, a list refresh in processTimer(); both re-read that list and then call
* applyAdditionalOutputs(), so an output that is plugged in later is picked up then.
*/
bool AudioPassthruPrivate::isDevicePresent(const std::wstring& device_id) const
{
	if (device_id.empty())
		return false;

	for (int index = 0; index < s_sndDevices_.totalNumDevices && index < SND_DEVICES_MAX_NUM_DEVICES; index++)
	{
		if (s_sndDevices_.deviceState[index] == DEVICE_STATE_ACTIVE && device_id == s_sndDevices_.pwszID[index])
			return true;
	}
	return false;
}

/*
* FUNCTION: applyAdditionalOutputs()
* DESCRIPTION: BetterFxSound. Works out, for the current carrier (the targeted real playback device):
*  - whether the carrier plays, and with which delay, channel mode and trim (published as atomics that the
*    processing thread reads);
*  - which other outputs play through the MultiOutputRouter. The carrier (it already plays the audio), a
*    carrier switch that is pending, the FxSound virtual device (playing into it would feed the output back
*    into the capture), disabled outputs and devices that are not present are left out.
* Called after every change of the requested outputs and after every reinit, since the carrier may change.
*/
void AudioPassthruPrivate::applyAdditionalOutputs()
{
	int i_resultFlag;
	wchar_t wcp_targeted_guid[PT_MAX_GENERIC_STRLEN];
	wchar_t wcp_dfx_guid[PT_MAX_GENERIC_STRLEN];

	wcp_targeted_guid[0] = L'\0';
	wcp_dfx_guid[0] = L'\0';

	if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_TARGETED_REAL_PLAYBACK, wcp_targeted_guid, &i_resultFlag) != OKAY)
		wcp_targeted_guid[0] = L'\0';
	if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_VIRTUAL_PLAYBACK_DFX, wcp_dfx_guid, &i_resultFlag) != OKAY)
		wcp_dfx_guid[0] = L'\0';

	const std::wstring carrier_id(wcp_targeted_guid);
	const std::wstring dfx_id(wcp_dfx_guid);
	const bool processing = isProcessing();
	std::wstring set_full_volume_id;	// Carrier device to set to 100% and unmute, after the lock is released
	float carrier_own_level = 1.0f;		// And its own setting, recorded as owed before that
	BOOL carrier_own_mute = FALSE;
	std::vector<std::wstring> in_use;	// Devices the engine drives now: what is owed to them stays owed

	{
		std::lock_guard<std::mutex> lock(config_mutex_);

		std::vector<AdditionalOutputConfig> effective;

		// Whether a requested output stays out of the router. Duplicates are dropped here as well, so the
		// indexes in effective_outputs_ match the router's own list (the delay measurement relies on it).
		auto excluded = [&](const std::wstring& id) -> bool
		{
			if (id.empty() || id == carrier_id || id == dfx_id || id == pending_carrier_id_)
				return true;
			for (const auto& added : effective)
			{
				if (added.device_id == id)
					return true;
			}
			return !isDevicePresent(id);
		};

		// The carrier's own entry decides whether it plays. Missing or disabled: it plays silence.
		bool carrier_enabled = false;
		int carrier_delay_ms = 0;
		OutputChannelMode carrier_channel_mode = OutputChannelMode::Both;
		int carrier_volume_percent = 100;

		for (const auto& output : outputs_)
		{
			if (!carrier_id.empty() && output.device_id == carrier_id && carrier_id != dfx_id)
			{
				carrier_enabled = output.enabled;
				carrier_delay_ms = output.delay_ms;
				carrier_channel_mode = output.channel_mode;
				carrier_volume_percent = output.volume_percent;
				break;
			}
		}

		for (const auto& output : outputs_)
		{
			if (!output.enabled || excluded(output.device_id))
				continue;

			AdditionalOutputConfig config;
			config.device_id = output.device_id;
			config.delay_ms = output.delay_ms;
			config.channel_mode = output.channel_mode;
			config.volume_percent = output.volume_percent;
			effective.push_back(config);
		}

		if (carrier_delay_ms < 0) carrier_delay_ms = 0;
		if (carrier_delay_ms > 500) carrier_delay_ms = 500;

		const bool carrier_plays = carrier_enabled && !carrier_id.empty();

		// The device's own level and mute are saved by the reinit (sndDevicesInitialSetupPlaybackDevice()) and
		// put back on power off, exit or a switch to another carrier. Without them the device is left alone.
		const bool carrier_setting_saved = s_sndDevices_.playbackSettingsAreSaved && s_sndDevices_.savedPlaybackLevelIsSaved &&
			carrier_id == s_sndDevices_.savedPlaybackDeviceID;

		// carrier_enabled_ last, with release: the processing thread reads it first (acquire), so once it
		// sees this carrier enabled it also sees this carrier's delay, channel mode and trim.
		main_delay_ms_.store(carrier_delay_ms, std::memory_order_relaxed);
		main_channel_mode_.store(static_cast<int>(carrier_channel_mode), std::memory_order_relaxed);
		carrier_trim_.store(outputTrimGain(carrier_volume_percent), std::memory_order_relaxed);
		carrier_enabled_.store(carrier_plays, std::memory_order_release);

		/*
		* The real device of an enabled carrier is set to 100% and unmuted when the engine starts using it: after
		* every reinit (start, power on, carrier switch) and when the carrier output gets enabled. Once only, no
		* callback keeps forcing it. Only while processing: when FxSound is off, the real device is the Windows
		* default and plays the apps' sound directly at its own level. Only once its own setting is saved, so it
		* can be handed back. carrier_plays already excludes the FxSound virtual device.
		*/
		if (!carrier_plays)
		{
			carrier_full_volume_id_.clear();
		}
		else if (processing && !handed_back_ && carrier_setting_saved && carrier_full_volume_id_ != carrier_id)
		{
			carrier_full_volume_id_ = carrier_id;
			set_full_volume_id = carrier_id;
			carrier_own_level = s_sndDevices_.savedPlaybackLevel;
			carrier_own_mute = s_sndDevices_.savedPlaybackMute;
		}

		/* The measurement relies on the output order, keep it stable while it runs. processTimer() applies the
		   change once the measurement is over. */
		if (calibration_.getState(NULL) == DelayCalibration::State::Running)
		{
			apply_pending_ = true;

			// A carrier switch cannot wait: the carrier's sweeps (solo -1) would now play on another device and be
			// reported for the old one, which stays silent because it cannot join the router before the end.
			if (carrier_id != calibration_carrier_id_)
				calibration_.abort(L"The outputs changed during the measurement. Synchronize again.");
		}
		else
		{
			// The carrier can still be one of the router's outputs here: a carrier switch during a measurement is
			// left out of the router only now, after the reinit took the device over as the carrier. Removing that
			// output hands the device's own level back, so the carrier takes it over again below, after the router
			// change (the COM calls run after this block).
			bool carrier_was_routed = false;
			for (const auto& output : effective_outputs_)
			{
				if (!carrier_id.empty() && output.device_id == carrier_id)
					carrier_was_routed = true;
			}

			apply_pending_ = false;
			effective_outputs_ = effective;
			multi_output_.configure(effective);

			if (carrier_was_routed && carrier_plays && processing && !handed_back_ && carrier_setting_saved)
			{
				carrier_full_volume_id_ = carrier_id;
				set_full_volume_id = carrier_id;
				carrier_own_level = s_sndDevices_.savedPlaybackLevel;
				carrier_own_mute = s_sndDevices_.savedPlaybackMute;
			}
		}

		/* The carrier while it plays (its device is FxSound's), and the router's outputs while the router drives
		   them: each of those takes its device over and hands it back itself. While FxSound is off the router's
		   outputs don't (their streams only open when audio is written), so what is owed to their devices is put
		   back here, for example to Bluetooth headphones that left while in use and came back while FxSound is off. */
		if (carrier_plays && processing && !handed_back_)
			in_use.push_back(carrier_id);
		if (multi_output_.isDriven())
		{
			for (const auto& output : effective_outputs_)
				in_use.push_back(output.device_id);
		}
	}

	/*
	* COM calls, outside the lock. The change is tagged with this app's GUID, so the playback volume callback
	* does not take it for the user's own mute setting (the one handed back when FxSound stops driving the device).
	* The device's own setting is recorded as owed first (kept if one is owed already: the reinit then saved it
	* from the record, see sndDevicesInitialSetupPlaybackDevice()), so it survives an unplug or a crash.
	*/
	if (!set_full_volume_id.empty())
	{
		owedVolumeRecord(set_full_volume_id, carrier_own_level, carrier_own_mute);
		s_sndDevices_.engineSetPlaybackLevel = 1.0f;
		s_sndDevices_.engineSetPlaybackMute = FALSE;
		s_sndDevices_.engineSetPlaybackTick = GetTickCount();
		setDeviceFullVolumeById(set_full_volume_id, &s_sndDevices_.guidThisApplication);
	}

	/* A device the engine no longer uses (output turned off while it is the carrier, back after an unplug, left at
	   100% by a crash) gets its own setting back. */
	handBackUnusedDevices(in_use);
}

/*
* FUNCTION: handBackUnusedDevices()
* DESCRIPTION: BetterFxSound. Puts the own level and mute back on every present device that is owed them and that
* the engine does not drive now. Cheap when nothing is owed: one registry read.
*/
void AudioPassthruPrivate::handBackUnusedDevices(const std::vector<std::wstring>& in_use)
{
	for (const auto& id : owedVolumeDevices())
	{
		if (std::find(in_use.begin(), in_use.end(), id) != in_use.end())
			continue;
		if (!isDevicePresent(id))
			continue;
		handBackOwedVolumeById(id);
	}
}

/*
* FUNCTION: isProcessing()
* DESCRIPTION: BetterFxSound. True while the processing thread runs, that is while FxSound drives the devices and
* the FxSound virtual device is the Windows default.
*/
bool AudioPassthruPrivate::isProcessing() const
{
	HANDLE thread = hProcessingThread_;
	if (thread == NULL || thread == INVALID_HANDLE_VALUE || i_kill_processing_thread_)
		return false;

	DWORD exit_code = 0;
	return GetExitCodeThread(thread, &exit_code) && exit_code == STILL_ACTIVE;
}

/*
* FUNCTION: applyCarrierGain()
* DESCRIPTION: BetterFxSound. Applies the Windows master volume (level and mute) times the carrier's own trim
* to the carrier in place, or fades it to silence when the carrier output is disabled. Changes are ramped
* over about 10 ms so they do not click. Processing thread only: atomics and arithmetic, no allocation,
* no lock, no COM call.
*
* Loopback capture is taken before the virtual device's endpoint volume, so the buffer is full scale and
* this is the only place the master volume is applied to the carrier: there is no double attenuation.
*/
void AudioPassthruPrivate::applyCarrierGain(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate, bool enabled)
{
	if (buffer == NULL || frames == 0 || channels == 0)
		return;

	const float target = enabled
		? carrier_trim_.load(std::memory_order_relaxed) * masterVolumeGain()
		: 0.0f;

	const size_t samples = (size_t)frames * channels;

	if (carrier_gain_ == target)
	{
		// Steady state.
		if (target == 1.0f)
			return;
		if (target == 0.0f)
		{
			memset(buffer, 0, sizeof(float) * samples);
			return;
		}
		for (size_t i = 0; i < samples; i++)
			buffer[i] *= target;
		return;
	}

	const float step = (float)(1.0 / (0.01 * (double)(sample_rate > 0 ? sample_rate : 48000)));
	float gain = carrier_gain_;
	for (UINT32 i = 0; i < frames; i++)
	{
		gain = rampGain(gain, target, step);
		float* frame = buffer + (size_t)i * channels;
		for (UINT32 c = 0; c < channels; c++)
			frame[c] *= gain;
	}
	carrier_gain_ = gain;
}

/*
* FUNCTION: applyMainDelay()
* DESCRIPTION: BetterFxSound. Delays the carrier in place. Processing thread only, no allocation.
* With a delay of 0 the audio passes through unchanged.
*/
void AudioPassthruPrivate::applyMainDelay(float* buffer, UINT32 frames, UINT32 channels, UINT32 sample_rate)
{
	if (buffer == NULL || channels == 0 || channels > kMainDelayChannels || sample_rate == 0 || main_delay_buffer_.empty())
		return;

	// A new format invalidates what is stored, start over with silence.
	UINT32 format = (sample_rate << 4) | channels;
	if (format != main_delay_format_)
	{
		std::fill(main_delay_buffer_.begin(), main_delay_buffer_.end(), 0.0f);
		main_delay_write_ = 0;
		main_delay_format_ = format;
	}

	UINT32 delay_frames = (UINT32)((UINT64)main_delay_ms_.load(std::memory_order_relaxed) * sample_rate / 1000);
	if (delay_frames >= kMainDelayFrames)
		delay_frames = kMainDelayFrames - 1;

	const UINT32 mask = kMainDelayFrames - 1;
	for (UINT32 i = 0; i < frames; i++)
	{
		float* frame = buffer + (size_t)i * channels;
		float* slot_in = &main_delay_buffer_[(size_t)(main_delay_write_ & mask) * kMainDelayChannels];
		memcpy(slot_in, frame, sizeof(float) * channels);
		const float* slot_out = &main_delay_buffer_[(size_t)((main_delay_write_ - delay_frames) & mask) * kMainDelayChannels];
		memcpy(frame, slot_out, sizeof(float) * channels);
		main_delay_write_++;
	}
}

/*
* FUNCTION: startDelayCalibration()
* DESCRIPTION: BetterFxSound. Starts measuring the delay of every enabled output that is present with the
* microphone: the carrier if it is enabled, and every output currently playing through the router (those are
* already limited to enabled, present devices). With nothing to measure it fails at once, with an error that
* says whether no output is turned on or none of those turned on is connected. It also fails at once when the
* sweeps could not be heard (processing stopped or muted, Windows volume muted or at 0) or a carrier switch is
* pending. A carrier switch while it runs aborts it (applyAdditionalOutputs()).
*/
bool AudioPassthruPrivate::startDelayCalibration()
{
	int i_resultFlag;
	wchar_t wcp_targeted_guid[PT_MAX_GENERIC_STRLEN];
	wchar_t wcp_dfx_guid[PT_MAX_GENERIC_STRLEN];

	if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_TARGETED_REAL_PLAYBACK, wcp_targeted_guid, &i_resultFlag) != OKAY)
		wcp_targeted_guid[0] = L'\0';
	if (sndDevicesGetID(hp_sndDevices_, SND_DEVICES_VIRTUAL_PLAYBACK_DFX, wcp_dfx_guid, &i_resultFlag) != OKAY)
		wcp_dfx_guid[0] = L'\0';

	// A change that is still waiting for the end of a previous measurement goes in first, so the targets
	// below match what the router really plays.
	{
		bool apply_pending;
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			apply_pending = apply_pending_;
		}
		if (apply_pending && calibration_.getState(NULL) != DelayCalibration::State::Running)
			applyAdditionalOutputs();
	}

	std::vector<DelayCalibration::Target> targets;
	std::wstring no_targets_error;
	{
		std::lock_guard<std::mutex> lock(config_mutex_);

		// The carrier is measured only if it is enabled, and is not necessarily the first result. Each target
		// carries its current delay: its sweep arrives that much later and is listened for that much longer.
		if (wcp_targeted_guid[0] != L'\0' && carrier_enabled_.load(std::memory_order_relaxed))
		{
			DelayCalibration::Target carrier_target = { std::wstring(wcp_targeted_guid), -1, main_delay_ms_.load(std::memory_order_relaxed) };
			targets.push_back(carrier_target);
		}
		for (size_t i = 0; i < effective_outputs_.size(); i++)
		{
			DelayCalibration::Target target = { effective_outputs_[i].device_id, (int)i, effective_outputs_[i].delay_ms };
			targets.push_back(target);
		}

		// A carrier switch during the measurement aborts it, see applyAdditionalOutputs().
		if (calibration_.getState(NULL) != DelayCalibration::State::Running)
			calibration_carrier_id_ = wcp_targeted_guid;

		// The sweeps could not be heard anywhere, or would be played to a carrier that is about to change:
		// fail at once with the reason rather than play them for nothing.
		std::wstring cannot_run_error;
		if (!pending_carrier_id_.empty())
			cannot_run_error = L"The outputs are not ready yet. Wait a moment, then synchronize again.";
		else if (mute_ || !isProcessing())
			cannot_run_error = L"FxSound is not playing sound right now. Make sure it is on, wait a moment, then synchronize again.";
		else if (masterVolumeGain() <= 0.0f)
			cannot_run_error = L"The Windows volume is muted or at 0, so the test sound cannot be heard. Turn it up, then synchronize again.";

		if (!targets.empty() && !cannot_run_error.empty())
		{
			targets.clear();
			no_targets_error = cannot_run_error;
		}
		// Nothing to measure: say why. No output turned on, or none of those turned on is connected, or
		// (rarely) the outputs are still being set up, for example during a switch of the carrier.
		else if (targets.empty())
		{
			bool any_enabled = false;
			bool any_connected = false;
			for (const auto& output : outputs_)
			{
				if (!output.enabled || output.device_id.empty() || output.device_id == wcp_dfx_guid)
					continue;
				any_enabled = true;
				if (isDevicePresent(output.device_id))
					any_connected = true;
			}

			if (!any_enabled)
				no_targets_error = L"No output is turned on. Turn on at least one output, then synchronize again.";
			else if (!any_connected)
				no_targets_error = L"None of the outputs that are turned on is connected. Connect one of them, then synchronize again.";
			else
				no_targets_error = L"The outputs are not ready yet. Wait a moment, then synchronize again.";
		}
	}

	// With no target the measurement fails at once with the error above. The virtual device is only needed
	// to play the test sound, so it is required only when there is something to measure.
	if (!targets.empty() && wcp_dfx_guid[0] == L'\0')
		return false;

	std::atomic<int>* solo = &solo_;
	return calibration_.start(std::wstring(wcp_dfx_guid), targets, [solo](int index) { solo->store(index, std::memory_order_relaxed); },
		no_targets_error);
}

void AudioPassthruPrivate::cancelDelayCalibration()
{
	calibration_.cancel();
}

int AudioPassthruPrivate::getDelayCalibrationState(int* progress_percent)
{
	return (int)calibration_.getState(progress_percent);
}

std::vector<DelayCalibrationResult> AudioPassthruPrivate::getDelayCalibrationResults()
{
	return calibration_.getResults();
}

std::wstring AudioPassthruPrivate::getDelayCalibrationError()
{
	return calibration_.getError();
}
