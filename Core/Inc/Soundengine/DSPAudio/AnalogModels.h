/*
 * AnalogModels.h
 *
 * The instruments of the TR-808 and TR-909, modelled after their circuits,
 * and acoustic instruments (gamelan, hand drums, taiko) modelled after their
 * modes of vibration. Each instrument makes its own pitch sweeps, clicks,
 * noise and envelopes; the voice's amp decay sets how long it rings.
 *
 * From AnalogModels 0.5 by G3nzalo (github.com/G3nzalo/analogmodels-firmware),
 * where they are extra oscillator waveforms. Here they are played by the
 * Analog engine (AnalogVoice.h): numbered from 0, with their state in an
 * AnalogOsc instead of the oscillator.
 *
 * Part of a custom firmware for the Erica Synths LXR-02 (not an official
 * Erica Synths release). Added 2026.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Licensed under the GNU General Public License version 3, with the
 * additional permission and terms in NOTICE.md. See LICENSE and NOTICE.md in
 * the root of the repository.
 */

#ifndef ANALOGMODELS_H_
#define ANALOGMODELS_H_

#include <stdint.h>
#include "Soundengine/DSPAudio/AnalogModelState.h"

//the order is part of the kit format (the instrument is stored as its number): only append
enum AnalogModelEnum
{
	MODEL_BD8 = 0,	/* 808 bass drum */
	MODEL_BD9,		/* 909 bass drum */
	MODEL_SD8,		/* 808 snare */
	MODEL_SD9,		/* 909 snare */
	MODEL_LT8,		/* 808 low tom */
	MODEL_MT8,		/* 808 mid tom */
	MODEL_HT8,		/* 808 high tom */
	MODEL_LT9,		/* 909 low tom */
	MODEL_MT9,		/* 909 mid tom */
	MODEL_HT9,		/* 909 high tom */
	MODEL_LC8,		/* 808 low conga */
	MODEL_MC8,		/* 808 mid conga */
	MODEL_HC8,		/* 808 high conga */
	MODEL_RS8,		/* 808 rim shot */
	MODEL_RS9,		/* 909 rim shot */
	MODEL_CL8,		/* 808 claves */
	MODEL_CP8,		/* 808 hand clap */
	MODEL_CP9,		/* 909 hand clap */
	MODEL_MA8,		/* 808 maracas */
	MODEL_CB8,		/* 808 cowbell */
	MODEL_HH8,		/* 808 hi-hat (closed or open by the decay) */
	MODEL_HH9,		/* 909 hi-hat */
	MODEL_CY8,		/* 808 cymbal */
	MODEL_CR9,		/* 909 crash */
	MODEL_RD9,		/* 909 ride */
	MODEL_SRN,		/* gamelan saron */
	MODEL_GND,		/* gamelan gender, a pair with ombak */
	MODEL_BNG,		/* gamelan bonang */
	MODEL_GNG,		/* gamelan gong ageng */
	MODEL_DJB,		/* djembe bass */
	MODEL_DJT,		/* djembe tone */
	MODEL_DJS,		/* djembe slap */
	MODEL_CGO,		/* conga open tone */
	MODEL_CGS,		/* conga slap */
	MODEL_ODK,		/* taiko: odaiko */
	MODEL_NGD,		/* taiko: nagado-daiko */
	MODEL_SHM,		/* taiko: shime-daiko */
	MODEL_TKR,		/* taiko: the bachi on the rim */
	MODEL_END
};

#define NUM_ANALOG_MODELS	(MODEL_END - MODEL_BD8)

//what an instrument needs from its voice (in AnalogModels: fields of the oscillator)
typedef struct
{
	uint8_t		waveform;	//the instrument (enum AnalogModelEnum)
	uint32_t	phaseInc;	//pitch: phase increment of a 32 bit phase per sample, as the sine oscillator's
	float		modelEnv;	//current amp EG level of the voice, set once per block
	float		modelDecay;	//amp EG decay step per block (the decay knob), set once per block
	ModelState	model;
} AnalogOsc;

static inline uint8_t osc_isModel(const uint8_t waveform)
{
	return waveform < MODEL_END;
}

/** start a new hit */
void model_trigger(AnalogOsc* osc, const uint8_t vel);
/** render one block. sat is the tube saturator [0:1] */
void model_calcBlock(AnalogOsc* osc, int16_t* buf, const uint8_t size, const float gain, const float sat);

#endif /* ANALOGMODELS_H_ */
