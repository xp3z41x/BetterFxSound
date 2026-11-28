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

#include "codedefs.h"
#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

/*
 * BetterFxSound: the carrier's queue is held at its setpoint (half the capture buffer, the latency it always had)
 * by playing the audio very slightly faster or slower. The carrier runs on its own device's clock and the audio comes
 * from the FxSound virtual device's clock; with nothing to make up for the difference (tens of ppm) its queue slowly
 * ran empty, then it stopped and refilled (a dropout), or it grew until the loopback overflowed, and its latency moved
 * against the additional outputs meanwhile.
 * - SND_DEVICES_CARRIER_MAX_RATIO_ADJUST: largest speed change, +-0.2% (3.5 cents), far more than clocks differ.
 * - SND_DEVICES_CARRIER_CORRECTION_PER_SEC: share of the queue's (averaged) offset made up per second.
 * - SND_DEVICES_CARRIER_ERROR_AVG_SECS: averaging time of that offset.
 */
#define SND_DEVICES_CARRIER_MAX_RATIO_ADJUST		0.002
#define SND_DEVICES_CARRIER_CORRECTION_PER_SEC		0.1
#define SND_DEVICES_CARRIER_ERROR_AVG_SECS			0.2

/*
 * FUNCTION: sndDevicesCarrierFrame()
 * DESCRIPTION: BetterFxSound. Frame j (-3 to capturedFramesCount - 1) of the block being played on the carrier: from
 * -3 to -1 the last frames of the previous block.
 */
static const float *sndDevicesCarrierFrame(struct sndDevicesHdlType *cast_handle, int j, int numPlaybackChannels)
{
	if( j < 0 )
		return cast_handle->carrierHistory[3 + j];
	return cast_handle->fPlaybackBuf + (size_t)j * (size_t)numPlaybackChannels;
}

/*
 * FUNCTION: sndDevicesDoPlayback()
 * DESCRIPTION: Plays back audio buffers to specified playback device.
 * Plays buffer frames that have been queued up by the capture device.
 * BetterFxSound: the block (capturedFramesCount frames at the capture rate) is resampled to the playback rate with a
 * cubic (Hermite) interpolator, whose speed holds the carrier's queue at its setpoint (see above). With no speed change
 * and the same rate, the audio is passed through unchanged, two frames late. When the block would take the queue
 * past three quarters of the capture buffer (the carrier stalled or the processing thread came late), its oldest
 * frames are skipped so the queue is left at the setpoint: the carrier keeps its latency. Writing to a carrier that
 * sndDevicesDoCapture() stopped queues the audio until it starts again; a backlog written to it is cut to the setpoint.
 */
int PT_DECLSPEC sndDevicesDoPlayback(PT_HANDLE *hp_sndDevices, int *ip_resultFlag)
{
	struct sndDevicesHdlType *cast_handle;
	HRESULT hr;
	UINT32 numFramesQueuedUpToPlay;
	UINT32 numInputFrames;
	UINT32 numOutputFrames;
	UINT32 numDropFrames;
	UINT32 numKeepFrames;
	UINT32 space;
	UINT32 setpoint;
	UINT32 cap;
	UINT32 upsampleRatio;
	UINT32 k;
	int j, c;
	DWORD flags = 0;
	int numPlaybackChannels;
	double ratio, step, pos, endPos, adjust, errorSecs, alpha;
	float nextHistory[3][SND_DEVICES_CARRIER_MAX_CHANNELS];
	float *fptr;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	if( cast_handle->pAudioClientPlayback == NULL )
	{
		*ip_resultFlag = SND_DEVICES_NULL_PLAYBACK_CLIENT;
		SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_PLAYBACK_CLIENT)
	}

	numPlaybackChannels = cast_handle->wfxPlayback.nChannels;
	upsampleRatio = cast_handle->upsampleRatio;
	if( upsampleRatio < 1 )
		upsampleRatio = 1;
	numInputFrames = cast_handle->capturedFramesCount;

	// This call returns the number of frames still awaiting playback in the playback buffer
	hr = cast_handle->pAudioClientPlayback->GetCurrentPadding(&numFramesQueuedUpToPlay);
	if (FAILED(hr))
	{
		// BetterFxSound: reported, so the processing thread ends and a reinit runs. It used to see success and go on.
		*ip_resultFlag = SND_DEVICES_GET_PADDING_FAILED;
		SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_GET_PADDING_FAILED)
	}

	space = (cast_handle->bufferFrameSizePlayback > numFramesQueuedUpToPlay) ? cast_handle->bufferFrameSizePlayback - numFramesQueuedUpToPlay : 0;
	cast_handle->numPlaybackFramesAvailableToFill = space;

	// The setpoint and the most the queue may hold, in playback frames.
	setpoint = (cast_handle->bufferFrameSizeCapture / 2) * upsampleRatio;
	cap = setpoint + (cast_handle->bufferFrameSizeCapture / 4) * upsampleRatio;
	if( cap > cast_handle->bufferFrameSizePlayback )
		cap = cast_handle->bufferFrameSizePlayback;
	if( setpoint > cap )
		setpoint = cap;

	numOutputFrames = 0;
	if( (numInputFrames > 0) && (numPlaybackChannels > 0) && (numPlaybackChannels <= SND_DEVICES_CARRIER_MAX_CHANNELS) && (cast_handle->fPlaybackBuf != NULL) )
	{
		// Output frames per input frame: the rate ratio, times the speed that steers the queue to its setpoint. A
		// stopped carrier is being filled up to it, nothing to steer.
		ratio = 1.0;
		if( cast_handle->playbackStreamIsTemporarilyPaused == 0 )
		{
			adjust = cast_handle->carrierErrorAvg * SND_DEVICES_CARRIER_CORRECTION_PER_SEC;
			if( adjust > SND_DEVICES_CARRIER_MAX_RATIO_ADJUST )
				adjust = SND_DEVICES_CARRIER_MAX_RATIO_ADJUST;
			if( adjust < -SND_DEVICES_CARRIER_MAX_RATIO_ADJUST )
				adjust = -SND_DEVICES_CARRIER_MAX_RATIO_ADJUST;
			ratio = 1.0 - adjust;
		}
		step = 1.0 / ((double)upsampleRatio * ratio);

		// Output frame k is taken at input position pos + k * step, from the frames around it (floor - 1 to floor + 2),
		// so up to the position numInputFrames - 2. "!(pos >= -2)" is also true for NaN.
		pos = cast_handle->carrierReadPos;
		if( !(pos >= -2.0) )
			pos = -2.0;
		endPos = (double)numInputFrames - 2.0;
		if( endPos > pos )
			numOutputFrames = (UINT32)ceil((endPos - pos) / step);

		// Too much for the queue: only the newest audio, up to the setpoint. A stopped carrier given a backlog (it ran
		// empty because the processing thread came late, which then read everything at once) is cut to the setpoint
		// too, not just to the cap: it starts as soon as it holds its setpoint, so anything above it would be extra
		// latency against the additional outputs, left for the resampler to work off over tens of seconds. Ordinary
		// blocks (one loopback packet, far less than half the setpoint) filling a stopped carrier keep the cap, so a
		// sound starting after silence is not cut into.
		numDropFrames = 0;
		if( (numFramesQueuedUpToPlay + numOutputFrames > cap) || (numOutputFrames > space) ||
			((cast_handle->playbackStreamIsTemporarilyPaused != 0) && (numOutputFrames > setpoint / 2) && (numFramesQueuedUpToPlay + numOutputFrames > setpoint)) )
		{
			numKeepFrames = (numFramesQueuedUpToPlay < setpoint) ? setpoint - numFramesQueuedUpToPlay : 0;
			if( numKeepFrames > space )
				numKeepFrames = space;
			if( numKeepFrames < numOutputFrames )
				numDropFrames = numOutputFrames - numKeepFrames;
			pos += (double)numDropFrames * step;
			numOutputFrames -= numDropFrames;
			cast_handle->carrierErrorAvg = 0.0;
		}

		if( numOutputFrames > 0 )
		{
			hr = cast_handle->pAudioClientPlaybackRender->GetBuffer(numOutputFrames, &(cast_handle->pDataPacketPlayback));
			if (FAILED(hr))
			{
				// BetterFxSound: reported, so the processing thread ends and a reinit runs. It used to see success and go on.
				*ip_resultFlag = SND_DEVICES_PLAYBACK_RENDER_FAILED;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_PLAYBACK_RENDER_FAILED)
			}

			if( cast_handle->pDataPacketPlayback != NULL )
			{
				fptr = (float *)(cast_handle->pDataPacketPlayback);

				for(k=0; k<numOutputFrames; k++)
				{
					const double p = pos + (double)k * step;
					int base = (int)floor(p);
					if( base > (int)numInputFrames - 3 )
						base = (int)numInputFrames - 3;
					if( base < -2 )
						base = -2;
					float t = (float)(p - (double)base);
					if( t < 0.0f )
						t = 0.0f;
					if( t > 1.0f )
						t = 1.0f;

					const float *fm1 = sndDevicesCarrierFrame(cast_handle, base - 1, numPlaybackChannels);
					const float *f0 = sndDevicesCarrierFrame(cast_handle, base, numPlaybackChannels);
					const float *f1 = sndDevicesCarrierFrame(cast_handle, base + 1, numPlaybackChannels);
					const float *f2 = sndDevicesCarrierFrame(cast_handle, base + 2, numPlaybackChannels);

					for(c=0; c<numPlaybackChannels; c++)
					{
						const float xm1 = fm1[c];
						const float x0 = f0[c];
						const float x1 = f1[c];
						const float x2 = f2[c];
						const float c1 = 0.5f * (x1 - xm1);
						const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
						const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
						fptr[c] = ((c3 * t + c2) * t + c1) * t + x0;
					}
					fptr += numPlaybackChannels;
				}
			}

			hr = cast_handle->pAudioClientPlaybackRender->ReleaseBuffer(numOutputFrames, flags);
			if (FAILED(hr))
			{
				// BetterFxSound: reported, so the processing thread ends and a reinit runs. It used to see success and go on.
				*ip_resultFlag = SND_DEVICES_RELEASE_BUFFER_FAILED;
				SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_RELEASE_BUFFER_FAILED)
			}
		}

		// The next block goes on where this one ends, its last three frames are kept for it.
		pos += (double)numOutputFrames * step - (double)numInputFrames;
		if( !(pos >= -2.0) )
			pos = -2.0;
		cast_handle->carrierReadPos = pos;

		for(j=0; j<3; j++)
			memcpy(nextHistory[j], sndDevicesCarrierFrame(cast_handle, (int)numInputFrames - 3 + j, numPlaybackChannels), sizeof(float) * (size_t)numPlaybackChannels);
		for(j=0; j<3; j++)
			memcpy(cast_handle->carrierHistory[j], nextHistory[j], sizeof(float) * (size_t)numPlaybackChannels);

		// The queue's offset from the setpoint right after the write, averaged over SND_DEVICES_CARRIER_ERROR_AVG_SECS
		// whatever the block size.
		if( (cast_handle->playbackStreamIsTemporarilyPaused == 0) && (numDropFrames == 0) &&
			(cast_handle->wfxPlayback.nSamplesPerSec > 0) && (cast_handle->wfxCapture.nSamplesPerSec > 0) )
		{
			errorSecs = ((double)(numFramesQueuedUpToPlay + numOutputFrames) - (double)setpoint) / (double)cast_handle->wfxPlayback.nSamplesPerSec;
			alpha = (double)numInputFrames / ((double)cast_handle->wfxCapture.nSamplesPerSec * SND_DEVICES_CARRIER_ERROR_AVG_SECS);
			if( alpha > 1.0 )
				alpha = 1.0;
			cast_handle->carrierErrorAvg += (errorSecs - cast_handle->carrierErrorAvg) * alpha;
		}
	}

	// BetterFxSound: the carrier's queue right after this write (see the watchdog in sndDevicesDoCapture()).
	cast_handle->carrierLastPadding = numFramesQueuedUpToPlay + numOutputFrames;
	cast_handle->playbackFrameCount = numOutputFrames;

	*ip_resultFlag = SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS;

	return(OKAY);
}
