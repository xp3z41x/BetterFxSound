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
#include "codedefs.h"

/* Standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <Propvarutil.h>
#include <Avrt.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

/*
 * BetterFxSound: carrier pacing.
 * - SND_DEVICES_CARRIER_STALL_MS: a running carrier that plays nothing out of its queue for this long while audio keeps
 *   coming ends the processing thread, so a reinit opens it again. Doubled for each consecutive one.
 * - SND_DEVICES_CARRIER_STALL_MAX_REINITS: consecutive stalls the watchdog acts on. After 2, 4 and 8 s the carrier is
 *   reinitialized; after the next one (16 s) the watchdog gives up instead, since each reinit interrupts the additional
 *   outputs too, which play on while the carrier stalls. It is armed again by a
 *   device notification or a device change (anything that stops the processing thread from outside), or once the
 *   carrier plays again for SND_DEVICES_CARRIER_STALL_RESET_MS.
 * - SND_DEVICES_CARRIER_POLL_GAP_MS: polls further apart than this (the processing thread was held off: a resume from
 *   sleep, a debugger) say nothing about the carrier, the stall is measured again from the next one.
 */
#define SND_DEVICES_CARRIER_STALL_MS			2000
#define SND_DEVICES_CARRIER_STALL_MAX_REINITS	4
#define SND_DEVICES_CARRIER_STALL_RESET_MS		10000
#define SND_DEVICES_CARRIER_POLL_GAP_MS			200

/*
 * BetterFxSound: capture polling (sndDevicesDoCapture()). Every millisecond while audio flows; every
 * SND_DEVICES_IDLE_POLL_MS once the carrier is stopped with nothing queued and no packet came for
 * SND_DEVICES_IDLE_AFTER_MS.
 */
#define SND_DEVICES_IDLE_AFTER_MS				100
#define SND_DEVICES_IDLE_POLL_MS				10

/*
 * FUNCTION: sndDevicesResetCarrierResampler()
 * DESCRIPTION: BetterFxSound. Starts the carrier's resampler (sndDevicesDoPlayback()) over from silence.
 */
static void sndDevicesResetCarrierResampler(struct sndDevicesHdlType *cast_handle)
{
	memset(cast_handle->carrierHistory, 0, sizeof(cast_handle->carrierHistory));
	cast_handle->carrierReadPos = -2.0;
	cast_handle->carrierErrorAvg = 0.0;
}

/*
 * FUNCTION: sndDevicesReleaseTimerPeriod()
 * DESCRIPTION: BetterFxSound. Ends the 1 ms timer resolution sndDevicesDoCapture() raised while polling without a high
 * resolution waitable timer, if it did. Called from the processing thread, also when it ends.
 */
int PT_DECLSPEC sndDevicesReleaseTimerPeriod(PT_HANDLE *hp_sndDevices)
{
	struct sndDevicesHdlType *cast_handle;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	if( cast_handle->timerPeriodRaised != 0 )
	{
		timeEndPeriod(1);
		cast_handle->timerPeriodRaised = 0;
	}

	return(OKAY);
}

/*
 * FUNCTION: sndDevicesDoCapture()
 * DESCRIPTION: Captures audio buffers from specified capture device.
 * Returns fp_buffer to use to do audio processing in place.
 * *ip_numSampleSets will contain the number of sample sets in the buffer
 * and **pp_wfxDfx contains the format of the buffer.
 * BetterFxSound: h_waitTimer is a waitable timer the calling thread owns, used to wait between polls; NULL waits
 * with Sleep() and the timer resolution raised while audio flows (the caller ends it with
 * sndDevicesReleaseTimerPeriod()).
 * Every loopback packet there is is read on each poll and handed over at once, whatever the carrier has queued. The
 * carrier used to pace the capture: only what it needed to get back to half the buffer was read, so the additional
 * outputs (fed with the same blocks) got nothing while it refilled from empty or stalled, a sound shorter than half
 * the buffer after silence was held until the next one, and a backlog left in the loopback by a late processing
 * thread stayed there for good. The carrier now keeps its own queue at its setpoint (sndDevicesDoPlayback()): it is
 * stopped when it runs empty and started again once the setpoint is queued.
 */
int PT_DECLSPEC sndDevicesDoCapture(PT_HANDLE *hp_sndDevices, float **fpp_buffer, int *ip_numSampleSets, WAVEFORMATEX **pp_wfxDfx, int *ip_resultFlag, HANDLE h_waitTimer)
{
	struct sndDevicesHdlType *cast_handle;
	HRESULT hr;
	UINT32 packetLength = 0;
	UINT32 numFramesQueuedUpToPlay;
	UINT32 numFramesQueuedUpToCapture;
	UINT32 numFramesQueuedUpToPlayReferencedToCapture; // Corrected for samp rate difference.
	DWORD flags;
	float *fptr;
	UINT32 loopsize, index, offset;
	UINT32 i;
	int numCaptureChannels;
	int numPlaybackChannels;
	UINT32 halfCaptureBufferSize;
	UINT32 maxCaptureFrames;
	UINT32 packetFrames;
	DWORD now;
	int i_playback_index;
#ifdef _DEBUG
	UINT64 DevicePosition;
	UINT64 QPCPosition;
#endif

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	if(cast_handle->pAudioClientCapture == NULL) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_CAPTURE_CLIENT)
	if(cast_handle->pAudioClientPlayback == NULL) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_PLAYBACK_CLIENT)
	if(cast_handle->pAudioCaptureLoopback == NULL) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_LOOPBACK_CLIENT)

	cast_handle->capturedFramesCount = 0; // Probably want to maintain and not zero this value.

	numCaptureChannels = cast_handle->wfxCapture.nChannels;
	numPlaybackChannels = cast_handle->wfxPlayback.nChannels;
	*fpp_buffer = NULL;
	*ip_numSampleSets = 0;
	*pp_wfxDfx = NULL;

	// BetterFxSound: the most frames one call can hand over: what the capture buffer holds, and what the processing
	// buffer holds at the playback channel count. A larger backlog is read by the next call.
	maxCaptureFrames = 0;
	if( (numCaptureChannels > 0) && (numPlaybackChannels > 0) && (cast_handle->captureBufAllocSize > 0) && (cast_handle->playbackBufAllocSize > 0) )
	{
		UINT32 maxPlaybackFrames = (UINT32)(cast_handle->playbackBufAllocSize / numPlaybackChannels);
		maxCaptureFrames = (UINT32)(cast_handle->captureBufAllocSize / numCaptureChannels);
		if( maxPlaybackFrames < maxCaptureFrames )
			maxCaptureFrames = maxPlaybackFrames;
	}
	if( maxCaptureFrames == 0 )
	{
		hr = E_FAIL;
		goto Exit;
	}

	// Repeat this loop until there are captured frames to hand over.
	do
	{
		// If the device callbacks or an external call has thrown the stop flag, exit this thread.
		if( cast_handle->stopAudioCaptureAndPlaybackLoop == 1 )
		{
			// BetterFxSound: a device notification or change, the carrier watchdog is armed again.
			cast_handle->carrierStallCount = 0;
			*ip_numSampleSets = 0;
			*ip_resultFlag = SND_DEVICES_CAPTURE_FORCED_EXIT;
			return(OKAY);
		}

		// BetterFxSound: poll every millisecond while audio flows. Sleep(1) lasts a whole timer tick (up to 15.6 ms)
		// unless the timer resolution is raised, which Windows 11 may ignore for a hidden window, so how soon a
		// packet was seen depended on the environment. A high resolution timer (Windows 10 1803 and later) does
		// not; without it the timer resolution is raised to 1 ms here. Once idle (the carrier stopped with nothing
		// queued and no packet for SND_DEVICES_IDLE_AFTER_MS) it polls every SND_DEVICES_IDLE_POLL_MS instead, with
		// the timer resolution back to normal, so an idle FxSound lets the CPU sleep; the first packet after
		// silence is seen at most that much later, which the carrier start below and the additional outputs absorb.
		{
			LARGE_INTEGER qpcPoll;
			BOOL b_idle = FALSE;
			DWORD waitMs;
			QueryPerformanceCounter(&qpcPoll);
			if( (cast_handle->playbackStreamIsTemporarilyPaused != 0) && (cast_handle->carrierLastPadding == 0) &&
				(cast_handle->qpcFrequency > 0) &&
				((qpcPoll.QuadPart - cast_handle->lastCapturePacketQpc) > (cast_handle->qpcFrequency * SND_DEVICES_IDLE_AFTER_MS) / 1000) )
				b_idle = TRUE;
			waitMs = b_idle ? SND_DEVICES_IDLE_POLL_MS : 1;

			if( h_waitTimer != NULL )
			{
				LARGE_INTEGER dueTime;
				dueTime.QuadPart = -10000LL * (LONGLONG)waitMs; // relative, in 100 ns units
				if( SetWaitableTimer(h_waitTimer, &dueTime, 0, NULL, NULL, FALSE) )
					WaitForSingleObject(h_waitTimer, waitMs + 20);
				else
					Sleep(waitMs);
			}
			else
			{
				if( b_idle )
					sndDevicesReleaseTimerPeriod(hp_sndDevices);
				else if( cast_handle->timerPeriodRaised == 0 )
					cast_handle->timerPeriodRaised = (timeBeginPeriod(1) == TIMERR_NOERROR) ? 1 : 0;
				Sleep(waitMs);
			}
		}

		// Check for bad value to upsample ratio, was happening when some Bluetooth devices were added or removed (11/25/15).
		if( (cast_handle->upsampleRatio <= 0) || (cast_handle->upsampleRatio > 16) )
		{/*
			*ip_numSampleSets = 0;
			*ip_resultFlag = SND_DEVICES_CAPTURE_ERROR;
			return(OKAY); */

			SLOUT_FIRST_LINE(L"sndDevicesDoCapture():: upsampleRatio is out of range");
			cast_handle->upsampleRatio = 1;

		}

		hr = cast_handle->pAudioClientCapture->GetCurrentPadding(&numFramesQueuedUpToCapture);
		if (FAILED(hr)) goto Exit;

		// This call returns the number of frames still awaiting playback in the playback buffer
		hr = cast_handle->pAudioClientPlayback->GetCurrentPadding(&numFramesQueuedUpToPlay);
		if (FAILED(hr)) goto Exit;

		numFramesQueuedUpToPlayReferencedToCapture = numFramesQueuedUpToPlay/cast_handle->upsampleRatio;

		halfCaptureBufferSize = cast_handle->bufferFrameSizeCapture/2;
		now = GetTickCount();

		// BetterFxSound: the carrier's stall is only measured across polls that ran (see SND_DEVICES_CARRIER_POLL_GAP_MS).
		if( (now - cast_handle->carrierLastPollTick) > SND_DEVICES_CARRIER_POLL_GAP_MS )
		{
			cast_handle->carrierLastConsumedTick = now;
			cast_handle->carrierStallCapturedFrames = 0;
		}
		cast_handle->carrierLastPollTick = now;

		// If playback buffer is totally empty, stop it until it has 1/2 the capture buffer queued again.
		if( numFramesQueuedUpToPlay == 0 )
		{
			cast_handle->playbackIsActive = SND_DEVICES_PLAYBACK_IS_STOPPED;

			if( cast_handle->playbackStreamIsTemporarilyPaused == 0 )
			{
				cast_handle->playbackStreamIsTemporarilyPaused = 1;
				hr = cast_handle->pAudioClientPlayback->Stop(); // Stop playback to allow PC to sleep if no audio is playing.
				if (FAILED(hr)) goto Exit;

				// BetterFxSound: everything was played, the next audio starts from silence.
				sndDevicesResetCarrierResampler(cast_handle);
			}
		}
		else if( cast_handle->playbackStreamIsTemporarilyPaused )
		{
			// BetterFxSound: the blocks handed over meanwhile were queued on the stopped carrier
			// (sndDevicesDoPlayback()). It starts once its setpoint is queued, or, when no more audio came, at the
			// moment more audio would have brought it there: a sound shorter than the setpoint after silence is
			// played with the usual latency, instead of being held until the next sound fills the rest.
			LARGE_INTEGER qpcNow;
			double waitedFrames = 0.0;
			QueryPerformanceCounter(&qpcNow);
			if( (cast_handle->qpcFrequency > 0) && (qpcNow.QuadPart > cast_handle->lastCapturePacketQpc) )
				waitedFrames = (double)(qpcNow.QuadPart - cast_handle->lastCapturePacketQpc) * (double)cast_handle->wfxCapture.nSamplesPerSec / (double)cast_handle->qpcFrequency;

			if( (double)numFramesQueuedUpToPlayReferencedToCapture + waitedFrames >= (double)halfCaptureBufferSize )
			{
				cast_handle->playbackStreamIsTemporarilyPaused = 0;
				hr = cast_handle->pAudioClientPlayback->Start(); // Restart playback.
				if (FAILED(hr)) goto Exit;

				cast_handle->playbackIsActive = SND_DEVICES_PLAYBACK_IS_ACTIVE;
				cast_handle->carrierErrorAvg = 0.0;
				cast_handle->carrierLastConsumedTick = now;
				cast_handle->carrierStallCapturedFrames = 0;
			}
		}
		else
		{
			cast_handle->playbackIsActive = SND_DEVICES_PLAYBACK_IS_ACTIVE;

			// BetterFxSound: watchdog. The additional outputs no longer depend on the carrier, but a carrier that stays
			// active without playing anything out of its queue and without an error (a display sink in power save, a
			// device half awake after a resume) would stay silent until some device notification. When audio keeps
			// coming and the queue has not gone down for a while, the processing thread ends and a reinit opens the
			// carrier again; a carrier that keeps stalling is given longer each time, and after
			// SND_DEVICES_CARRIER_STALL_MAX_REINITS it is left alone (its blocks are dropped, the additional outputs
			// play on) until a device notification or change.
			if( numFramesQueuedUpToPlay < cast_handle->carrierLastPadding )
			{
				cast_handle->carrierLastConsumedTick = now;
				cast_handle->carrierStallCapturedFrames = 0;
				if( (now - cast_handle->processingStartTick) >= SND_DEVICES_CARRIER_STALL_RESET_MS )
					cast_handle->carrierStallCount = 0;
			}
			else if( (cast_handle->carrierStallCapturedFrames > 0) && (cast_handle->carrierStallCount < SND_DEVICES_CARRIER_STALL_MAX_REINITS) )
			{
				if( (now - cast_handle->carrierLastConsumedTick) >= ((DWORD)SND_DEVICES_CARRIER_STALL_MS << cast_handle->carrierStallCount) )
				{
					cast_handle->carrierStallCount++;
					if( cast_handle->carrierStallCount < SND_DEVICES_CARRIER_STALL_MAX_REINITS )
					{
						SLOUT_FIRST_LINE(L"sndDevicesDoCapture():: the playback device stopped playing, reinitializing");
						hr = E_FAIL;
						goto Exit;
					}
					SLOUT_FIRST_LINE(L"sndDevicesDoCapture():: the playback device keeps stalling, waiting for a device change");
				}
			}
		}
		cast_handle->carrierLastPadding = numFramesQueuedUpToPlay;

		// The audio is streamed in small "packets", a number of frames (sample sets). So far it appears that
		// packets are always 10ms in duration, independent of sampling freq, so at 44.1k we get 441 sample sets/packet.
		hr = cast_handle->pAudioCaptureLoopback->GetNextPacketSize(&packetLength); // The number of frames in the next packet.
		if (FAILED(hr)) goto Exit;

		// BetterFxSound: every packet there is, up to what the buffers hold. A backlog (the processing thread came
		// late) is read at once too; the carrier keeps only the newest of it (sndDevicesDoPlayback()), the additional
		// outputs line themselves up again. The loopback buffer is never left to overflow, so a discontinuity
		// (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) needs nothing more.
		while( (packetLength > 0) && ((cast_handle->capturedFramesCount == 0) || (cast_handle->capturedFramesCount + packetLength <= maxCaptureFrames)) )
		{
			// Get the data in this packet.
			hr = cast_handle->pAudioCaptureLoopback->GetBuffer(&(cast_handle->pDataPacketCapture), &(cast_handle->numCaptureFramesAvailable), &flags,
#ifdef _DEBUG
				&DevicePosition, &QPCPosition);
#else
				NULL, NULL);
#endif
			if (FAILED(hr)) goto Exit;

			fptr = (float *)(cast_handle->pDataPacketCapture);

			// BetterFxSound: never more than the buffer holds (a first packet larger than that is cut, the rest dropped).
			packetFrames = cast_handle->numCaptureFramesAvailable;
			if( packetFrames > maxCaptureFrames - cast_handle->capturedFramesCount )
				packetFrames = maxCaptureFrames - cast_handle->capturedFramesCount;

			loopsize = packetFrames * numCaptureChannels;
			offset = cast_handle->capturedFramesCount * numCaptureChannels;

			// Silent flag means to treat packet as containing all zeros, even though it may not.
			if( flags & AUDCLNT_BUFFERFLAGS_SILENT )
			{
				cast_handle->playbackIsActive = SND_DEVICES_PLAYBACK_IS_STOPPED;
				for(i=0; i<loopsize; i++)
					cast_handle->fCaptureBuf[ offset + i ] = (float)0.0;
			}
			else
			{
				for(i=0; i<loopsize; i++)
					cast_handle->fCaptureBuf[ offset + i ] = fptr[i];
			}

// #define DO_BEEPS_ON_SONG_CHANGES
#ifdef DO_BEEPS_ON_SONG_CHANGES
			// The discontinuity flag gets set by WMP when clicking on different song selections but not on start/stop.
			// In youtube the flag doesn't get set by start/stop or playing songs but does get set when capture
			// buffer overflows, as does WMP.
			static int discontinuity_count = 0;
			static int silent_count = 0;
			static int timestamp_error_count = 0;
			int event_flag;

			event_flag = 1;

			for(i=0; i<loopsize; i++ )
				if( fabs(cast_handle->fCaptureBuf[ offset + i ]) > 0.001 )
					event_flag = 0;

			if( flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY )
			{
 				discontinuity_count++;
				event_flag = 1;
			}
			if( flags & AUDCLNT_BUFFERFLAGS_SILENT )
			{
				silent_count++;
				event_flag = 1;
			}
			if( flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR )
			{
				timestamp_error_count++;
				event_flag = 1;
			}

			if( event_flag )
				for(i=0; i<loopsize; i++)
					if( (i % 100) == 0 )
						cast_handle->fCaptureBuf[ offset + i ] = (float)0.95;
					else
						cast_handle->fCaptureBuf[ offset + i ] = (float)0.0;
#endif

			// This release call is to be called as soon as possible following the GetBuffer call.
			hr = cast_handle->pAudioCaptureLoopback->ReleaseBuffer(cast_handle->numCaptureFramesAvailable);
			if (FAILED(hr)) goto Exit;

			cast_handle->capturedFramesCount += packetFrames;

			hr = cast_handle->pAudioCaptureLoopback->GetNextPacketSize(&packetLength);
			if (FAILED(hr)) goto Exit;
		}

		if( cast_handle->capturedFramesCount > 0 )
		{
			LARGE_INTEGER qpcPacket;
			QueryPerformanceCounter(&qpcPacket);
			cast_handle->lastCapturePacketQpc = qpcPacket.QuadPart;

			// BetterFxSound: audio the running carrier was given since it last played something (see the watchdog above).
			if( cast_handle->playbackStreamIsTemporarilyPaused == 0 )
				cast_handle->carrierStallCapturedFrames += cast_handle->capturedFramesCount;
		}

	} while( cast_handle->capturedFramesCount == 0 ); // This loop has a sleep to allow capture buffers to build up.

	hr = S_OK;

	// Correct number of playback frame for sample rate difference.
	cast_handle->playbackFrameCount = cast_handle->upsampleRatio * cast_handle->capturedFramesCount;

	// We should now have enough audio data to fill playback buffer
	if( cast_handle->capturedFramesCount > 0 )
	{
		loopsize = cast_handle->capturedFramesCount * numCaptureChannels;

		// If channels don't match, correctly fill playback buffer.
		//Ordering for 5.1 is: Front Left, Front Right, Front Center, Low Frequency, Back Left, Back Right
		//Ordering for 7.1 is: Front Left, Front Right, Front Center, Low Frequency, Back Left, Back Right, Side Left, Side Right
		// With current implementation, the only case we hit here should be 6 channel capture, 8 channel playback.
		// Note, with added 5.1 and 7.1 support we shouldn't ever hit this case.
		if( numCaptureChannels != numPlaybackChannels )
		{
			if( numCaptureChannels == 6 )
			{
				// For now we will always assume capture device has 6 channels.
				index = 0;
				if( numPlaybackChannels == 2 ) // Stereo
				{
					for(i=0; i<loopsize; i += numCaptureChannels) // Index through frames (sample sets)
					{
							cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i];
							index++;
							cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 1];
							index++;
					}
				}
				else if( numPlaybackChannels == 4 )	// Quad
				{
					for(i=0; i<loopsize; i += numCaptureChannels) // Index through frames (sample sets)
					{
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 1];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 4];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 5];
						index++;
					}
				}
				else if( numPlaybackChannels == 8 )	// 7.1 (eight channel)
				{
					for(i=0; i<loopsize; i += numCaptureChannels) // Index through frames (sample sets)
					{
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 1];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 2];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 3];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 4];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 5];
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 4]; // Fill side channels with back channels
						index++;
						cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 5];
						index++;
					}
				}
			} // End of numCaptureChannels == 6
			else if( (numPlaybackChannels == 4) && (numCaptureChannels == 2) )
			{
				index = 0;
				for(i=0; i<loopsize; i += numCaptureChannels) // Index through frames (sample sets)
				{
					cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i];
					index++;
					cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 1];
					index++;
					cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i];
					index++;
					cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i + 1];
					index++;
				}
			}
			// Since we are not able to playback to mono devices correctly (bluetooth problem) we simply fill the playback buffer with zeros
			else if (numPlaybackChannels == 1)
			{
				if (SND_DEVICES_MONO_BUG_FORCE_SILENCE)
				{
					for (i_playback_index = 0; i_playback_index < cast_handle->playbackBufAllocSize; i_playback_index++)
					{
						cast_handle->fPlaybackBuf[i_playback_index] = (realtype)0.0;
					}
				} else
				{
					// TODO: Handle mono playback correctly. The code below doesn't work well.
					if (numCaptureChannels == 2)
					{
						index = 0;
						for (i = 0; i<loopsize; i += numCaptureChannels) // Index through frames (sample sets)
						{
							cast_handle->fPlaybackBuf[index] = (cast_handle->fCaptureBuf[i] + cast_handle->fCaptureBuf[i + 1]) / (realtype)2.0;
							index++;
						}
					}
				}
			}
		}
		else	// Channel sizes match, just do a straight copy.
		{
			for(i=0; i<loopsize; i++)
				cast_handle->fPlaybackBuf[i] = cast_handle->fCaptureBuf[i];
		}
	}

	// We now exit the capture loop with the playback buffers ready for processing
	// They are at 44.1 or 48khz but at the playback devices channel format.
	*fpp_buffer = cast_handle->fPlaybackBuf;

	*ip_numSampleSets = cast_handle->capturedFramesCount;

	*pp_wfxDfx = &(cast_handle->wfxDfxProcessing);

Exit:
	if(hr == S_OK)
		*ip_resultFlag = SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS;
	else
		*ip_resultFlag = SND_DEVICES_CAPTURE_ERROR;

	return(OKAY);
}

/*
 * FUNCTION: sndDevicesStartStopCapture()
 * DESCRIPTION: Allows starting or stopping audio capture. Note that after capture stops, existing buffers
 * that remain queued up for the playback buffer will playout and then playback will stop also.
 */
int PT_DECLSPEC sndDevicesStartStopCapture(PT_HANDLE *hp_sndDevices, int i_startStopFlag)
{
	struct sndDevicesHdlType *cast_handle;
	HRESULT hr;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	if(cast_handle->pAudioClientCapture == NULL) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_CAPTURE_CLIENT)
	if(cast_handle->pAudioClientPlayback == NULL) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_PLAYBACK_CLIENT)

	if( i_startStopFlag == SND_DEVICES_START_CAPTURE )
	{
		// BetterFxSound: the stop flag is not cleared here. sndDevicesReInit() cleared it before it enumerated the
		// devices, and a stop requested since then (a device change, a carrier switch) must make this new
		// processing thread exit at once so another reinit picks it up. Clearing it here lost those requests.

		// BetterFxSound: the carrier pacing starts over with this processing thread (see sndDevicesDoCapture()).
		LARGE_INTEGER qpc;
		cast_handle->processingStartTick = GetTickCount();
		QueryPerformanceFrequency(&qpc);
		cast_handle->qpcFrequency = qpc.QuadPart;
		QueryPerformanceCounter(&qpc);
		cast_handle->lastCapturePacketQpc = qpc.QuadPart;
		cast_handle->carrierLastConsumedTick = cast_handle->processingStartTick;
		cast_handle->carrierLastPollTick = cast_handle->processingStartTick;
		cast_handle->carrierLastPadding = 0;
		cast_handle->carrierStallCapturedFrames = 0;
		sndDevicesResetCarrierResampler(cast_handle);

		hr = cast_handle->pAudioClientCapture->Start(); // Start capturing, this starts filling of the internal capture buffers.
		hr = cast_handle->pAudioClientPlayback->Start(); // Start playback, this starts filling of the internal playback buffers.
	}
	else
	{
		hr = cast_handle->pAudioClientCapture->Stop();  // Stop capturing.
		hr = cast_handle->pAudioClientPlayback->Stop(); // Stop playback.
		InterlockedExchange(&(cast_handle->stopAudioCaptureAndPlaybackLoop), 1);
	}

	return(OKAY);
}
