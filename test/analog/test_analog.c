/*
 * test_analog.c: renders every instrument of the Analog engine with a fixed
 * set of pitches, decays, velocities, saturator settings, retriggers and amp
 * EG levels, and writes the samples (int16, little endian) to stdout.
 * Built twice by run.sh: against the original AnalogModels source
 * (-DORIGINAL, original/) and against the port in Core/; both outputs must
 * be identical.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "Soundengine/config.h"
#ifdef ORIGINAL
#include "Soundengine/DSPAudio/Oscillator.h"
#include "Soundengine/DSPAudio/AnalogModels.h"
typedef OscInfo Osc;
#define FIRST_INSTRUMENT	NUM_OSC_WAVEFORMS
#else
#include "Soundengine/DSPAudio/AnalogModels.h"
typedef AnalogOsc Osc;
#define FIRST_INSTRUMENT	0
#endif

//the hardware RNG: a fixed sequence, restarted for every case
static uint32_t rngState;
uint32_t GetRngValue(void)
{
	rngState = rngState * 1664525u + 1013904223u;
	return rngState;
}

#define BLOCKS	300		//0.43 s

typedef struct
{
	float hz;		//pitch (261.6256 = the original tuning)
	float decayS;	//amp EG length [s] (0: the decay knob at 0, the instrument's longest)
	uint8_t vel;
	float sat;
	int retrig;		//block of a second hit (0: none)
	int gateEnd;	//block in which the amp EG closes (0: open)
} Case;

static const Case cases[] =
{
	{ 261.6256f, 0.20f, 127, 0.f,  0,   0 },
	{ 261.6256f, 0.05f,  40, 0.f,  60,  0 },
	{ 130.8128f, 0.80f, 100, 0.5f, 120, 0 },
	{ 523.2511f, 0.02f, 127, 1.f,  10,  0 },
	{ 261.6256f, 0.f,    90, 0.2f, 200, 150 },
	{ 2093.005f, 0.30f, 127, 0.8f, 33,  0 },
	{ 32.7032f,  1.50f,  64, 0.f,  0,   80 },
};

int main(void)
{
	static int16_t buf[OUTPUT_DMA_SIZE];
	for(int ins = 0; ins < 38; ins++)
	{
		for(unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); k++)
		{
			const Case* c = &cases[k];
			Osc osc;
			memset(&osc, 0, sizeof(osc));
			rngState = 12345u + ins * 7919u + k;
			osc.waveform = FIRST_INSTRUMENT + ins;
			osc.phaseInc = (uint32_t)(c->hz * (4294967296.f / REAL_FS));
			//the amp EG decay step per block: open for decayS
			osc.modelDecay = c->decayS > 0.f ? (float)OUTPUT_DMA_SIZE / (c->decayS * REAL_FS) : 0.f;
			float env = 0.f;
			for(int b = 0; b < BLOCKS; b++)
			{
				if(b == 0 || b == c->retrig)
				{
					model_trigger(&osc, c->vel);
					env = 1.f;
				}
				if(c->gateEnd && b >= c->gateEnd)
					env = 0.f;
				osc.modelEnv = env;
				model_calcBlock(&osc, buf, OUTPUT_DMA_SIZE, 1.f, c->sat);
				fwrite(buf, sizeof(buf), 1, stdout);
			}
		}
	}
	return 0;
}
