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

/*
 * FILE: u_sos.h
 * DATE: 7/2/96
 * AUTOHOR: Mark Kaplan
 * DESCRIPTION:
 *
 * Local header file for the sos coefficients module
 * This section type is of the normalized form
 *
 *                b0*Z**2 + b1*Z + b2
 *    H(Z) = g* ----------------------
 *                 Z**2 + a1*Z + a2
 */

#ifndef _U_SOS_H_
#define _U_SOS_H_

#include "slout.h"

#define SOS_FLOAT_BIAS 1.0e-30 // Was 1.0e-5 Thru 7/11/15, then changed to 1.0e-30 values that was being used in Hyperbass.
#define SOS_DCBLOCK_ALPHA 0.999	// See http://peabody.sapp.org/class/dmp2/lab/dcblock/ for freq response curves for differen values, 0.999 is good for 44.1 and 48khz.
#define SOS_VOLUME_LEVELING_HISTORY_SIZE 6
#define SOS_VOLUME_LEVELING_PEAK_WINDOW_SIZE 30

/* Section type definition */
struct sosSectionType {
   realtype b0;
   realtype b1;
   realtype b2;
   realtype a1;
   realtype a2;
   realtype a1_old;
   realtype a2_old;
	realtype state1;
	realtype state2;
	realtype state3;
	realtype state4;
	// Added for Surround Sound function
	realtype state_1[8];
	realtype state_2[8];
};   

/* BetterFxSound: coefficients of one section as used by the processing thread (see run_coeffs).
 * The processing kernels assume b1 == a1 (parametric form), so a1 is not needed here. */
struct sosRunCoeffType {
   realtype b0;
   realtype b1;
   realtype b2;
   realtype a2;
   int on;
};

/* Sos handle definition */
struct sosHdlType {
   /* Initialization info */
   CSlout *slout_hdl;
   char msg1[1024]; /* String for messages */

   int num_allocated_sections;  /* The number of sections allocated */
   int num_active_sections;     /* Active number of sections (must be <= num allocated) */
   int sos_response_valid[SOS_MAX_NUM_SOS_SECTIONS]; /* IS_TRUE if sos response, freq is current */ 
   realtype sos_center_freq[SOS_MAX_NUM_SOS_SECTIONS];/* Contains current center freq of section */
   int sos_center_freq_indexes[SOS_MAX_NUM_SOS_SECTIONS];/* Contains pixel index corresponding to center freq of section */
   realtype sos_center_freq_response[SOS_MAX_NUM_SOS_SECTIONS];/* Contains power response at center freq */
   realtype balance_left;       /* Externally applied balance for the combined sections */
   realtype balance_right;      /* Externally applied balance for the combined sections */
   realtype target_rms;         /* Externally applied normalization rms  for the combined sections */
   realtype normalization_gain; /* normalization gain */
   realtype volume_leveling_target_rms; /* Externally applied volume leveling rms for the combined sections */
   realtype volume_leveling_gain; /* volume leveling gain */
   realtype volume_leveling_power_history[SOS_VOLUME_LEVELING_HISTORY_SIZE]; /* Recent power history for volume leveling */
   realtype volume_leveling_power_sum; /* Running sum of recent power history */
   int volume_leveling_power_index; /* Ring buffer write index for recent power history */
   int volume_leveling_power_count; /* Number of valid entries in recent power history */
   realtype volume_leveling_previous_average_rms; /* Previous averaged rms used for gradient prediction */
   realtype volume_leveling_previous_predicted_rms; /* Previous predicted rms for miss detection */
   realtype volume_leveling_alpha_sample_rate; /* Sample rate used for cached detector coefficients */
   realtype volume_leveling_sc_hpf_alpha; /* Cached sidechain high-pass coefficient */
   realtype volume_leveling_tone_low_alpha; /* Cached low-band detector coefficient */
   realtype volume_leveling_tone_body_alpha; /* Cached body-band detector coefficient */
   realtype volume_leveling_tone_presence_alpha; /* Cached presence-band detector coefficient */
   realtype volume_leveling_sc_prev_in[8]; /* Sidechain HPF previous input per channel */
   realtype volume_leveling_sc_prev_out[8]; /* Sidechain HPF previous output per channel */
   realtype volume_leveling_tone_lp_state[8][3]; /* Lowpass states used to estimate muffled vs clear balance */
   realtype volume_leveling_tonality_score; /* -1.0 muffled ... +1.0 clear */
   realtype volume_leveling_headroom_score; /* +1.0 ceiling pressure ... -1.0 comfortable headroom */
   realtype volume_leveling_quiet_duration_seconds; /* How long the leveled output has continuously stayed in the very quiet region */
   realtype volume_leveling_quiet_gain_floor; /* Retained gain floor after quiet boost has made the output audible */
   realtype volume_leveling_quiet_peak_history[SOS_VOLUME_LEVELING_PEAK_WINDOW_SIZE]; /* One-second output peak buckets used for the 30-second headroom window */
   realtype volume_leveling_quiet_peak_bucket_max; /* Peak accumulated in the current partial one-second bucket */
   realtype volume_leveling_quiet_peak_bucket_seconds; /* Duration accumulated in the current partial one-second bucket */
   int volume_leveling_quiet_peak_history_index; /* Ring buffer write index for one-second output peak buckets */
   int volume_leveling_quiet_peak_history_count; /* Number of valid one-second output peak buckets */
   realtype master_gain; /* Externally applied master gain for the combined sections */
   int sos_type[SOS_MAX_NUM_SOS_SECTIONS]; /* SOS_PARA for parametric, SOS_SHELF for shelf type */
   struct sosSectionType *sections; 
   int section_on_flag[SOS_MAX_NUM_SOS_SECTIONS]; /* If IS_TRUE, then displayed gain is not 0 */

	/* BetterFxSound: the coefficients above are written by the parameter setters (UI, CLI and
	 * preset threads, serialized by DfxDspPrivate::param_lock_) while the processing thread runs
	 * the filters. Reading them directly could run a section with a mix of old and new b0..a2
	 * (a torn set can even be unstable) whenever a setter was preempted mid-update. So the
	 * processing code only uses run_coeffs, a copy the processing thread itself refreshes once
	 * per buffer through a sequence lock: sosSetSection() makes coeff_seq odd while it writes
	 * and even again when the section is complete, and the processing thread copies only an
	 * even, unchanged sequence (otherwise it keeps the previous set for one more buffer).
	 * Neither side ever waits. run_coeff_seq is the sequence the copy was taken at. */
	struct sosRunCoeffType run_coeffs[SOS_MAX_NUM_SOS_SECTIONS];
	volatile long coeff_seq;
	long run_coeff_seq;

	/* BetterFxSound: set by sosSetVolumeLeveling() when leveling is turned off; the processing
	 * thread clears the leveling history itself at the start of its next buffer instead of the
	 * setter zeroing it while the processing thread is updating it. */
	volatile long volume_leveling_reset_request;

	realtype in1_old, in2_old, outDC1_old, outDC2_old;	// Added for DC blocking filter.
	realtype in1_oldSS[8];
	realtype in2_oldSS[8];
	realtype outDC1_oldSS[8];
	realtype outDC2_oldSS[8];

};

#endif //_U_SOS_H_
