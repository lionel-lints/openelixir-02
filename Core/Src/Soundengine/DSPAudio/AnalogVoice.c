/*
 * AnalogVoice.c
 *
 * Part of a custom firmware for the Erica Synths LXR-02 (not an official
 * Erica Synths release). Added 2026.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Licensed under the GNU General Public License version 3, with the
 * additional permission and terms in NOTICE.md. See LICENSE and NOTICE.md in
 * the root of the repository.
 */
#include "Soundengine/DSPAudio/AnalogVoice.h"
#include "Soundengine/DSPAudio/modulationNode.h"
#include "Soundengine/DSPAudio/BufferTools.h"
#include "IO/adcPots.h"
#include <math.h>

//------------------------------------------------------------------------------------
void Analog_init(AnalogVoice* v, uint8_t slot)
{
	v->slot = slot;
	initOnePole(&v->FaderVolumeFilter);
	setOnePoleCoef(&v->FaderVolumeFilter,0.01);
	initOnePole(&v->MixerPageFilter);
	setOnePoleCoef(&v->MixerPageFilter,0.01);
	initOnePole(&v->distortion.DriveParamFilter);
	setOnePoleCoef(&v->distortion.DriveParamFilter,0.01);
	SnapEg_init(&v->snapEg);
	v->pan = 0;
	v->vol = 0.8f;

	//the oscillator isn't played, it only holds the pitch (a sine's phase increment)
	v->osc.freq = 440;
	v->osc.waveform = SINE;
	v->osc.fmMod = 0;
	v->osc.midiFreq = 70<<8 | 64;
	v->osc.pitchMod = 1.0f;
	v->osc.modNodeValue = 1;

	setDistortionShape(&v->distortion, 0.f);
	v->volumeMod = 1;
	transient_init(&v->transGen);
	slopeEg2_init(&v->oscVolEg);
	SVF_init(&v->filter);
	v->filterType = 0x01;
	lfo_init(&v->lfo);

	v->instrument = MODEL_BD8;
	v->inst.waveform = MODEL_BD8;
}
//------------------------------------------------------------------------------------
void Analog_setInstrument(AnalogVoice* v, const uint8_t instrument)
{
	//a hit that rings keeps its instrument (its state means something else to another one)
	v->instrument = instrument < MODEL_END ? instrument : MODEL_END-1;
}
//------------------------------------------------------------------------------------
void Analog_trigger(AnalogVoice* v, const uint8_t vel, const uint8_t note)
{
	lfo_retrigger(v->slot);
	modNode_updateValue(&velocityModulators[v->slot],vel/127.f);

	v->velo = vel/127.f;
	osc_setBaseNote(&v->osc,note);
	transient_trigger(&v->transGen);
	SnapEg_trigger(&v->snapEg);

	v->triggeredPanL = sqrtf((127-v->pan)/127.f);
	v->triggeredPanR = sqrtf(v->pan/127.f);

	//the instrument and the envelope restart in the audio interrupt (a MIDI note from the
	//main loop could be interrupted half way otherwise)
	v->pendingVel = vel;
	v->pending = 1;
}
//------------------------------------------------------------------------------------
void Analog_calcAsync(AnalogVoice* v)
{
	const uint8_t start = v->pending;
	v->pending = 0;

	//transient "snap" mode: a short pitch drop like the other engines
	v->osc.pitchMod = 1.f;
	if(v->transGen.waveform == 0)
	{
		const float snapVal = SnapEg_calc(&v->snapEg, v->transGen.pitch);
		v->osc.pitchMod += snapVal * v->transGen.volume;
	}
	osc_setFreq(&v->osc);			//a sine: the phase increment of a 32 bit phase, as the instruments use it
	v->inst.phaseInc = v->osc.phaseInc;

	if(start)
	{
		//the instrument fades out its last hit, taken at the amp EG level of the last block
		v->inst.waveform = v->instrument;
		model_trigger(&v->inst, v->pendingVel);
		slopeEg2_trigger(&v->oscVolEg);
		SVF_reset(&v->filter);
	}

	//as the drum voice: an attack of 0 jumps, otherwise the level ramps over the block
	if((v->oscVolEg.attack == 1) && ((v->oscVolEg.state == EG_A) || (v->oscVolEg.state == EG_REPEAT)))
	{
		v->egValueOscVol = slopeEg2_calc(&v->oscVolEg);
		v->lastGain = v->egValueOscVol;
	}
	else
	{
		v->lastGain = v->egValueOscVol;
		v->egValueOscVol = slopeEg2_calc(&v->oscVolEg);
	}

	//the instrument follows the amp EG: its level (the declick of a retrigger) and its
	//decay step, which sets the instrument's length
	v->inst.modelEnv = v->egValueOscVol;
	v->inst.modelDecay = (float)v->oscVolEg.decay;
}
//------------------------------------------------------------------------------------
void Analog_calcSyncBlock(AnalogVoice* v, int16_t* buf, const uint8_t size)
{
	int16_t transBuf[size];

	model_calcBlock(&v->inst, buf, size, 1.f, v->sat);

	transient_calcBlock(&v->transGen,transBuf,size);
	bufferTool_addBuffersSaturating(buf,transBuf,size);

	SVF_calcBlockZDF(&v->filter,v->filterType,buf,size);

	bufferTool_addGainInterpolated(buf,v->egValueOscVol,v->lastGain,size);
	if(v->volumeMod)
	{
		bufferTool_addGain(buf,v->velo,size);
	}
	calcDistBlockFiltered(&v->distortion,buf,size);

	//the fader can't change while a block is calculated: read it once, not per sample
	const float faderVolume = adc_getFaderVolume(v->slot);
	for(int i=0; i< size; i++)
	{
		const float mixVol = calcOnePole(&v->MixerPageFilter, v->vol);
		v->combinedVolume = mixVol * calcOnePole(&v->FaderVolumeFilter, faderVolume);
		buf[i] *= v->combinedVolume;
	}
}
