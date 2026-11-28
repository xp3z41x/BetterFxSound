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
#ifndef _SND_DEVICES_H_
#define _SND_DEVICES_H_

#include <Winbase.h>
#include <comdef.h>
#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <atomic>
#include "slout.h"

#define PT_MAX_GENERIC_STRLEN          512
/*
 * THERE IS CURRENTLY A PROBLEM WITH MONO SOUND DEVICES.
 * We can temporarily turn off mono soundcard functionality
 * with these defines.
 */

#define SND_DEVICES_MONO_BUG_DO_NOT_PROCESS		IS_TRUE
#define SND_DEVICES_MONO_BUG_FORCE_SILENCE		IS_TRUE
#define SND_DEVICES_MONO_BUG_SKIP_MONO_DEVICES	IS_TRUE

/*
#define SND_DEVICES_MONO_BUG_DO_NOT_PROCESS		IS_TRUE
#define SND_DEVICES_MONO_BUG_FORCE_SILENCE		IS_FALSE
#define SND_DEVICES_MONO_BUG_SKIP_MONO_DEVICES	IS_FALSE
*/

/* Limit settings */
#define SND_DEVICES_MAX_NUM_DEVICES 64

/* BetterFxSound: the most channels the carrier's resampler handles (sndDevicesDoPlayback()), a carrier with more is silent */
#define SND_DEVICES_CARRIER_MAX_CHANNELS 32

/* BetterFxSound: reading again an active device's channel count that could not be read (sndDevicesGetChannelProbeIsDue()) */
#define SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES		10
#define SND_DEVICES_CHANNEL_PROBE_INTERVAL_MS		500

/* Device "friendly name" string, used to identify DFX device. */
#define SND_DEVICES_DFX_DEVICE_STRING L"FxSound Audio Enhancer"
#define SND_DEVICES_DFX_PREVIOUS_DEVICE_STRING L"DFX Audio Enhancer"

/* Device "description name" (default is "Speakers") */
#define SND_DEVICES_DFX_DEVICE_DESCRIPTION_STRING L"FxSound Speakers"


/* Initialization and control type specifiers */
#define SND_DEVICES_INIT_FOR_PROCESSING	1	/* Use when audio processing will be performed */
#define SND_DEVICES_INIT_NO_PROCESSING		2	/* Use when only device operations will be performed but no audio processing*/

#define SND_DEVICES_START_CAPTURE			6
#define SND_DEVICES_STOP_CAPTURE				7

/* Mode Flags */
#define SND_DEVICES_AUTO_SELECT_DEFAULT_DEVICE_OFF	0
#define SND_DEVICES_AUTO_SELECT_DEFAULT_DEVICE_ON	1

/* DFX processed buffer playback Status */
#define SND_DEVICES_PLAYBACK_IS_STOPPED 0
#define SND_DEVICES_PLAYBACK_IS_ACTIVE	1

/* Get and Set result flags */
#define SND_DEVICES_DEVICE_OPERATION_COMPLETED	 0
#define SND_DEVICES_GENERIC_FAILURE					-1
#define SND_DEVICES_DEVICE_NOT_PRESENT				-2
#define SND_DEVICES_PROPERTY_SET_FAILED			-3
#define SND_DEVICES_RULES_NOT_POSSIBLE				-4
#define SND_DEVICES_DLL_LOAD_ERROR					-13
#define SND_DEVICES_THREAD_MADE_REQUESTED_EXIT	-14
#define SND_DEVICES_THREAD_TERMINATE_FAILED		-15
#define SND_DEVICES_INSTANCE_CREATE_FAILED		-16
#define SND_DEVICES_ENUMERATE_FAILED				-17
#define SND_DEVICES_REGISTER_FAILED					-18
#define SND_DEVICES_GETCOUNT_FAILED					-19
#define SND_DEVICES_GETID_FAILED						-20
#define SND_DEVICES_GET_DEVICES_FAILED				-21
#define SND_DEVICES_GET_AUDIOENDPOINT_FAILED		-22
#define SND_DEVICES_GET_INDEXED_DEVICE_FAILED	-23
#define SND_DEVICES_GET_PADDING_FAILED				-24
#define SND_DEVICES_GET_REG_PROPS_FAILED			-25
#define SND_DEVICES_GET_BUFFER_FAILED				-26
#define SND_DEVICES_RELEASE_BUFFER_FAILED			-27
#define SND_DEVICES_NULL_CAPTURE_CLIENT			-28
#define SND_DEVICES_NULL_PLAYBACK_CLIENT			-29
#define SND_DEVICES_NULL_LOOPBACK_CLIENT			-30
#define SND_DEVICES_WAIT_FOR_MUTEX_FAILED			-31
#define SND_DEVICES_PLAYBACK_RENDER_FAILED		-32
#define SND_DEVICES_THREAD_PLAYBACK_FAILED		-33
#define SND_DEVICES_DEVICE_ACTIVATION_FAILED		-34
#define SND_DEVICES_DEVICE_GET_FORMAT_FAILED		-35
#define SND_DEVICES_DEVICE_SET_FORMAT_FAILED		-36
#define SND_DEVICES_DEVICE_OPEN_PROPS_FAILED		-37
#define SND_DEVICES_DEVICE_ICON_PROP_GET_FAILED -38
#define SND_DEVICES_DEVICE_INIT_PROP_FAILED		-39
#define SND_DEVICES_CAPTURE_DEVICE_IS_NULL		-40
#define SND_DEVICES_SAMP_FREQ_NOT_VALID			-41
#define SND_DEVICES_NUM_CHANS_NOT_VALID			-42
#define SND_DEVICES_MP3_DLL_LOAD_FAILED			-43
#define SND_DEVICES_SET_MASTER_VOLUME_FAILED		-44
#define SND_DEVICES_GET_MASTER_VOLUME_FAILED		-45
#define SND_DEVICES_NULL_VOLUME_ENDPOINT			-46
#define SND_DEVICES_SET_MUTE_FAILED					-47
#define SND_DEVICES_NULL_CAPTURE_DEVICE			-48
#define SND_DEVICES_NULL_PLAYBACK_DEVICE			-49
#define SND_DEVICES_GET_SERVICE_FAILED				-50
#define SND_DEVICES_AUDIO_CLIENT_INIT_FAILED		-54
#define SND_DEVICES_NO_VALID_PLAYBACK_DEVICE 	-57
#define SND_DEVICES_ASK_USER_SELECT_PLAYBACK_DEVICE	-58

/* Device Status Change Events */
#define SND_DEVICES_NO_CHANGE						0
#define SND_DEVICES_DEFAULT_DEVICE_CHANGED	1
#define SND_DEVICES_DEVICE_ADDED					2
#define SND_DEVICES_DEVICE_REMOVED				3
#define SND_DEVICES_DEVICE_ACTIVE				4
#define SND_DEVICES_DEVICE_DISABLED				5
#define SND_DEVICES_DEVICE_NOTPRESENT			6
#define SND_DEVICES_DEVICE_UNPLUGGED			7
#define SND_DEVICES_DEVICE_PROPERTY_CHANGE	8

/* Capture and playback return flags */
#define SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS		0
#define SND_DEVICES_FILE_PLAYBACK_SUCCESS			0
#define SND_DEVICES_CAPTURE_ERROR					201
#define SND_DEVICES_CAPTURE_FILE_WRITE_ERROR		202
#define SND_DEVICES_PLAYBACK_ERROR					203
#define SND_DEVICES_CAPTURE_FORCED_EXIT			204
#define SND_DEVICES_FILE_PLAYBACK_FORCED_EXIT	205
#define SND_DEVICES_FILE_PLAYBACK_REACHED_EOF	206
#define SND_DEVICES_CAPTURE_NOT_POSSIBLE			207
#define SND_DEVICES_PLAYBACK_NOT_POSSIBLE			208
#define SND_DEVICES_NO_REAL_DEVICES_FOUND			209

/* Device specifiers and related storage locations */
#define SND_DEVICES_TARGETED_REAL_PLAYBACK			100
#define SND_DEVICES_VIRTUAL_PLAYBACK_DFX				101
#define SND_DEVICES_USER_SELECTED_PLAYBACK_DEVICE	103
#define SND_DEVICES_CAPTURE								104
#define SND_DEVICES_DEFAULT								105
#define SND_DEVICES_PRIOR_DEFAULT						106
#define SND_DEVICES_PRIOR_PLAYBACK						107

/* General control defines */
#define SND_DEVICES_USER_SETTING				0
#define SND_DEVICES_DEFAULT_SETTING			1

/* Registry related defines, current user */
#define SND_DEVICES_REGISTRY_DEVICES_WIDE							L"devices"
#define SND_DEVICES_REGISTRY_ORIGINAL_DEFAULT_WIDE				L"original_default"
#define SND_DEVICES_REGISTRY_MOST_RECENT_DEFAULT_WIDE			L"most_recent_default"
#define SND_DEVICES_REGISTRY_PRIOR_DEFAULT_WIDE					L"prior_default"
#define SND_DEVICES_REGISTRY_MOST_RECENT_PLAYBACK_WIDE		L"most_recent_playback"
#define SND_DEVICES_REGISTRY_USER_SELECTED_PLAYBACK_WIDE		L"user_selected_playback"
#define SND_DEVICES_REGISTRY_DEFAULT_DEVICE_SELECTION_MODE	L"default_device_mode"
#define SND_DEVICES_REGISTRY_USER_BUFFER_SIZE					L"user_buffer_size"

/* Registry related defines, local machine */
#define SND_DEVICES_REGISTRY_DFX_GUID								L"dfx_guid"
#define SND_DEVICES_REGISTRY_DEFAULT_BUFFER_SIZE				L"default_buffer_size"

/* Dfx Device sample freq flags */
#define SND_DEVICES_DFX_SAMP_FREQ_44_1			0
#define SND_DEVICES_DFX_SAMP_FREQ_48			1

/* Controls the period in MS where following device callbacks are ignored */
#define SND_DEVICES_CALLBACK_TIME_WINDOW		0
/* Controls the time in MS that the callbacks wait before setting reset flag */
#define SND_DEVICES_CALLBACK_WAIT_TIME			0
/* Controls the time in MS that RuleChange code sleeps after changing the default device */
#define SND_DEVICES_DEFAULT_CHANGE_WAIT_TIME	1500

/* Max number of allowed simultaneous encoding threads. */
#define SND_DEVICES_MAX_ENCODING_THREAD_COUNT 4

/* Time to wait in millisecs for the encoding thread termination to complete */
#define SND_DEVICES_MAX_WAIT_FOR_ENCODING_THREAD_TERMINATION 1000

#define SND_DEVICES_MAX_SAMP_FREQ 192000
#define SND_DEVICES_MIN_NUM_CHANS 2
#define SND_DEVICES_MAX_NUM_CHANS 8

// These are the average delay buffer sizes. The actual internal buffers will be twice this length.
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_32BIT_OS_32BIT_CPU 80
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_32BIT_VISTA_32BIT_CPU 100
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_32BIT_VISTA_64BIT_CPU 100
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_32BIT_OS_64BIT_CPU 60
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_64BIT_OS 40
#define SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS SND_DEVICES_CAPTURE_BUFFER_DEFAULT_SIZE_MILLI_SECS_32BIT_OS_32BIT_CPU
#define SND_DEVICES_CAPTURE_BUFFER_MIN_SIZE_MILLI_SECS 10
#define SND_DEVICES_CAPTURE_BUFFER_MAX_SIZE_MILLI_SECS 100
#define SND_DEVICES_REFTIMES_PER_SEC 1.0e7		// A fixed reference number, used in for internal calculations

// Function Status Error Macro, uses Slout module.
// First make a w_char version of __FILE__
#define SND_DEVICES_WIDE2(x) L##x
#define SND_DEVICES_WIDE1(x) SND_DEVICES_WIDE2(x)
#define SND_DEVICES_WFILE SND_DEVICES_WIDE1(__FILE__)

#define SND_DEVICES_SET_STATUS_AND_RETURN_OK(status)  { \
cast_handle->function_status = status; \
if((cast_handle->i_trace_on) && (cast_handle->slout_hdl)) \
{ \
	swprintf(cast_handle->wcp_msg1, PT_MAX_GENERIC_STRLEN, L"File %s, Line %d :: sndDevices Error Code: %d ", SND_DEVICES_WFILE, __LINE__, cast_handle->function_status); \
	cast_handle->slout_hdl->Message_Wide(FIRST_LINE, cast_handle->wcp_msg1); \
 } \
return(OKAY);}

// Device callback class declarations.
class CsndDevicesMMNotificationClient : public IMMNotificationClient
{
    LONG _cRef;
    IMMDeviceEnumerator *_pEnumerator;

public:
    CsndDevicesMMNotificationClient();

    ~CsndDevicesMMNotificationClient();

	 // This is a sndDevices handle that is set during sndDevicesInit and then used in this class for all handle based ops.
	 PT_HANDLE *g_sndDevicesCallbacks_hdl;

    // IUnknown methods -- AddRef, Release, and QueryInterface

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(
                                REFIID riid, VOID **ppvInterface);
    
    // Callback methods for device-event notifications.

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
                                EDataFlow flow, ERole role,
                                LPCWSTR pwstrDeviceId);

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId);

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId);

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
                                LPCWSTR pwstrDeviceId,
                                DWORD dwNewState);

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
                                LPCWSTR pwstrDeviceId,
                                const PROPERTYKEY key);

	 // Added to allow handle to be set during sndDevicesInit call
	 void SetPtHandle(PT_HANDLE *sndDevices_hdl);
};

/* Capture and Playback device Volume Callback Declarations */
class CsndDevicesAudioEndpointVolumeCallbackCapture : public IAudioEndpointVolumeCallback
{
	LONG _cRef;

public:
    CsndDevicesAudioEndpointVolumeCallbackCapture();

    ~CsndDevicesAudioEndpointVolumeCallbackCapture();

	 // This is a sndDevices handle that is set during sndDevicesInit and then used in this class for all handle based ops.
	 PT_HANDLE *g_sndDevicesVolumeCallbackCapture_hdl;

    // IUnknown methods -- AddRef, Release, and QueryInterface

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface);

	 // PTNOTE - this appears to be the callback hit when the real control is moved.
    // Callback method for endpoint-volume-change notifications.
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify);

	 // Added to allow handle to be set during sndDevicesInit call
	 void SetPtHandle(PT_HANDLE *sndDevices_hdl);

	 bool IsRegistered();
};

class CsndDevicesAudioEndpointVolumeCallbackPlayback : public IAudioEndpointVolumeCallback
{
	LONG _cRef;

public:
    CsndDevicesAudioEndpointVolumeCallbackPlayback();

    ~CsndDevicesAudioEndpointVolumeCallbackPlayback();

	 // This is a sndDevices handle that is set during sndDevicesInit and then used in this class for all handle based ops.
	 PT_HANDLE *g_sndDevicesVolumeCallbackPlayback_hdl;

    // IUnknown methods -- AddRef, Release, and QueryInterface

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface);

	 // PTNOTE - this appears to be the callback hit when the real control is moved.
    // Callback method for endpoint-volume-change notifications.
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify);

	 // Added to allow handle to be set during sndDevicesInit call
	 void SetPtHandle(PT_HANDLE *sndDevices_hdl);

	 bool IsRegistered();
};


struct sndDevicesHdlType {
   
	/* Message info */
   CSlout *slout_hdl;
   wchar_t wcp_msg1[PT_MAX_GENERIC_STRLEN]; /* String for messages */
   int i_trace_on;

	// Note, originally declared as const but had to take that off to move them into global struct.
	CLSID CLSID_MMDeviceEnumerator;
	IID IID_IMMDeviceEnumerator;
	IID IID_IAudioClient;
	IID IID_IAudioCaptureClient;
	IID IID_IAudioRenderClient;
	IID IID_IAudioEndpointVolume;

	GUID guidThisApplication; // For identifying in callbacks device volume control changes made by this app

	IMMDevice *pAllDevices[SND_DEVICES_MAX_NUM_DEVICES]; // Object pointers for each device.
	IMMDevice *pCaptureDevice;
	IMMDevice *pPlaybackDevice;

	WCHAR pwszID[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN]; // For the GUID ID strings for each device, all devices combined.
	LPWSTR pwszIDRealDevices[SND_DEVICES_MAX_NUM_DEVICES]; // For the GUID ID strings for each real playback device.
	WCHAR pwszIDPreviousRealDevices[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN]; // To detect when a new devices is added.
    DWORD deviceState[SND_DEVICES_MAX_NUM_DEVICES]; // State of each device.

	wchar_t deviceFriendlyName[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN];  // Friendly name, ie "DFX Audio Enhancer 10.5", all devices.
	wchar_t deviceDescription[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN];  // Descriptive name, ie "Speakers", all devices.
	wchar_t *deviceFriendlyNameRealDevices[SND_DEVICES_MAX_NUM_DEVICES];  // Friendly names, just the real devices.
	wchar_t *deviceDescriptionRealDevices[SND_DEVICES_MAX_NUM_DEVICES];  // Descriptive names, just the real devices.
	int deviceNumChannel[SND_DEVICES_MAX_NUM_DEVICES]; // Number of channels for all devices.
	wchar_t deviceFormFactor[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN]; // Form factor type string, eg "HDMI", "Speakers", etc.

   IAudioClient *pAudioClientCapture;
   IAudioCaptureClient *pAudioCaptureLoopback;
	IAudioEndpointVolume *pEndptVolCapture;
   IAudioClient *pAudioClientPlayback;
   IAudioRenderClient *pAudioClientPlaybackRender;
	IAudioEndpointVolume *pEndptVolPlayback;

	CsndDevicesMMNotificationClient DeviceEvents; // For device callbacks.
	CsndDevicesAudioEndpointVolumeCallbackCapture EPVolEventsCapture;	  // For capture device volume callbacks.
	CsndDevicesAudioEndpointVolumeCallbackPlayback EPVolEventsPlayback; // For playback device volume callbacks.

   REFERENCE_TIME hnsRequestedDurationCapture;
   REFERENCE_TIME hnsActualDurationCapture;
   REFERENCE_TIME hnsRequestedDurationPlayback;
   REFERENCE_TIME hnsActualDurationPlayback;

	WAVEFORMATEX wfxCapture;
	WAVEFORMATEX wfxPlayback;
	WAVEFORMATEX wfxDfxProcessing;
	WAVEFORMATEX wfxRecording;

   UINT32 numCaptureFramesAvailable;

   UINT32 bufferFrameSizeCapture;  // Contain the actual full size of the internal shared buffers in frames.
   UINT32 bufferFrameSizePlayback;

	// For small packet exchanges, these point to temp memory in the record and playback methods
   BYTE *pDataPacketCapture;
   BYTE *pDataPacketPlayback;

	// Full length buffers, scale up a little for rounding slop
	// Now allocated in init call.
	float *fCaptureBuf;
	float *fPlaybackBuf;
	float *fFilePlaybackBuf;
	int captureBufAllocSize;
	int playbackBufAllocSize;

	UINT32 capturedFramesCount;		// The number of frames we have read from the system capture device.
	UINT32 playbackFrameCount;			// The number of frames we have ready to pass to the system playback device.
    UINT32 numPlaybackFramesAvailableToFill;	// The number of open frames available to fill in the system playback buffer.

	UINT32 upsampleRatio;
	float sigPower;
	int bufferSizeMilliSecs;		// This is the average delay, actual internal buffers are twice this length.
	//int playback_has_started;
	// BetterFxSound: written from the device notification threads, the timer thread and the processing thread, so it
	// is set with InterlockedExchange() and read as a volatile. Only sndDevicesReInit() clears it, before it enumerates
	// the devices, so a request made at any later point (a device change, a carrier switch) makes the processing
	// thread started after that reinit exit at once and run another reinit, instead of being lost.
	volatile LONG stopAudioCaptureAndPlaybackLoop;
	int dfxDeviceNum;	// The combo 44.1k and 48k hz. DFX device
	//int dfx48DeviceNum;	// The 48k hz. DFX device
	int defaultDeviceNum;
	// BetterFxSound: the Windows default (eRender, eMultimedia) device as read by the last sndDevices_GetAll(). Unlike
	// defaultDeviceNum, the device notification callbacks never write it, so the device rules can rely on it.
	int enumeratedDefaultDeviceNum;
	// BetterFxSound: the ID of the FxSound virtual device, kept across reinits (dfxDeviceNum is reset during one), so
	// the device notification callbacks can recognize it at any time.
	wchar_t dfxDeviceID[PT_MAX_GENERIC_STRLEN];
	// BetterFxSound: odd while sndDevices_GetAll() rewrites the device list. The device notification callbacks read
	// the list without a lock, and only trust what they read when this was even and unchanged around the read.
	// sndDevices_GetAll() makes it odd only from an even value (compare-exchange), and so does a device notification
	// callback for the moment it publishes defaultDeviceNum, so the two writers never overlap.
	volatile LONG enumerationSequence;
	// BetterFxSound: set when a playback endpoint other than the virtual device and the carrier changed (the device
	// notification callbacks, sndCheckDeviceChanges()). Such a change no longer stops the processing; the timer reads
	// the device list again and refreshes the outputs instead (AudioPassthruPrivate::processTimer()).
	volatile LONG deviceListIsStale;
	// BetterFxSound: set by the device notification callback when a playback endpoint's format changed
	// (PKEY_AudioEngine_DeviceFormat), for example a Bluetooth headset going mono while its microphone is open and
	// back to stereo after. Its state does not change, so the comparison with the system (sndCheckDeviceChanges())
	// would find no difference: this makes it read the list (and the channel counts) again.
	volatile LONG deviceFormatChanged;
	// BetterFxSound: the channel count of each device as last read by sndDevices_GetAll() (0 when unknown), by ID. A
	// device whose format cannot be read for a moment (being reconfigured) keeps the count it had, instead of 0, which
	// made it look absent to the controller.
	WCHAR channelCacheID[SND_DEVICES_MAX_NUM_DEVICES][PT_MAX_GENERIC_STRLEN];
	int channelCacheNumChannel[SND_DEVICES_MAX_NUM_DEVICES];
	// BetterFxSound: per device, how many enumerations started by a due probe could not read its channel count yet.
	int channelCacheProbeRetries[SND_DEVICES_MAX_NUM_DEVICES];
	int channelCacheCount;
	// BetterFxSound: TRUE when the last enumeration found an active device whose channel count it never read, and that
	// is still under SND_DEVICES_MAX_CHANNEL_PROBE_RETRIES. The list is then read again (sndDevicesGetChannelProbeIsDue()).
	BOOL activeDeviceIsUnprobed;
	// BetterFxSound: set by sndDevicesGetChannelProbeIsDue() when it reports a probe as due, consumed by the next
	// enumeration, which alone counts as a retry (not the GUI's, a reinit loop pass or a notification's refresh).
	volatile LONG channelProbeIsPending;
	DWORD lastEnumerationTick;	// GetTickCount() at the end of the last sndDevices_GetAll()
	// BetterFxSound: the carrier the controller asked for (sndDevicesRequestTargetedPlaybackDevice()). The next run of the
	// device rules takes it first, and consumes it.
	wchar_t requestedPlaybackID[PT_MAX_GENERIC_STRLEN];
	// BetterFxSound: TRUE once the controller manages the outputs (AudioPassthruPrivate::setOutputs()). It elects the
	// carrier itself, so the device rules no longer make a newly added device the carrier.
	BOOL outputsAreManaged;
	int playbackDeviceNum;
	int captureDeviceNum;
	int priorDefaultDeviceNum;
	int totalNumDevices;
	int numPreviousRealDevices;
	int numRealDevices;
	int deviceCallBackType;
	DWORD lastDeviceCallbackTime;
	DWORD lastDeviceAddCallbackGuidtype;
	wchar_t lastDeviceAddCallbackGuid[PT_MAX_GENERIC_STRLEN]; // Contains the guid of the last device added or removed that generated a callback.
	int totalCallbackCount;			// For debugging.
	int totalReInitCalls;			// For debugging.
	BOOL ignoreDeviceCallbacks;
	BOOL ignoreVolumeCallbacks;
	int initializationMode;		// Will be either SND_DEVICES_INIT_FOR_PROCESSING or SND_DEVICES_INIT_NO_PROCESSING
	int playbackIsActive;		// Will be either SND_DEVICES_PLAYBACK_IS_STOPPED or SND_DEVICES_PLAYBACK_IS_ACTIVE.
	int playbackStreamIsTemporarilyPaused;	// Used to turn off playback stream when no capture or playback is occuring.

	// BetterFxSound: carrier pacing (sndDevicesDoCapture(), sndDevicesDoPlayback()). Processing thread only, reset by
	// sndDevicesStartStopCapture() when a processing thread starts, carrierStallCount excepted.
	DWORD processingStartTick;			// GetTickCount() when this processing thread started.
	LONGLONG lastCapturePacketQpc;		// QueryPerformanceCounter() when the last loopback packet was read.
	LONGLONG qpcFrequency;				// QueryPerformanceFrequency().
	DWORD carrierLastConsumedTick;		// When the running carrier was last seen playing out of its queue.
	DWORD carrierLastPollTick;			// When sndDevicesDoCapture() last read the carrier's queue.
	UINT32 carrierLastPadding;			// The carrier's queue (playback frames) as last read, or right after the last write.
	UINT32 carrierStallCapturedFrames;	// Frames captured since the running carrier last played out of its queue.
	int carrierStallCount;				// Consecutive stalls of the carrier, each reinit waits longer; it gives up at the max.
	int timerPeriodRaised;				// sndDevicesDoCapture() raised the timer resolution (sndDevicesReleaseTimerPeriod()).
	double carrierErrorAvg;				// Averaged offset of the queue from its setpoint right after a write, seconds.
	double carrierReadPos;				// Resampler: input position of the next output frame, from -2 (see carrierHistory).
	float carrierHistory[3][SND_DEVICES_CARRIER_MAX_CHANNELS];	// Resampler: the last 3 frames of the previous block.
	float carrierLowPassState[2][2][SND_DEVICES_CARRIER_MAX_CHANNELS];	// Resampler: anti-alias filter state (2 biquads).
	// BetterFxSound: the carrier's delay line (AudioPassthruPrivate::applyMainDelay()) still holds this many frames of
	// audio not played yet. While it does, a pause in the loopback (no packet for SND_DEVICES_TAIL_GAP_MS) makes
	// sndDevicesDoCapture() hand over silence, as much as the pause lasted, so that audio plays out on time instead of
	// staying in the line and being played before the next sound. Processing thread only.
	UINT32 carrierDelayTailFrames;
	LONGLONG carrierTailSilenceQpc;		// sndDevicesDoCapture(): QPC up to which the pause was handed over as silence.
	BOOL capturedBlockIsSilenceFill;	// The block sndDevicesDoCapture() handed over is such silence, not captured audio.

	// The mute setting that belongs to the real playback device FxSound is currently rendering to, ie
	// the setting the device had before FxSound applied its own to it, plus any the user has since made
	// on the device itself. It is put back on that device when FxSound stops driving it, so a device we
	// stop using is not left muted when the user never muted it.
	// BetterFxSound: the device's own volume level is saved and put back the same way. While FxSound uses
	// the device as an enabled output, the engine sets it to 100% and unmuted (the Windows volume is then
	// applied inside the app), so what the device gets back is the level the user had, or has since set on
	// the device itself.
	wchar_t savedPlaybackDeviceID[PT_MAX_GENERIC_STRLEN];	// ID of the device this setting belongs to.
	BOOL savedPlaybackMute;			// Its mute setting.
	float savedPlaybackLevel;		// Its volume level, 0..1 as IAudioEndpointVolume reports it.
	BOOL savedPlaybackLevelIsSaved;	// FALSE if the level could not be read; then only mute is put back.
	BOOL playbackSettingsAreSaved;	// TRUE when the values above hold a valid setting.
	// BetterFxSound: the last level and mute this app set on the playback device, and when (GetTickCount()). Some
	// drivers (Bluetooth with absolute volume) report such a change back a second time, without this app's event
	// context; the playback volume callback must not take that for the user's own setting.
	// Written on the message thread, read on the COM thread that delivers the callback: atomics, the tick written
	// last and read first.
	std::atomic<float> engineSetPlaybackLevel;
	std::atomic<BOOL> engineSetPlaybackMute;
	std::atomic<DWORD> engineSetPlaybackTick;
	// BetterFxSound: the engine writes the level, then the mute, and a driver may report the level change on its own:
	// the new level with the mute the device had before (engineSetPlaybackFromMute). A late report of the engine's
	// previous change (engineSetPlaybackPrev*, when the engine changes the device again within a few seconds) is not
	// the user's either. Written with sndDevicesNoteEngineSetPlayback(), before engineSetPlaybackTick.
	std::atomic<BOOL> engineSetPlaybackFromMute;
	std::atomic<float> engineSetPlaybackPrevLevel;
	std::atomic<BOOL> engineSetPlaybackPrevMute;
	std::atomic<DWORD> engineSetPlaybackPrevTick;
	// BetterFxSound: when the engine last set the carrier's device to 100% and unmuted when it started using it, 0 when it
	// does not hold the device now. A change of the device's own level a few seconds after that (a Bluetooth device with
	// absolute volume sending its stored level once it is connected) is not taken for the user's: the callback asks for
	// 100% again (playbackFullVolumeReassert, applied by AudioPassthruPrivate::processTimer()).
	std::atomic<DWORD> engineTookOverPlaybackTick;
	std::atomic<BOOL> playbackFullVolumeReassert;
	// BetterFxSound: the user changed the carrier's own level or mute while the engine held it at 100%: the device keeps
	// that setting (it is also the owed one) and a reinit does not set it to 100% again, until the engine hands the
	// device back (sndDevices_RestoreSavedPlaybackDeviceSettings()). The same policy as the additional outputs.
	std::atomic<BOOL> playbackLevelIsUsers;

	int noBufferCount;			// This is a count of how many times no buffers were available in a DoRecording call.
	int WindowsSilentBufferCount;				// This is a buffer specified by the Windows flag to be all zeros.
	int MeasuredVirtualSilentBufferCount;	// This is buffer of measured all very low values, essentially silence.
	int MeasuredTrueSilentBufferCount;		// This is buffer of measured all true zero values.
    BOOL playbackDeviceIsUnavailable;
	// BetterFxSound: the last reinit failed on the carrier itself (its format could not be read, or its stream could not
	// be set up or opened), not on the FxSound virtual device. Written by sndDevicesReInit(), read after it; UI/timer
	// thread only.
	BOOL playbackSetupFailed;

	// Module common status flag, set by functions that can't complete their requestion operation
	int function_status;

	void (*deviceChangeCallback)();
};

// BetterFxSound: notes a level and mute the engine is about to set on the carrier's device, and the mute the device has
// before it, so the playback volume callback does not take a driver's report of them for the user's own change. The
// previous change is kept as well (see engineSetPlaybackPrev*).
inline void sndDevicesNoteEngineSetPlayback(struct sndDevicesHdlType *cast_handle, float level, BOOL mute, BOOL from_mute)
{
	cast_handle->engineSetPlaybackPrevLevel = cast_handle->engineSetPlaybackLevel.load();
	cast_handle->engineSetPlaybackPrevMute = cast_handle->engineSetPlaybackMute.load();
	cast_handle->engineSetPlaybackPrevTick = cast_handle->engineSetPlaybackTick.load();
	cast_handle->engineSetPlaybackLevel = level;
	cast_handle->engineSetPlaybackMute = mute;
	cast_handle->engineSetPlaybackFromMute = from_mute;
	cast_handle->engineSetPlaybackTick = GetTickCount();
}

_COM_SMARTPTR_TYPEDEF(IMMDevice, __uuidof(IMMDevice));
_COM_SMARTPTR_TYPEDEF(IMMDeviceEnumerator, __uuidof(IMMDeviceEnumerator));
_COM_SMARTPTR_TYPEDEF(IMMDeviceCollection, __uuidof(IMMDeviceCollection));
_COM_SMARTPTR_TYPEDEF(IPropertyStore, __uuidof(IPropertyStore));

int PT_DECLSPEC sndDevicesInit(PT_HANDLE *, CSlout *, int, int, int *);
int PT_DECLSPEC sndDevicesFree(PT_HANDLE *);

/* sndDevicesReInit.cpp */
int PT_DECLSPEC sndDevicesReInit(PT_HANDLE *, int, int *, int *, int *);
int PT_DECLSPEC sndCheckDeviceChanges(PT_HANDLE*, BOOL*);
int PT_DECLSPEC sndDevices_GetAll(PT_HANDLE*, int*);

/* sndDevicesGet.cpp */
int PT_DECLSPEC sndDevicesGetID(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetFormatFromID(PT_HANDLE *, wchar_t *, WAVEFORMATEX *, int *);
int PT_DECLSPEC sndDevicesGetFriendlyName(PT_HANDLE *, int , wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetFriendlyNameFromID(PT_HANDLE *, wchar_t *, wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetDeviceName(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetDeviceNameFromID(PT_HANDLE *, wchar_t *, wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetNumberOfChannelsFromID(PT_HANDLE *, wchar_t *, int *, int *);
int PT_DECLSPEC sndDevicesGetIconFileName(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesGetLoopStopFlag(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesGetBuffersForProcessing(PT_HANDLE *, float **, int *, WAVEFORMATEX **);
int PT_DECLSPEC sndDevicesGetTotalNumDevices(PT_HANDLE *,  int *);
int PT_DECLSPEC sndDevicesGetIDFromIndex(PT_HANDLE *, int, wchar_t *);
int PT_DECLSPEC sndDevicesGetDefaultDeviceSelectionMode(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesGetAllRealDeviceIDs(PT_HANDLE *, wchar_t ***, int *);
int PT_DECLSPEC sndDevicesGetBufferSizeMilliSecs(PT_HANDLE *, int, int *);
int PT_DECLSPEC sndDevicesGetRecommendedBufferSizeMilliSecs(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesGetPlayBackStatus(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesGetNumMonoDevices(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesGetPlaybackDeviceAvialblility(PT_HANDLE*, BOOL*);
int PT_DECLSPEC sndDevicesGetVirtualDeviceIsDefault(PT_HANDLE*, BOOL*);
int PT_DECLSPEC sndDevicesGetChannelProbeIsDue(PT_HANDLE*, BOOL*);

/* sndDevicesSet.cpp */
int PT_DECLSPEC sndDevicesSetDeviceType(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesRequestTargetedPlaybackDevice(PT_HANDLE *, wchar_t *, int *);
int PT_DECLSPEC sndDevicesSetIconFileName(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesSetDeviceName(PT_HANDLE *, int, wchar_t *, int *);
int PT_DECLSPEC sndDevicesSetDefaultDeviceSelectionMode(PT_HANDLE *, int);
int PT_DECLSPEC sndDevicesSetDeviceEnabledStatus(PT_HANDLE *, int, BOOL, int *);
int PT_DECLSPEC sndDevicesSetDeviceEnabledStatusFromGuid(PT_HANDLE *, wchar_t *, BOOL, int *);
int PT_DECLSPEC sndDevicesSetDfxDeviceSampleRateAndChannels(PT_HANDLE *, int, int, int *);
int PT_DECLSPEC sndDevicesSetBufferSizeMilliSecs(PT_HANDLE *, int);

/* sndDevicesImplementDeviceRules.cpp */
int PT_DECLSPEC sndDevicesImplementDeviceRules(PT_HANDLE *, int *);

/* sndDevicesSetupDevices.cpp */
int PT_DECLSPEC sndDevicesInitialSetupCaptureDevice(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesInitialSetupPlaybackDevice(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesFinalSetupCaptureDevice(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesFinalSetupPlaybackDevice(PT_HANDLE *, int *);
int PT_DECLSPEC sndDevicesRestoreDefaultDevice(PT_HANDLE *, int *);

/* sndDevicesDoCapture.cpp */
int PT_DECLSPEC sndDevicesDoCapture(PT_HANDLE *, float **, int *, WAVEFORMATEX **, int *, HANDLE);
int PT_DECLSPEC sndDevicesStartStopCapture(PT_HANDLE *, int);
int PT_DECLSPEC sndDevicesReleaseTimerPeriod(PT_HANDLE *);

/* sndDevicesDoPlayback.cpp */
int PT_DECLSPEC sndDevicesDoPlayback(PT_HANDLE *, int *);

/* sndDevicesReg.cpp */
int sndDevicesWriteToRegistry(PT_HANDLE *, int, wchar_t *, wchar_t *);
int sndDeviceReadFromRegistry(PT_HANDLE *, int, wchar_t *, wchar_t *);

#endif //_SND_DEVICES_H_