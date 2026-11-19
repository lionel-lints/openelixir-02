/*
 * Oscillator.h for test/analog: the part of AnalogModels' OscInfo that its
 * AnalogModels.c uses, so the original instruments build without the rest of
 * its tree. Field names and types as in AnalogModels 0.5 (commit 17b06e7).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef OSCILLATOR_H_
#define OSCILLATOR_H_
#include <stdint.h>
#include "Soundengine/DSPAudio/AnalogModelState.h"
#define NUM_OSC_WAVEFORMS	6	//Sin, Tri, Saw, Rec, Noi, PWM
typedef struct
{
	uint8_t		waveform;
	uint32_t	phaseInc;
	float		modelEnv;
	float		modelDecay;
	ModelState	model;
} OscInfo;
#endif
