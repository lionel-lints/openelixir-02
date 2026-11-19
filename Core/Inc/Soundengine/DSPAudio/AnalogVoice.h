/*
 * AnalogVoice.h
 *
 * The Analog engine: a voice that plays one of the instruments of
 * AnalogModels.h (TR-808, TR-909, gamelan, hand drums, taiko) instead of
 * oscillators, through the drum voice's transient, filter, amp EG, velocity,
 * drive and volume. The amp decay sets how long the instrument rings (use the
 * amp EG as a gate: slope 127), coarse 0 is the instrument's original tuning.
 *
 * Part of a custom firmware for the Erica Synths LXR-02 (not an official
 * Erica Synths release). Added 2026.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Licensed under the GNU General Public License version 3, with the
 * additional permission and terms in NOTICE.md. See LICENSE and NOTICE.md in
 * the root of the repository.
 */

#ifndef ANALOGVOICE_H_
#define ANALOGVOICE_H_

#include "Soundengine/DSPAudio/VoiceCommon.h"
#include "Soundengine/DSPAudio/AnalogModels.h"

typedef struct
{
	VOICE_COMMON_FIELDS
	// engine specific
	AnalogOsc	inst;		//the instrument that sounds
	uint8_t		instrument;	//the instrument setting: played from the next hit on
	float		sat;		//tube saturator [0:1]
	float		lastGain;	//amp EG level of the last block (the level ramps from there)
	OnePoleFilter MixerPageFilter;	//volume smoothing, as the drum voice
	volatile uint8_t pending;	//triggered, the hit starts in the next block (Analog_calcAsync)
	uint8_t		pendingVel;
} AnalogVoice;

void Analog_init(AnalogVoice* v, uint8_t slot);
void Analog_trigger(AnalogVoice* v, const uint8_t vel, const uint8_t note);
void Analog_setInstrument(AnalogVoice* v, const uint8_t instrument);
void Analog_calcAsync(AnalogVoice* v);
void Analog_calcSyncBlock(AnalogVoice* v, int16_t* buf, const uint8_t size);

#endif /* ANALOGVOICE_H_ */
