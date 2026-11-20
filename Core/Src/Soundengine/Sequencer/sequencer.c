/*
 * sequencer.c
 *
 *  Created on: 11.04.2012
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */


#include "stm32f7xx_hal.h"
#include "Soundengine/globals.h"
#include "Soundengine/DSPAudio/DrumVoice.h"
#include "Soundengine/DSPAudio/Snare.h"
#include "Soundengine/DSPAudio/HiHat.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/MIDI/Uart.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/MIDI/MidiVoiceControl.h"
#include "Soundengine/DSPAudio/CymbalVoice.h"
#include "Soundengine/Sequencer/sequencer.h"
#include <string.h>
#include <math.h>
#include <Soundengine/MIDI/CortexParser.h>
#include "Soundengine/USB/usb_manager.h"
#include "Soundengine/Sequencer/clockSync.h"
#include "Soundengine/MIDI/MidiParser.h"
#include "Soundengine/DSPAudio/automationNode.h"
//#include "SomGenerator.h"
#include "Soundengine/TriggerOut.h"
#include "Soundengine/FxBus.h"

#include "menu/menu.h"
#include "ledHandler.h"
#include "buttonHandler.h"
#include "Preset/PresetManager.h"
#include "Soundengine/CpuMeter.h"

void seq_setPendingPatternActive();



#define SEQ_PRESCALER_MASK 	0x03
#define MIDI_PRESCALER_MASK	0x04
uint8_t seq_prescaleCounter = 0;

uint8_t seq_masterStepCnt=0;				/** keeps track of the played steps between 0 and 127 independent from the track counters*/
static uint16_t seq_rollInterval = 24;		/**< 96 ppq ticks between roll hits ("16"), 0: one shot*/
uint8_t seq_rollState = 0;					/**< each bit represents a voice. if bit is set, roll is active*/

static int8_t 	seq_stepIndex[NUM_TRACKS];	/**< we have 16 steps con 4 bars = 64 steps.
											     each track has its own counter to allow different pattern lengths */

static uint16_t seq_tempo = 120;			/**< seq speed in bpm*/

static uint32_t	seq_lastTick = 0;			/**< stores the time the last step change occured*/
static float	seq_deltaT;					/**< time in [ms] until the next step
 	 	 	 	 	 	 	 	 	 	 	 1000ms = 1 sec
 	 	 	 	 	 	 	 	 	 	 	 1 min = 60 sec*/
uint8_t seq_activeAutomTrack=0;

uint8_t seq_delayedSyncStepFlag = 0;		//normally sync steps will only be advanced by external midi clocks in ext. sync mode
											//if the shuffle needs a delayed sync step, it is indicated here.

uint8_t seq_isSyncExternal = 0;
uint8_t seq_lastMasterStep[NUM_TRACKS];		//keeps track of the last triggered master sync step of each track


float seq_shuffle = 0;

int8_t seq_stepToSkip[NUM_TRACKS];

//---- note scheduler (OpenElixir-02 2.0, docs/PLAN-note-swing.md) ----
//The 96 ppq clock always runs straight. The hits of a step (its flam/arp substeps, with
//micro-shift and swing) are put into a queue per track with the exact time they sound and
//fired in the first audio block at or after it. Swing doesn't stretch the clock any more;
//seq_clockSwing only decides whether the clock outputs swing along.
//Times are in 1/SEQ_TIME_SCALE system ticks (wraps after 18 hours: compared as differences).
#define SEQ_TIME_SCALE		16
#define SEQ_QUEUE_SIZE		16		//a step has at most 8 substeps, older steps' repeats are cut
#define SEQ_CLOCK_QUEUE_SIZE	32		//the pulses of one swung step (24) and some
#define SEQ_HIT_FIRST		0x01	//the step's first hit (its note): never cut by a newer step
#define SEQ_HIT_ROLL		0x02	//a roll hit: no MIDI note off afterwards (stock)
//run once per step or per hit: kept out of seq_nextStep() (ITCM, which is nearly full)
#define SEQ_NOT_INLINED		__attribute__((noinline))
typedef struct
{
	uint32_t time;		//when it sounds
	uint8_t step;		//master step count of the step it belongs to
	uint8_t flags;
	uint8_t vol;
	uint8_t note;
} SeqHit;
static void seq_fireRoll(const uint8_t voice, const SeqHit* hit);
static SeqHit seq_queue[NUM_TRACKS][SEQ_QUEUE_SIZE];
static uint8_t seq_queueLen[NUM_TRACKS];
uint16_t seq_queueDropped = 0;				/**< hits that didn't fit into a queue (the tests require 0)*/
static uint8_t seq_firedSinceTick = 0;		/**< bit per track: a hit sounded since the last tick (MIDI note off)*/
typedef struct
{
	uint32_t time;
	uint8_t cnt;		//tick in the step (clock output)
	uint8_t midi;		//1: MIDI clock, 0: clock output tick
} SeqClockPulse;
static SeqClockPulse seq_clockQueue[SEQ_CLOCK_QUEUE_SIZE];
static uint8_t seq_clockQueueLen = 0;
uint8_t seq_clockSwing = 0;					/**< 0 (Not): clock outputs straight, 1 (Clk): they swing like the notes*/
static uint32_t seq_stepGrid = 0;			/**< exact time the current step started (straight grid)*/
static float seq_tickLateness = 0;			/**< system ticks the tick being processed is late (seq_tick)*/



static int8_t seq_armedArmedAutomationStep = -1;
static int8_t seq_armedArmedAutomationTrack = -1;

static uint8_t seq_mutedTracks=0;			/**< indicate which tracks are muted */
uint8_t seq_songModeEnabled=0;
uint8_t seq_running = 0;					/**<  1 running, 0 if stopped*/

uint8_t seq_activePattern = 0;				/**< the currently playing pattern*/
uint8_t seq_pendingPattern = 0;				/**< next pattern to play*/



uint8_t seq_selectedStep = 0;

uint8_t seq_recordActive = 0;				/**< set to 1 to activate the reording mode*/

uint8_t seq_eraseActive=0;					/**RECORD will be 1 if live erasing the active voice  */

uint8_t seq_quantisation = QUANT_16;

/**< random value for probability function*/
struct
{
	uint8_t prev;
	uint8_t current;
	uint8_t next;
}seq_rndValue[NUM_TRACKS];


//uint8_t seq_barCounter;						/**< counts the absolute position in bars since the seq was started */

static uint8_t seq_loadPendigFlag = 0;

// Allow it to be configured whether it keeps track of bar position in the song for
// the purpose of pattern changes
uint8_t seq_resetBarOnPatternChange=0;

// keep track of which midi notes are playing
static uint8_t midi_chan_notes[16];		    /**< what note is playing on each channel */
static uint16_t midi_notes_on=0;		    /**< which channels have a note currently playing */

const float seq_shuffleTable[16] =
{
		0.f,
		0.015625f,
		0.0625f,
		0.140625f,
		0.25f,
		0.390625f,
		0.5625f,
		0.765625f,
		1.f,
		0.984375f,
		0.9375f,
		0.859375f,
		0.75f,
		0.609375f,
		0.4375f,
		0.234375f,
};

#if USE_PATTERN_EDIT_BUFFER
	PatternSet seq_patterns;
#else
	PatternSet seq_patternSet;
#endif

PatternBackupBuffer patternBackupBuffer;

//TempPattern seq_tmpPattern;

//uint8_t seq_newPatternAvailable = 0; 	//indicate that a new pattern has loaded in the background and we should switch
uint8_t seq_songData[NUM_KIT_SLOTS][MAX_SONG_LENGTH];
uint8_t *seq_activeSong;
uint8_t seq_songPositionPointer = 0;	//for song playback, current song position
//REP xNN: repeats of seq_songRepeatPattern still to come (the position stays at the REP slot)
static uint8_t seq_songRepeatsLeft = 0;
static uint8_t seq_songRepeatPattern = 0;
//uint8_t seq_bankPreloadPendingFlag = NO_PRELOAD_PENDING;

//for the automation tracks each track needs 2 modNodes
static AutomationNode seq_automationNodes[NUM_TRACKS][2];

static void seq_sendMidi(MidiMsg msg);
static void seq_sendRealtime(const uint8_t status);
static void seq_sendProgChg(const uint8_t ptn);
//static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep);
//static void seq_activateTmpPattern();



//static uint8_t seq_isNextStepSyncStep();
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr);
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern);
static void seq_resetNote(Step *step);
static void seq_setStepIndexToStart();


uint8_t lastWasNoteOn[NUM_TRACKS];
//------------------------------------------------------------------------------
uint8_t seq_getMasterStepCnt()
{
	return seq_masterStepCnt;
}
//------------------------------------------------------------------------------
void seq_init()
{
	int i;

	//give active song pointer a proper value
	seq_activeSong = seq_songData[0];

	memset(seq_songData, SONG_END, sizeof(uint8_t) * MAX_SONG_LENGTH * NUM_KIT_SLOTS);
	seq_songData[0][0] = 0;

	for(i=0;i<NUM_TRACKS;i++) {
		autoNode_init(&seq_automationNodes[i][0]);
		autoNode_init(&seq_automationNodes[i][1]);

		seq_stepToSkip[i] = -1;
	}

	memset(seq_stepIndex,0,NUM_TRACKS);
	memset(seq_lastMasterStep,0,NUM_TRACKS);


	for(i=0;i<NUM_PATTERN;i++)
	{
		//seq_patternSet.seq_patternSettings[i].changeBar 	= 0;	//default setting: zero repeats (play once then change)
		//seq_patternSet.seq_patternSettings[i].nextPattern 	= i;	//default setting: repeat same pattern
		seq_clearPattern(i); // will clear all tracks in the pattern
	}

}
//------------------------------------------------------------------------------
/*
static void seq_activateTmpPattern()
{
	memcpy(&seq_patternSet.seq_subStepPattern[seq_activePattern],&seq_tmpPattern.seq_subStepPattern,sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][seq_activePattern],&seq_tmpPattern.seq_mainSteps[i],sizeof(uint16_t)*NUM_TRACKS);
	}
	//memcpy(&seq_patternSet.seq_patternSettings[seq_activePattern],&seq_tmpPattern.seq_patternSettings,sizeof(PatternSetting));
	memcpy(&seq_patternSet.length[seq_activePattern],&seq_tmpPattern.length,sizeof(uint8_t)*NUM_TRACKS);
}
*/
//------------------------------------------------------------------------------

void seq_copyPatternToBackupBuffer(uint8_t patternNr)
{
	memcpy(&patternBackupBuffer.seq_subStepPattern, &seq_patternSet.seq_subStepPattern[patternNr],  sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	memcpy(&patternBackupBuffer.seq_stepTiming, &seq_patternSet.seq_stepTiming[patternNr],  sizeof(uint8_t)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&patternBackupBuffer.seq_mainSteps[i], &seq_patternSet.seq_mainSteps[i][patternNr],  sizeof(uint16_t)*NUM_TRACKS);
	}
	memcpy(&patternBackupBuffer.length, &seq_patternSet.length[patternNr],  sizeof(uint8_t)*NUM_TRACKS);



	patternBackupBuffer.shuffle = seq_patternSet.shuffle[patternNr];;
	patternBackupBuffer.kitNr = seq_patternSet.kitNr[patternNr];
	memcpy(&patternBackupBuffer.arp, &seq_patternSet.arp[patternNr], sizeof(patternBackupBuffer.arp));

}
//------------------------------------------------------------------------------
void seq_copyBackupBufferToPattern(uint8_t patternNr)
{
	memcpy(&seq_patternSet.seq_subStepPattern[patternNr], &patternBackupBuffer.seq_subStepPattern,  sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	memcpy(&seq_patternSet.seq_stepTiming[patternNr]    , &patternBackupBuffer.seq_stepTiming,      sizeof(uint8_t)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][patternNr], &patternBackupBuffer.seq_mainSteps[i],  sizeof(uint16_t)*NUM_TRACKS);
	}
	memcpy(&seq_patternSet.length[patternNr], &patternBackupBuffer.length,  sizeof(uint8_t)*NUM_TRACKS);

	seq_patternSet.shuffle[patternNr] = patternBackupBuffer.shuffle;
	seq_patternSet.kitNr[patternNr] = patternBackupBuffer.kitNr;
	memcpy(&seq_patternSet.arp[patternNr], &patternBackupBuffer.arp, sizeof(patternBackupBuffer.arp));
}
/*
//------------------------------------------------------------------------------
void seq_copyEditBufferToRam(uint8_t patternNr)
{
	memcpy(&seq_patternSet.seq_subStepPattern[patternNr],&seq_tmpPattern.seq_subStepPattern, sizeof(Step)*NUM_TRACKS*NUM_STEPS);
	for(uint8_t i=0; i<NUM_BARS; i++)
	{
		memcpy(&seq_patternSet.seq_mainSteps[i][patternNr],&seq_tmpPattern.seq_mainSteps[i],sizeof(uint16_t)*NUM_TRACKS);
	}
	//memcpy(&seq_patternSet.seq_patternSettings[seq_activePattern],&seq_tmpPattern.seq_patternSettings,sizeof(PatternSetting));
	memcpy(&seq_patternSet.length[patternNr],&seq_tmpPattern.length,sizeof(uint8_t)*NUM_TRACKS);
}
*/
//------------------------------------------------------------------------------
void seq_setShuffle(float shuffle)
{
	seq_shuffle = shuffle;
}
//------------------------------------------------------------------------------
void seq_setClockSwing(uint8_t on)
{
	seq_clockSwing = on != 0;
}
//------------------------------------------------------------------------------
void seq_setTrackLength(uint8_t trackNr, uint8_t length)
{
	//set new end marker
	/*
	if(length == 16)
		length=0;
	*/
	// **PATROT this was changed from setting length on seq_activePattern to shown (or edited) pattern

#if USE_PATTERN_EDIT_BUFFER

	patternBackupBuffer.length[trackNr]=length;

#else
	seq_patternSet.length[frontParser_shownPattern][trackNr]=length;
#endif
}

//------------------------------------------------------------------------------
uint8_t seq_getTrackLength(uint8_t trackNr)
{
	// **PATROT this was changed from getting seq_activePattern to shown (or edited) pattern
#if USE_PATTERN_EDIT_BUFFER
	uint8_t r=patternBackupBuffer.length[trackNr];
#else
	uint8_t r=seq_patternSet.length[frontParser_shownPattern][trackNr];
#endif
	//0 is the old way of storing 16 (stock Euclid patterns): the sequencer plays it as 16
	if(r==0)
		return 16;
	return r;
}
//------------------------------------------------------------------------------
// **PATROT
//TODO check if rotation is working with new 4 bar layout
/*
void seq_setTrackRotation(uint8_t trackNr, const uint8_t newRot)
{
	// frontParser_shownPattern contains the pattern that is shown (being edited) on the front at the time this is called
	// seq_activePattern is the pattern that is now playing
	LengthRotate *lr=&seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr];

	if(newRot == lr->rotate)
		return;

	// if sequencer is running, move the current step position to compensate for the rotation
	if(seq_running) {
		int8_t len = lr->length;
		if(len==0)
			len=16;

		// this is how many main steps we need to move in one direction, negative means
		// move back positive is move forward
		int8_t offset=(((int8_t)newRot) % len) - (((int8_t)lr->rotate) % len);

        // if adding the offset to si would push it over the edge either way we need to wrap it
		// offset will always be less than len
        int16_t si = seq_stepIndex[trackNr] + (offset*8);
        if(si < 0 )
            si += (len*8);
        else if(si >= (len*8))
            si -= (len*8);
        seq_stepIndex[trackNr]=(int8_t)si;
	}

	//set new rotation value
	lr->rotate=newRot;

}
*/
//------------------------------------------------------------------------------
// **PATROT
/*
uint8_t seq_getTrackRotation(uint8_t trackNr)
{
	return seq_patternSet.seq_patternLengthRotate[frontParser_shownPattern][trackNr].rotate;
}
*/
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//time of one 96 ppq tick in system ticks (0.25 ms): 24 per 16th step
SEQ_NOT_INLINED static float seq_tickLength(void)
{
	if(seq_tempo == 0) return 1;
	return (1000*60.f)/seq_tempo/96.f*4.f;	//float: the integer division ran up to 0.45 % fast
}
//------------------------------------------------------------------------------
//the next tick interval. The clock always runs straight: swing is in the note times (and,
//with seq_clockSwing, in the clock output times), not in the clock any more.
static void seq_calcDeltaT(uint16_t bpm)
{
	(void)bpm;
	seq_deltaT = seq_tickLength();
}
//------------------------------------------------------------------------------
//the swing delay of a step in system ticks. All swing goes through here (per-track swing:
//only this function). The 2nd and 4th 16th of each beat are late: the steps with an even
//master step count (the first step after start counts 1).
SEQ_NOT_INLINED static float seq_swingDelay(uint8_t track, uint8_t masterStep)
{
	(void)track;
	return (masterStep & 1) ? 0.f : seq_shuffle * 16.f * seq_tickLength();
}
//------------------------------------------------------------------------------
static inline uint32_t seq_now(void)
{
	return systick_ticks * SEQ_TIME_SCALE;
}
//system ticks (float) -> scheduler time units, rounded up: a hit sounds in the first block at or
//after its exact time, like a stock tick
static inline int32_t seq_toTime(float systicks)
{
	return (int32_t)ceilf(systicks * SEQ_TIME_SCALE);
}
static inline uint8_t seq_isDue(uint32_t time, uint32_t now)
{
	return (int32_t)(time - now) <= 0;
}
//------------------------------------------------------------------------------
void seq_setBpm(uint16_t bpm)
{
	seq_tempo 	= bpm;
	//seq_calcDeltaT(bpm);
	lfo_recalcSync();
}
//------------------------------------------------------------------------------
uint16_t seq_getBpm()
{
	return seq_tempo;
}
//------------------------------------------------------------------------------
void seq_sync()
{
  if (seq_getExtSync() == 2)
  {
    sync_tick();
  }
}
//------------------------------------------------------------------------------
void seq_setNextPattern(const uint8_t patNr)
{
	if(patNr >= MAX_PATTERN) return;
	seq_pendingPattern = patNr;
	seq_loadPendigFlag = 1;

	if(!seq_running)
	{
		//immediately change pattern
		seq_setPendingPatternActive();
	}
}
//------------------------------------------------------------------------------
static void seq_sendMidi(MidiMsg msg)
{
	//send to usb midi
	usb_sendMidi(msg);

	//send to hardware midi out jack
	uart_sendMidi(msg);

}


//------------------------------------------------------------------------------
static void seq_parseAutomationNodes(uint8_t track, Step* stepData)
{
	//set new destination
	autoNode_setDestination(&seq_automationNodes[track][0], stepData->param1Nr);
	autoNode_setDestination(&seq_automationNodes[track][1], stepData->param2Nr);
	//set new mod value
	autoNode_updateValue(&seq_automationNodes[track][0], stepData->param1Val);
	autoNode_updateValue(&seq_automationNodes[track][1], stepData->param2Val);
}
//------------------------------------------------------------------------------
void seq_triggerVoice(uint8_t voiceNr, uint8_t vol, uint8_t note)
{
	uint8_t midiChan; // which midi channel to send a note on
	uint8_t midiNote; // which midi note to send

	if(voiceNr > 6) return;

	//the step index is -1 while stopped (e.g. rolls): no step to take automation and velocity from
	const int8_t step = seq_stepIndex[voiceNr];

#if USE_PATTERN_EDIT_BUFFER
	seq_parseAutomationNodes(voiceNr, &patternBackupBuffer.seq_subStepPattern[voiceNr][seq_stepIndex[voiceNr]]);
#else
	if(step >= 0)
		seq_parseAutomationNodes(voiceNr, &seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][step]);
#endif

	//turn the trigger off before sending the next one
	/*
	if(voiceNr>=5)
	{
		//hihat channels choke each other
		trigger_triggerVoice(5, TRIGGER_OFF);
		trigger_triggerVoice(6, TRIGGER_OFF);
	} else {
		trigger_triggerVoice(voiceNr, TRIGGER_OFF);
	}
	*/

	// if a note is on for that channel send note-off first
	voiceControl_noteOff(voiceNr);

	//Trigger internal synth voice
	voiceControl_noteOn(voiceNr, note, vol);

	midiChan = midi_MidiChannels[voiceNr];

	// the note that is played will be whatever is received unless we have a note override set
	// A note override is any non-zero value for this parameter
	//if(midi_NoteOverride[voiceNr] == 0)
	if(activeParameters[PAR_MIDI_NOTE1 + voiceNr] == 0)
		midiNote = note;
	else
		//midiNote = midi_NoteOverride[voiceNr];
		midiNote = activeParameters[PAR_MIDI_NOTE1 + voiceNr];

	//send the new note to midi/usb out
#if USE_PATTERN_EDIT_BUFFER
	seq_sendMidiNoteOn(midiChan, midiNote, patternBackupBuffer.seq_subStepPattern[voiceNr][seq_stepIndex[voiceNr]].volume&STEP_VOLUME_MASK);
#else
	seq_sendMidiNoteOn(midiChan, midiNote, step >= 0 ? seq_patternSet.seq_subStepPattern[seq_activePattern][voiceNr][step].volume&STEP_VOLUME_MASK : vol);
#endif
}
//------------------------------------------------------------------------------
//has to be called AFTER the last pattern switch is finished, so the playing pattern is properly put into the temp buffer
#if 0
static void seq_CheckIfNextPatternNeedsBankSwitch()
{
	if(seq_songPositionPointer+1 < MAX_SONG_LENGTH)
	{
		//determine next bank to load
		const uint8_t nextPattern = seq_activeSong[seq_songPositionPointer+1];
		if(nextPattern==SONG_END || nextPattern == SONG_LOOP) return;
		const uint8_t nextBank = nextPattern/16;

		/*
		const uint8_t lowestPatternInMemory = menu_activePatternBank * 16;
		const uint8_t highestPatternInMemory = lowestPatternInMemory + 15;

		if( (nextPattern > lowestPatternInMemory) && (nextPattern  < highestPatternInMemory) )
		*/
		menu_activePatternBank = nextBank ;
	}
}
#endif
//------------------------------------------------------------------------------
static uint8_t seq_determineNextPattern(uint8_t autoIncrement)
{

	/*
	  extern uint8_t seq_songData[MAX_SONG_LENGTH];
	  extern uint8_t seq_songPositionPointer;
	 */
	if(seq_songModeEnabled)
	{
		if(autoIncrement)
		{
			if(seq_songRepeatsLeft)
			{
				seq_songRepeatsLeft--;
				return seq_songRepeatPattern;
			}
			//(bounded: REP entries that have nothing to repeat are skipped)
			for(uint8_t i=0;i<MAX_SONG_LENGTH;i++)
			{
				seq_songPositionPointer++;
				//TODO: this will loop when all 64 pattern slots are used... maybe a stop would be better?
				if(seq_songPositionPointer >= MAX_SONG_LENGTH) seq_songPositionPointer = 0;
				const uint8_t entry = seq_activeSong[seq_songPositionPointer];
				if(entry < SONG_REP_FIRST || entry > SONG_REP_LAST)
					return entry;
				//REP xNN: the pattern that just played, NN more times (this one is the first);
				//nothing to repeat at the song start
				if(seq_songPositionPointer > 0 && seq_activePattern < MAX_PATTERN)
				{
					seq_songRepeatPattern = seq_activePattern;
					seq_songRepeatsLeft = (uint8_t)(entry - SONG_REP_FIRST);
					return seq_songRepeatPattern;
				}
			}
			return SONG_END;
		}
		else
		{
			if(seq_songRepeatsLeft)
				return seq_songRepeatPattern;
			uint8_t pos = seq_songPositionPointer +1;
			if(pos >= MAX_SONG_LENGTH) pos = 0;
			if(seq_activeSong[pos] >= SONG_REP_FIRST && seq_activeSong[pos] <= SONG_REP_LAST)
				return seq_activePattern;
			return seq_activeSong[pos];
		}
	}
	else
		return seq_activePattern;

	/*
	const PatternSetting * const p=&seq_patternSet.seq_patternSettings[seq_activePattern];
	if(seq_barCounter % (p->changeBar+1) == 0)
		return p->nextPattern;
	else
		*/

}

#include "Soundengine/MIDI/Uart.h"

IN_ITCM_RAM void seq_setPendingPatternActive()
{
  midi_process_block = 1;
	if((seq_activePattern != seq_pendingPattern) || seq_loadPendigFlag)
	{
    seq_setStepIndexToStart();
		// if this setting is active and the user has manually changed patterns,
		// reset the bar counter. uncommenting the below will cause it to only reset
		// when a manual pattern change is invoked. to me the whole auto pattern change modulo stuff
		// above is a bit broken.

		/*
		if( seq_resetBarOnPatternChange)
			seq_barCounter=0;
			*/

		// TODO we need to also reset barCounter to 0 when the end of a repetition set of a pattern plays
		// EVEN IF the pattern is set to play itself again, this will facilitate having the bits that play
		// certain steps only on certain intervals of bar counter

		seq_loadPendigFlag = 0;

		/*//background loading removed
		//first check if 2 new pattern is available
		if(seq_newPatternAvailable)
		{
			seq_newPatternAvailable = 0;
			seq_activateTmpPattern();
		}
		*/
		//copy current backup to current slot (discarding unsaved changes)
		seq_copyBackupBufferToPattern(seq_activePattern);
		//copy pending pattern to backup buffer
		seq_copyPatternToBackupBuffer(seq_pendingPattern);

		seq_activePattern = seq_pendingPattern;
		//a pattern loaded from another project replaces the slot's restored version now
		preset_patternActivated();

		activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[seq_activePattern];
		seq_setShuffle(activeParameters[PAR_SHUFFLE]/127.f);

		//A new pattern is activated here -> Load corresponding drumkit
		if(activeParameters[PAR_PATTERN_MODE] == SKIP_NEXT_KIT_AUTOLOAD)
		{
			activeParameters[PAR_PATTERN_MODE] = 1;
		}
		else if(activeParameters[PAR_PATTERN_MODE] == 1)
		{
			//turn FX off to save cycles
			fxBus.fxType = FX_OFF;
			preset_loadDrumsetRam(seq_patternSet.kitNr[seq_activePattern], 0, 1);
			menu_selectedPresetNr[SAVE_TYPE_KIT] = seq_patternSet.kitNr[seq_activePattern];
			menu_loadedPresetNr[CURRENT_KIT] = seq_patternSet.kitNr[seq_activePattern];
			preset_touchTimer(PRESET_KIT_TIMER);
		}

		//use shuffle value from pattern
		//activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[seq_activePattern];
		//seq_setShuffle(activeParameters[PAR_SHUFFLE]/127.f);

		//reset pattern position to pattern rotate starting position for the active pattern **PATROT
		// seq_setStepIndexToStart();

		//send the ack message to tell the front that a new pattern starts playing
		/*
		uart_sendFrontpanelByte(FRONT_SEQ_CC);
		uart_sendFrontpanelByte(FRONT_SEQ_CHANGE_PAT);
		uart_sendFrontpanelByte(seq_activePattern);
		*/

		//if(frontParser_midiMsg.data2 > 7) return;
		//ack message that the sequencer changed to the requested pattern

		//stop blinking pattern led
		led_setBlinkLed((uint8_t)(led_idx2StepLed(seq_activePattern)),0);
		//clear last pattern led



		menu_playedPattern = seq_activePattern;

		/*
		//step display follows pattern changes
		//if(parameter_values[PAR_FOLLOW] & FOLLOW_PATTERN ) {
		//Playing pattern is always the shown pattern. Makes more sense!
		if(1) {

			//update for song screen to follow patterns
			menu_songMenuPosition = seq_songPositionPointer;

			if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
			{

				menu_setShownPattern(seq_activePattern);
				led_clearSequencerLeds();
				//query current sequencer step states and light up the corresponding leds
				const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
				//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
				//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


				//frontPanel_sendData(LED_CC,LED_QUERY_SEQ_TRACK,value); muss gemacht werden
				//frontParser_updateTrackLeds(trackNr, patternNr);
				led_updateSeqLeds();
				activeParameters[PAR_TRACK_LENGTH] = seq_getTrackLength(trackNr);
				//parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(trackNr);

				//frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,frontParser_midiMsg.data2); muss auch gem,acht werdne!
				//parameter_values[PAR_PATTERN_BEAT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].changeBar;
				//parameter_values[PAR_PATTERN_NEXT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].nextPattern;
				menu_repaint();


			} else {
				//store the pending pattern update for shift button release handler
				menu_shownPattern = seq_activePattern;


			}
		}




		//if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
		if( (buttonHandler_getMode() == SELECT_MODE_PATTERN))
		{
			//only show pattern changes when in select pattern  mode

			led_clearSequencerLeds();
			//led_clearSelectLeds();
			led_clearAllBlinkLeds();
			// re init the LEDs showing active/viewed pattern
			led_initPatternSelectLeds();
			//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
			menu_repaint();

		}
		else if(( (buttonHandler_getMode() == SELECT_MODE_SONG)))
		{
			//update song mode screen
			menu_repaintAll();
		}
		*/


		// send a pattern change message to midi/usb out
		seq_sendProgChg(seq_activePattern);


		// all notes off here since we are switching patterns
		voiceControl_noteOff(0xFF);

		//song mode - check for preload pattern bank
		/*
		if(seq_songModeEnabled)
		{
			seq_CheckIfNextPatternNeedsBankSwitch();
		}
		*/

	}
	else if(activeParameters[PAR_PATTERN_MODE] == SKIP_NEXT_KIT_AUTOLOAD)
	{
		activeParameters[PAR_PATTERN_MODE] = 1;
	}
	//moved song list display here to prevent skipping list on same consecutive patterns
	if(1) {

				//update for song screen to follow patterns (-1 = 255: the song hasn't started yet,
				//it starts at 0; never more than the 64 song slots)
				menu_songMenuPosition = seq_songPositionPointer < MAX_SONG_LENGTH ? seq_songPositionPointer : 0;

				if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
				{

					menu_setShownPattern(seq_activePattern);
					led_clearSequencerLeds();
					//query current sequencer step states and light up the corresponding leds
					const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
					//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
					//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


					//frontPanel_sendData(LED_CC,LED_QUERY_SEQ_TRACK,value); muss gemacht werden
					//frontParser_updateTrackLeds(trackNr, patternNr);
					led_updateSeqLeds();
					activeParameters[PAR_TRACK_LENGTH] = seq_getTrackLength(trackNr);
					//parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(trackNr);

					//frontPanel_sendData(SEQ_CC,SEQ_REQUEST_PATTERN_PARAMS,frontParser_midiMsg.data2); muss auch gem,acht werdne!
					//parameter_values[PAR_PATTERN_BEAT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].changeBar;
					//parameter_values[PAR_PATTERN_NEXT] = seq_patternSet.seq_patternSettings[frontParser_shownPattern].nextPattern;
					menu_repaint();


				} else {
					//store the pending pattern update for shift button release handler
					menu_shownPattern = seq_activePattern;

					//the song page shows the new song position
					if(menu_activePage == SONG_MODE_PAGE)
						menu_repaint();
				}
			}




			//if( (buttonHandler_getMode() == SELECT_MODE_PERF) || (buttonHandler_getMode() == SELECT_MODE_PAT_GEN) )
			if( (buttonHandler_getMode() == SELECT_MODE_PATTERN))
			{
				//only show pattern changes when in select pattern  mode

				led_clearSequencerLeds();
				//led_clearSelectLeds();
				led_clearAllBlinkLeds();
				// re init the LEDs showing active/viewed pattern
				led_initPatternSelectLeds();
				//led_setValue(1,LED_PART_SELECT1+frontParser_midiMsg.data2);
				menu_repaint();

			}
			else if(( (buttonHandler_getMode() == SELECT_MODE_SONG)))
			{
				//update song mode screen
				menu_repaintAll();
			}

  midi_process_block = 0;
}


uint8_t seq_getLongestTrack()
{
	uint8_t trackId = 0;

	for(int i=1;i<NUM_TRACKS;i++)
	{
		if(seq_patternSet.length[seq_activePattern][i] > seq_patternSet.length[seq_activePattern][trackId])
		{
			trackId = i;
		}
	}
	return trackId;
}

int8_t getShiftValue(uint8_t in)
{
	switch(in)
	{
	case 0:
		return -7;
		break;

	case 1:
		return -6;
		break;

	case 2:
		return -5;
		break;

	case 3:
		return -4;
		break;

	case 4:
		return -3;
		break;

	case 5:
		return -2;
		break;

	case 6:
		return -1;
		break;

	default:
	case 7:
		return 0;
		break;

	case 8:
		return 1;
		break;

	case 9:
		return 2;
		break;

	case 10:
		return 3;
		break;

	case 11:
		return 4;
		break;

	case 12:
		return 5;
		break;

	case 13:
		return 6;
		break;

	case 14:
		return 7;
		break;
	}
}

//------------------------------------------------------------------------------
//step arp: chord tones in semitones above the step's note, they repeat one octave higher
static const int8_t seq_arpChords[ARP_NUM_CHORDS][5] =
{
	//number of tones, tones
	{1, 0},				//ARP_REPEAT
	{3, 0, 4, 7},		//ARP_MAJ
	{3, 0, 3, 7},		//ARP_MIN
	{3, 0, 3, 6},		//ARP_DIM
	{3, 0, 4, 8},		//ARP_AUG
	{4, 0, 4, 7, 11},	//ARP_MAJ7
	{4, 0, 3, 7, 10},	//ARP_MIN7
	{4, 0, 3, 6, 9},	//ARP_DIM7
	{4, 0, 4, 8, 10},	//ARP_AUG7
};
//number of substeps for the flam values (off, x2, x3, x4, x6, x8)
static const uint8_t seq_flamSubsteps[6] = {1, 2, 3, 4, 6, 8};
//------------------------------------------------------------------------------
//the note substep (0..numSubsteps-1) of a step plays: the step's note, or with the step arp a
//note of the chord
SEQ_NOT_INLINED static uint8_t seq_arpNote(const uint8_t track, const uint8_t step, const uint8_t pattern, uint8_t substep, const uint8_t numSubsteps)
{
	const uint8_t note = seq_patternSet.seq_subStepPattern[pattern][track][step].note;
	const uint8_t arp = seq_patternSet.arp[pattern][track][step];
	const uint8_t chord = arp & ARP_CHORD_MASK;
	if(chord == ARP_REPEAT || chord >= ARP_NUM_CHORDS)
		return note;
	if(arp & ARP_DIR_DOWN)
		substep = numSubsteps-1 - substep;
	const uint8_t numTones = seq_arpChords[chord][0];
	const int16_t arpNote = note + seq_arpChords[chord][1 + substep%numTones] + 12*(substep/numTones);
	return arpNote > 127 ? 127 : (uint8_t)arpNote;
}
//------------------------------------------------------------------------------
//put a hit into the track's queue, sorted by time (equal times keep their order)
SEQ_NOT_INLINED static void seq_queueInsert(const uint8_t track, const SeqHit* hit)
{
	SeqHit* q = seq_queue[track];
	uint8_t len = seq_queueLen[track];
	if(len == SEQ_QUEUE_SIZE)
	{
		//full: an older step's repeat makes room (first hits are never dropped)
		int8_t victim = -1;
		for(uint8_t i=0;i<len;i++)
		{
			if(!(q[i].flags & SEQ_HIT_FIRST) && (victim < 0 || (int8_t)(q[i].step - q[victim].step) < 0))
				victim = (int8_t)i;
		}
		seq_queueDropped++;
		if(victim < 0)
			return;
		for(uint8_t i=(uint8_t)victim;i<len-1;i++)
			q[i] = q[i+1];
		len--;
	}
	uint8_t pos = len;
	while(pos > 0 && (int32_t)(q[pos-1].time - hit->time) > 0)
	{
		q[pos] = q[pos-1];
		pos--;
	}
	q[pos] = *hit;
	seq_queueLen[track] = len + 1;
}
//------------------------------------------------------------------------------
static uint8_t seq_queuePrimed = 0;			//0: nothing was queued ahead for the next step yet
static void seq_queueClear(void)
{
	memset(seq_queueLen, 0, sizeof(seq_queueLen));
	seq_clockQueueLen = 0;
	seq_firedSinceTick = 0;
	seq_queuePrimed = 0;
}
//------------------------------------------------------------------------------
//queue the hits of a step: its flam/arp substeps, moved by micro-shift and swing.
//gridOffset: system ticks from seq_stepGrid to the step's straight start (0, or one step
//ahead for a step that plays early). dice: the step's random value for the probability.
SEQ_NOT_INLINED static void seq_enqueueStep(const uint8_t track, const uint8_t step, const uint8_t pattern,
		const uint8_t masterStep, const uint8_t dice, const float gridOffset)
{
	if(!seq_intIsMainStepActive(track, step, pattern))
		return;
	const Step* s = &seq_patternSet.seq_subStepPattern[pattern][track][step];
	if(dice > s->prob)
		return;
	const StepTiming timing = seq_patternSet.seq_stepTiming[pattern][track][step];
	const uint8_t numSubsteps = timing.flam < 6 ? seq_flamSubsteps[timing.flam] : 1;
	const float tick = seq_tickLength();
	const float start = gridOffset + seq_swingDelay(track, masterStep) + getShiftValue(timing.shift)*3*tick;
	SeqHit hit;
	hit.step = masterStep;
	hit.vol = s->volume & STEP_VOLUME_MASK;
	for(uint8_t k=0;k<numSubsteps;k++)
	{
		hit.time = seq_stepGrid + (uint32_t)seq_toTime(start + k*(24/numSubsteps)*tick);
		hit.flags = k == 0 ? SEQ_HIT_FIRST : 0;
		hit.note = seq_arpNote(track, step, pattern, k, numSubsteps);
		seq_queueInsert(track, &hit);
	}
}
//------------------------------------------------------------------------------
//the steps that play before their own tick 0 (negative micro-shift) are queued one step early
SEQ_NOT_INLINED static uint8_t seq_isEarlyStep(const uint8_t track, const uint8_t step, const uint8_t pattern)
{
	return getShiftValue(seq_patternSet.seq_stepTiming[pattern][track][step].shift) < 0;
}
//------------------------------------------------------------------------------
static void seq_emitClockPulse(const uint8_t cnt, const uint8_t midi)
{
	if(midi)
		seq_sendRealtime(MIDI_CLOCK);
	else
		trigger_clockTick(0, cnt);
}
//------------------------------------------------------------------------------
//a clock output pulse of the tick being processed (tick cnt of the current step). Straight it
//goes out now; with seq_clockSwing the pulses of a swung step are spread evenly from the swing
//delay to the end of the step (the next step's first pulse is on time).
static void seq_clockOut(const uint8_t cnt, const uint8_t midi)
{
	float delay = 0;
	//(not before the first step: a tick of no step doesn't swing)
	if(seq_clockSwing && seq_running && seq_queuePrimed)
	{
		const float swing = seq_swingDelay(0, seq_masterStepCnt);
		delay = swing - cnt*swing/24.f;
	}
	if(delay <= 0 || seq_clockQueueLen >= SEQ_CLOCK_QUEUE_SIZE)
	{
		seq_emitClockPulse(cnt, midi);
		return;
	}
	SeqClockPulse* p = &seq_clockQueue[seq_clockQueueLen++];
	p->time = seq_now() - (uint32_t)seq_toTime(seq_tickLateness) + (uint32_t)seq_toTime(delay);
	p->cnt = cnt;
	p->midi = midi;
}
//------------------------------------------------------------------------------
//sound everything that is due: called on every tick and once per audio block
SEQ_NOT_INLINED static void seq_firePending(void)
{
	const uint32_t now = seq_now();

	//clock pulses (in time order: they are queued in order)
	uint8_t n = 0;
	while(n < seq_clockQueueLen && seq_isDue(seq_clockQueue[n].time, now))
	{
		seq_emitClockPulse(seq_clockQueue[n].cnt, seq_clockQueue[n].midi);
		n++;
	}
	if(n)
	{
		memmove(seq_clockQueue, &seq_clockQueue[n], (seq_clockQueueLen-n)*sizeof(SeqClockPulse));
		seq_clockQueueLen -= n;
	}

	if(!seq_running)
		return;

	for(uint8_t track=0;track<NUM_TRACKS;track++)
	{
		SeqHit* q = seq_queue[track];
		const uint8_t len = seq_queueLen[track];
		if(len == 0 || !seq_isDue(q[0].time, now))
			continue;

		//the newest step among the due hits sounds; hits of older steps due in the same block
		//are merged into it (stock: one trigger per track and tick)
		uint8_t due = 0;
		uint8_t newest = q[0].step;
		while(due < len && seq_isDue(q[due].time, now))
		{
			if((int8_t)(q[due].step - newest) > 0)
				newest = q[due].step;
			due++;
		}
		uint8_t cut = 0;
		const uint8_t muted = (seq_mutedTracks & (1<<track)) != 0;
		{
			for(uint8_t i=0;i<due;i++)
			{
				//(rolls also play on muted tracks, as in stock)
				if(q[i].step != newest || (muted && !(q[i].flags & SEQ_HIT_ROLL)))
					continue;
				if(q[i].flags & SEQ_HIT_ROLL)
				{
					seq_fireRoll(track, &q[i]);
				}
				else
				{
					seq_triggerVoice(track, q[i].vol, q[i].note);
					lastWasNoteOn[track] = 1;
					seq_firedSinceTick |= (uint8_t)(1<<track);
				}
				//a newer step's note ends the repeats of older steps
				if((q[i].flags & (SEQ_HIT_FIRST|SEQ_HIT_ROLL)) == SEQ_HIT_FIRST)
					cut = 1;
			}
		}
		uint8_t kept = 0;
		for(uint8_t i=due;i<len;i++)
		{
			if(cut && !(q[i].flags & SEQ_HIT_FIRST) && (int8_t)(q[i].step - newest) < 0)
				continue;
			q[kept++] = q[i];
		}
		seq_queueLen[track] = kept;
	}
}
//------------------------------------------------------------------------------
//MIDI note off of a track whose last tick had a hit and this one hasn't (stock: notes are one
//tick long)
static void seq_tickNoteOffs(void)
{
	if(!seq_running)
		return;
	for(uint8_t track=0;track<NUM_TRACKS;track++)
	{
		if(seq_stepIndex[track] < 0 || (seq_mutedTracks & (1<<track)))
			continue;
		if(!(seq_firedSinceTick & (1<<track)) && lastWasNoteOn[track])
		{
			lastWasNoteOn[track] = 0;
			voiceControl_noteOff(track);
		}
	}
	seq_firedSinceTick = 0;
}
//------------------------------------------------------------------------------
//the step after step on a track (each track has its own length)
SEQ_NOT_INLINED static uint8_t seq_nextTrackStep(const uint8_t track, const uint8_t step)
{
	uint8_t len = seq_patternSet.length[seq_activePattern][track];
	if(!len)
		len = 16;
	uint8_t next = (uint8_t)(step + 1);
	if(next >= len || (next & 0x3f) == 0)
		next = 0;
	return next;
}
//------------------------------------------------------------------------------
//does the next step start the pattern again (the longest track ends)? (as in seq_nextStep)
SEQ_NOT_INLINED static uint8_t seq_nextIsPatternStart(void)
{
	const uint8_t longest = seq_getLongestTrack();
	uint8_t len = seq_patternSet.length[seq_activePattern][longest];
	if(!len)
		len = 16;
	const uint8_t next = (uint8_t)(seq_stepIndex[longest] + 1);
	return ((next & 0x3f) == 0) || next >= len;
}
//------------------------------------------------------------------------------
//the pattern the next step plays in: at the end of the pattern a queued change or the song's
//next pattern (>= MAX_PATTERN: the song ends there)
SEQ_NOT_INLINED static uint8_t seq_lookaheadPattern(void)
{
	if(!seq_nextIsPatternStart())
		return seq_activePattern;
	if(seq_activePattern != seq_pendingPattern || seq_loadPendigFlag)
		return seq_pendingPattern;
	uint8_t p = seq_determineNextPattern(0);
	if(p == SONG_LOOP)
		p = seq_activeSong[0];
	return p;
}
//------------------------------------------------------------------------------
//queue the notes of the step that just started: each track's step (unless it plays early and
//was queued one step ahead), and the track's next step if that one plays early (negative
//micro-shift)
SEQ_NOT_INLINED static void seq_queueStepNotes(void)
{
	const uint8_t primed = seq_queuePrimed;
	seq_queuePrimed = 1;
	const uint8_t nextPattern = seq_lookaheadPattern();
	const float stepLength = 24.f*seq_tickLength();
	for(uint8_t t=0;t<NUM_TRACKS;t++)
	{
		const int8_t step = seq_stepIndex[t];
		if(step < 0)
			continue;
		//(right after start nothing was queued ahead)
		if(!primed || !seq_isEarlyStep(t, (uint8_t)step, seq_activePattern))
			seq_enqueueStep(t, (uint8_t)step, seq_activePattern, seq_masterStepCnt, seq_rndValue[t].current, 0);
		if(nextPattern < MAX_PATTERN)
		{
			const uint8_t next = nextPattern != seq_activePattern || seq_nextIsPatternStart() ? 0 : seq_nextTrackStep(t, (uint8_t)step);
			if(seq_isEarlyStep(t, next, nextPattern))
				seq_enqueueStep(t, next, nextPattern, (uint8_t)(seq_masterStepCnt+1), seq_rndValue[t].next, stepLength);
		}
	}
}
//------------------------------------------------------------------------------
//a roll hit sounds (and is recorded, as in stock)
static void seq_fireRoll(const uint8_t voice, const SeqHit* hit)
{
	seq_triggerVoice(voice, hit->vol, hit->note);
	seq_addNote(voice, hit->vol, hit->note);
}
//------------------------------------------------------------------------------
//queue a held roll's hits of the current step that are at or after "from" (time since the step's
//grid in system ticks; 0 at the step start). Rolls are on the global grid: a hit where the tick
//count since the start is a multiple of the interval (the count wraps after 256 steps = 6144
//ticks, which every interval divides). Rolls whose hits are all on 16ths (16 and slower) swing
//with their step; the faster ones stay straight. A swung hit counts by its swung time: pressed
//during a swung step before its swung hit, that hit still comes.
SEQ_NOT_INLINED static void seq_queueRollHits(const uint8_t voice, const float from)
{
	if(seq_rollInterval == 0)
		return;
	const uint16_t base = (uint16_t)((uint8_t)(seq_masterStepCnt - 1)) * 24;
	const float tick = seq_tickLength();
	const float swing = (seq_rollInterval % 24) == 0 ? seq_swingDelay(voice, seq_masterStepCnt) : 0.f;
	const int8_t stepIdx = seq_stepIndex[voice];
	const uint8_t note = stepIdx >= 0 ? seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepIdx].note : SEQ_DEFAULT_NOTE;
	const uint32_t now = seq_now();
	for(uint8_t k=0;k<24;k++)
	{
		if((base + k) % seq_rollInterval != 0)
			continue;
		//(the tick at "from" itself still counts, as the next tick that hasn't passed)
		if(k*tick + swing < from - 1e-3f*tick)
			continue;
		const SeqHit hit = { seq_stepGrid + (uint32_t)seq_toTime(k*tick + swing), seq_masterStepCnt,
				SEQ_HIT_FIRST|SEQ_HIT_ROLL, ROLL_VOLUME, note };
		//due now (a straight step's first hit): at once, like stock
		if(seq_isDue(hit.time, now))
			seq_fireRoll(voice, &hit);
		else
			seq_queueInsert(voice, &hit);
	}
}
//------------------------------------------------------------------------------
//a released roll: its pending hits don't sound
static void seq_removeRollHits(const uint8_t voice)
{
	SeqHit* q = seq_queue[voice];
	uint8_t kept = 0;
	for(uint8_t i=0;i<seq_queueLen[voice];i++)
	{
		if(!(q[i].flags & SEQ_HIT_ROLL))
			q[kept++] = q[i];
	}
	seq_queueLen[voice] = kept;
}
//------------------------------------------------------------------------------
//everything a tick does after seq_nextStep(): clock output, notes, MIDI clock
static void seq_processTick(const uint8_t sendMidiClock)
{
	if(seq_isRunning())
		seq_clockOut(seq_prescaleCounter, 0);
	seq_firePending();
	seq_tickNoteOffs();
	if(sendMidiClock && (seq_prescaleCounter%MIDI_PRESCALER_MASK) == 0)
		seq_clockOut(seq_prescaleCounter, 1);
}
//------------------------------------------------------------------------------
void seq_MidiSyncQueue(uint8_t cnt)
{

	seq_prescaleCounter = cnt;
	trigger_reset(1);
	seq_lastTick = systick_ticks;



	if(seq_tempo !=0)
	{
		seq_calcDeltaT(seq_tempo);
	}

}


IN_ITCM_RAM void seq_nextStep()
{
  midi_process_block = 1;
	led_blinkHandler(); //blink leds in sync to tempo/seq

	if(!seq_running)
		return;

	seq_masterStepCnt++;
	//the exact time of this step on the straight grid (its tick may be handled a little late)
	seq_stepGrid = seq_now() - (uint32_t)(int32_t)(seq_tickLateness*SEQ_TIME_SCALE + 0.5f);

	//---- calc master step position. max value is 64. also take in regard the pattern length -----
	// track 0 determines the master step position
	uint8_t masterStepPos;
	uint8_t seqlen;
	uint8_t longestTrack;

	longestTrack = seq_getLongestTrack();
	seqlen=seq_patternSet.length[seq_activePattern][longestTrack];
	if(!seqlen)
		seqlen=16;

	if( (((seq_stepIndex[longestTrack]+1) & 0x3f) == 0) || ((seq_stepIndex[longestTrack]+1) >= seqlen))
			//(((seq_patternSet.seq_subStepPattern[seq_activePattern][0][seq_stepIndex[0]+1]).note & PATTERN_END)) )
	{
		masterStepPos = 0;
		//a bar has passed
		//seq_barCounter++;
	}
	else
	{
		//determine the longest sequencer track and use it as master track (make sure all bars play befor epattern changes)
		masterStepPos = seq_stepIndex[longestTrack]+1;
	}

	//-------- check if the master track has ended and check if a pattern switch is necessary --------
	if(masterStepPos == 0)
	{
		if(seq_activePattern == seq_pendingPattern)
		{
			//check pattern settings if we have to auto change patterns
			seq_pendingPattern = seq_determineNextPattern(1);
			/*
			if(seq_pendingPattern >= SEQ_NEXT_RANDOM)
			{
				uint8_t limit = seq_pendingPattern - SEQ_NEXT_RANDOM +2;
				uint8_t rnd = GetRngValue() % limit;
				seq_pendingPattern = rnd;
			}
			*/
			if(seq_pendingPattern == SONG_LOOP)
			{
				//reset to song start
				seq_pendingPattern = seq_activeSong[0];
				seq_songPositionPointer = 0;
				seq_songRepeatsLeft = 0;
			}
			//the end of the song, or a loop to a song that has no pattern before its
			//end/loop mark (that played "pattern 65" and edits went past the patterns)
			if(seq_pendingPattern >= MAX_PATTERN)
			{
				seq_setRunning(0);
				//make sure pending pattern does not remain at 64 resulting in a seg fault.
				seq_pendingPattern = seq_activeSong[0];
				if(seq_pendingPattern >= MAX_PATTERN) seq_pendingPattern  = 0;
				//like pressing stop: MIDI stop, notes off, position reset, and tell
				//the front panel (it still showed running, so the next start press stopped)
				seq_resetPosition();
				uart_sendFrontpanelByte(FRONT_SEQ_CC);
				uart_sendFrontpanelByte(FRONT_SEQ_RUN_STOP);
				uart_sendFrontpanelByte(0);
				return;
			}

		}

		// a new pattern is about to start
		// set pendingPattern active
		seq_setPendingPatternActive();

    midi_process_block = 0;
	}

	//---------- now check if the master track is at a full beat position to flash the start/stop button --------
	if((masterStepPos&3) == 0)
	{
		//&32 <=> %32
		//a quarter beat occured (multiple of 4 steps in the 16 step pattern)
		//turn on the start/stop led (beat indicator)
		led_setValue(1,LED_START_STOP);

	}
	else if ((masterStepPos&3) == 1)
	{
		//TODO datenmenge zur front reduzieren
		//turn it of again on the next step
		led_setValue(0,LED_START_STOP);
	}

	//--------- Time to process the single tracks -------------------------
	//trigger_clockTick(seq_stepIndex[0]+1);
//	trigger_clockTick(1, 0);

	int i;
	for(i=0;i<NUM_TRACKS;i++)
	{
		//increment the step index
		seq_stepIndex[i]++;
		//check if track end is reached

		// **PATROT we now use this for length
		seqlen=seq_patternSet.length[seq_activePattern][i];
		if(!seqlen)
			seqlen=16;

		if((seq_stepIndex[i]) >= seqlen || (seq_stepIndex[i] & 0x3f) == 0)
		{
			//if end is reached reset track to step 0
			seq_stepIndex[i] = 0;
		}



		//if track is not muted
		if(!(seq_mutedTracks & (1<<i) ) )
		{
/*
			//if main step (associated with current substep) is active
			if(seq_intIsMainStepActive(i,seq_stepIndex[i]/8,seq_activePattern)) {
*/
			//we only have mainsteps now
			if(seq_intIsMainStepActive(i,seq_stepIndex[i],seq_activePattern)) {



				// **RECORD if we are in erase mode (shift clear while record and playing)
				// and this is the active track on the front, we erase the note value
				if(seq_eraseActive && i==frontParser_activeTrack) {
					//sub steps are ditched, so only remove main step
					seq_setMainStep(seq_activePattern, frontParser_activeTrack, seq_stepIndex[i],0);
					//if steps are shown, (active bar) turn off LED
					if( (seq_stepIndex[i] >= (menu_activeBar*16)) && (seq_stepIndex[i] < (menu_activeBar+1)*16))
					{
						led_setValue(0,led_idx2StepLed(seq_stepIndex[i]%16));
					}
				} else
				// if sub-step is active
				//if(seq_intIsStepActive(i,seq_stepIndex[i],seq_activePattern))
				{
					//PROBABILITY

#if 0
					//every 8th step a new random value is generated
					//thus every sub step block has only one random value to compare against
					//allows randomisation of rolls by chance

					if((seq_stepIndex[i] & 0x07) == 0x00) //every 8th step
					{
						seq_rndValue[i] = GetRngValue()&0x7f;
					}
#else
					//every step gets a random value
					//GetRngValue()&0x7f;
					seq_rndValue[i].prev = seq_rndValue[i].current;
					seq_rndValue[i].current = seq_rndValue[i].next;
					seq_rndValue[i].next = GetRngValue()% 100 + 1; //value between 0 and 100
#endif

#if 0 //moved to flam/shift processing routine

					if( (seq_rndValue[i]) <= seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].prob )
					{
						//only play if timeshift is not active
						if(seq_patternSet.seq_stepTiming[seq_activePattern][i][seq_stepIndex[i]] < 6)
						{
							if(seq_stepToSkip[i] == seq_stepIndex[i])
							{
								//freshly recorded step, skip and reset seq_stepToSkip[]
								seq_stepToSkip[i] = -1;
							}
							else
							{
								const uint8_t vol = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].volume&STEP_VOLUME_MASK;
								const uint8_t note = seq_patternSet.seq_subStepPattern[seq_activePattern][i][seq_stepIndex[i]].note;
								seq_triggerVoice(i,vol,note);
							}
						}
					}
#endif
				} // if sub step is active
			} // if main step is active
		} // if this track is not muted



}
	seq_queueStepNotes();
	//the held rolls' hits of this step
	for(uint8_t v=0;v<NUM_TRACKS;v++)
	{
		if(seq_rollState & (1<<v))
			seq_queueRollHits(v, 0.f);
	}
	//following an external clock the step arrives in the middle of a block: its hits sound now
	if(seq_getExtSync())
		seq_firePending();

	//TODO can maybe be combined with pattern change follow above?!
	//step display follows bars
	if(activeParameters[PAR_FOLLOW] & FOLLOW_BAR ) {

		//only update if bar changed
		if(menu_activeBar != seq_stepIndex[menu_getActiveVoice()]/16)
		{
			if( menu_activePage < PATTERN_LOAD_PAGE || menu_activePage > SONG_MODE_PAGE)
			{



				menu_setShownPattern(seq_activePattern);
				led_clearSequencerLeds();
				//query current sequencer step states and light up the corresponding leds
				const uint8_t trackNr = menu_getActiveVoice(); //max 6 => 0x6 = 0b110
				//uint8_t patternNr = menu_getViewedPattern(); //max 7 => 0x07 = 0b111
				//uint8_t value = (uint8_t)((trackNr<<4) | (patternNr&0x7));


				//update menu_activeBar and Bar LEDs
				menu_activeBar = seq_stepIndex[trackNr]/16;

				led_updateSeqLeds();

			}
		}
	}

	//send message to frontpanel
	//to display the current step
	/*
	uart_sendFrontpanelByte(FRONT_STEP_LED_STATUS_BYTE);
	uart_sendFrontpanelByte(FRONT_CURRENT_STEP_NUMBER_CC);
	uart_sendFrontpanelByte(seq_stepIndex[frontParser_activeTrack]);
	*/
	const uint8_t shownPattern = menu_getViewedPattern();
	const uint8_t playedPattern = menu_playedPattern;

	const uint8_t stepIdx = seq_stepIndex[frontParser_activeTrack];

	const uint8_t isOnActivePage = (stepIdx >= menu_activeBar*16) && (stepIdx < (menu_activeBar*16+16));

	if( (shownPattern == playedPattern) && (isOnActivePage)) {
		//only update chaselight LED when in step edit mode
		if( (menu_activePage < MENU_MIDI_PAGE) || menu_activePage == PERF_PAGE ||menu_activePage == SEQ_PAGE || menu_activePage == EUKLID_PAGE) {
			led_setActive_step(stepIdx%16);
		}
	} else {
		led_clearActive_step();
	}

	// check mtc, which might stop the sequencer if we haven't seen one in a while
	midiParser_checkMtc();

}
//------------------------------------------------------------------------------
uint8_t seq_getExtSync()
{
	return seq_isSyncExternal;
}
//------------------------------------------------------------------------------
void seq_setExtSync(uint8_t isExt)
{
	seq_isSyncExternal = isExt;
}
//------------------------------------------------------------------------------
void seq_armAutomationStep(uint8_t stepNr, uint8_t track,uint8_t isArmed)
{
	if(isArmed) {
		seq_armedArmedAutomationStep 	= stepNr;
		seq_armedArmedAutomationTrack 	= track;
	} else {
		seq_armedArmedAutomationStep 	= -1;
		seq_armedArmedAutomationTrack 	= -1;
	}
}
//------------------------------------------------------------------------------
void seq_setDeltaT(float delta)
{
	seq_deltaT = delta;
}
//------------------------------------------------------------------------------

/*This is called from IRQ handler when an external clock tick is received
 * master steps are used to keep the sync with the external clocks
 * a master step is a step that is directly triggered by the external clock signal.
 * non master steps are derived from the internaly calculated phase accumulator.
 * spacing is defined by the prescaler value
 * - with 32ppq every step is a master step
 * - with 4ppq only every 8th step is a master step
 * We set the next step index to a value - 1 because seq_nextStep() will
 * increment the value itself
 */
void seq_triggerNextMasterStep(uint8_t stepSize)
{
	uint8_t i, sn, len;
	for(i=0;i<NUM_TRACKS;i++) {
		len = seq_patternSet.length[seq_activePattern][i];
		if(!len) // length of 0 means length of 16 (since we are using 4 bits)
			len=16;
		len *= 8; // need length in steps

		if(seq_lastMasterStep[i] == 0) // need to set it so next step will inc it to 0
			sn=len-1; // set to last step before wrap around, effectively 0
		else
			sn=seq_lastMasterStep[i]-1; // adjust for seq_nextStep

		// establish the next step for this track for the sequencer
		seq_stepIndex[i] = sn;

		// save the position where we will trigger on the next external clock tick.
		// wrap around if we would exceed our track length
		seq_lastMasterStep[i] += stepSize;
		if(seq_lastMasterStep[i] >= len)
			seq_lastMasterStep[i] -= len;

		//set time to next step to zero, forcing the sequencer to process the next step now
		seq_setDeltaT(-1);
	}
}
//------------------------------------------------------------------------------
/*
void seq_resetDeltaAndTick()
{
	//if there are unplayed steps jump over them
	while(!seq_isNextStepSyncStep())
	{
		seq_nextStep();
	}

	//is shuffle delay necessary?

	if(seq_shuffle != 0)
	{
		//seq_deltaT = 0;^

		seq_deltaT 	= (1000*60.f)/seq_tempo; 	//bei 12 = 500ms = time for one beat (float, as the other two: the truncation cost up to 0.45 % at 274 bpm)
		seq_deltaT /= 96.f; //we run the internal clock at 96ppq -> seq clock 32 ppq == prescaler 3, midi clock 24 ppq == prescale 4
		seq_deltaT *= 4;

		seq_lastTick = systick_ticks;

		uint8_t stepInHalfBeat = seq_masterStepCnt&0xf;
		const float shuffleFactor = seq_shuffleTable[stepInHalfBeat] * seq_shuffle;
		const float originalDeltaT = seq_deltaT;

		seq_deltaT = shuffleFactor * originalDeltaT * 16.f;
		seq_lastShuffle = shuffleFactor;

		if(seq_deltaT <= 0)
		{
			seq_nextStep();
			seq_lastTick = systick_ticks;
			seq_calcDeltaT(seq_tempo);
		} else
		{
			seq_delayedSyncStepFlag =1;
		}

		seq_prescaleCounter = 0;

	}
	else
	{
		//play next sync step
		seq_nextStep();

		seq_lastTick = systick_ticks;
		seq_calcDeltaT(seq_tempo);

		seq_prescaleCounter = 0;
	}


}
*/
//------------------------------------------------------------------------------
/** call periodically to check if the next step has to be processed */

/*IN_ITCM_RAM*/ void seq_tick()
{
	if(seq_getExtSync())
	{
#if !HANDLE_SEQ_IN_CLOCK_INTERRUPT
		//analog clock: every pulse starts a step, on time (swing is in the note times)
		while(trigger_clockReceived != 0)
		{
			trigger_clockReceived--;
			seq_nextStep();
			seq_prescaleCounter=0;
			seq_MidiSyncQueue(0);
		}

		//the internal clock fills in the 24 ticks between the external steps (and waits after
		//the 24th for the next external step)
		if(systick_ticks-seq_lastTick >= seq_deltaT)
		{
			const float rest = systick_ticks-seq_lastTick - seq_deltaT;
			seq_lastTick = systick_ticks;
			if(seq_tempo !=0)
			{
				seq_calcDeltaT(seq_tempo);
			}
			seq_deltaT = seq_deltaT - rest;

			if(seq_prescaleCounter!=24)
			{
				seq_tickLateness = rest;
				seq_processTick(0);		//no MIDI clock out while following an external clock
				seq_tickLateness = 0;
				seq_prescaleCounter++;
			}
		}
#endif
	}
	else
	{
		if(seq_deltaT == -1)
		{
			seq_deltaT = 32000;
			seq_nextStep();

			return;
		}

		if(systick_ticks-seq_lastTick >= seq_deltaT)
		{
			const float rest = systick_ticks-seq_lastTick - seq_deltaT;
#if CPU_METER
			if(seq_isRunning())
				cpuMeter_seqTickLate(rest);
#endif
			seq_lastTick = systick_ticks;
			seq_calcDeltaT(seq_tempo);
			seq_deltaT = seq_deltaT - rest;

			seq_tickLateness = rest;
			if(seq_prescaleCounter == 0)
			{
				seq_nextStep();
			}
			seq_processTick(1);
			seq_tickLateness = 0;

			seq_prescaleCounter++;
			if(seq_prescaleCounter>=24)
			{
				seq_prescaleCounter=0;
			}
		}
	}

	//the hits between the ticks (swing, micro-shift)
	seq_firePending();
}

//------------------------------------------------------------------------------
void seq_setQuantisation(uint8_t value)
{
	seq_quantisation = value;
}
//------------------------------------------------------------------------------
void seq_toggleStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	if((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume&STEP_ACTIVE_MASK)==0)
	{
		seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume |= STEP_ACTIVE_MASK;
	} else {
		seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
	}
}
//------------------------------------------------------------------------------
void seq_toggleMainStep(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	const uint8_t bar 	= stepNr / 16;
	const uint8_t shift = stepNr - bar*16;
	seq_patternSet.seq_mainSteps[bar][patternNr][voice] ^= (1<<shift);
}
//------------------------------------------------------------------------------
void seq_setMainStep(uint8_t patternNr, uint8_t voice, uint8_t stepNr, uint8_t onOff)
{
	const uint8_t bar 	= stepNr / 16;
	const uint8_t shift = stepNr - bar*16;

	if(onOff)
	{
		seq_patternSet.seq_mainSteps[bar][patternNr][voice] |= (1<<shift);
	}
	else
	{
		seq_patternSet.seq_mainSteps[bar][patternNr][voice] &= ~(1<<shift);
	}
}
//------------------------------------------------------------------------------
// this appears unused
//void seq_setStep(uint8_t voice, uint8_t stepNr, uint8_t onOff)
//{
//	if(onOff)
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume |= STEP_ACTIVE_MASK;
//	}
//	else
//	{
//		seq_patternSet.seq_subStepPattern[seq_activePattern][voice][stepNr].volume &= ~STEP_ACTIVE_MASK;
//	}
//}
//------------------------------------------------------------------------------
uint8_t seq_isRunning() {
	return seq_running;
}
//------------------------------------------------------------------------------
void seq_resetPosition()
{
	led_resetBlinkTimer();

	// reset all track rotations to 0. We are not saving rotated value. it's a performance tool.
	/*
	uint8_t i;
	for(i=0;i<NUM_TRACKS;i++) {
		seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate=0;
		// let the front know this is happening


		parameter_values[PAR_TRACK_ROTATION] = seq_getTrackRotation(i);
		menu_repaint(); // TODO we might not need this
	}
	*/

	//reset song position bar counter
	seq_queueClear();
	seq_stepGrid = seq_now();
//	seq_barCounter = 0;
	seq_masterStepCnt = 0;
	seq_songPositionPointer = -1;
	seq_songRepeatsLeft = 0;
	//so the next seq_tick call will trigger the next step immediately
	seq_deltaT = 0;
	seq_sendRealtime(MIDI_STOP);

	// send notes off on all channels that have notes playing and reset our bitmap to reflect that
	voiceControl_noteOff(0xFF);

	trigger_reset(0);
	//trigger_allOff();


	// if mtc was doing it's thing, tell it to stop it.
	midiParser_checkMtc();


	// set start points back to default (happens on start and stop. needs to happen on start
		// in case the user has entered a rotate value while stopped)
		seq_setStepIndexToStart();
}
//------------------------------------------------------------------------------
void seq_setRunning(uint8_t isRunning)
{
	seq_running = isRunning;
	//jump to 1st step if sequencer is stopped
	if(!seq_running)
	{
		preset_touchTimer(PRESET_PAT_TIMER);
		//moved to seq_resetPosition()
	} else {
		//abort pattern save timer, since seq is running again...
		preset_touchTimer(PRESET_STOP_PAT_TIMER);

		trigger_clockReceived = 0;
		seq_prescaleCounter = 0;
		seq_queueClear();
		seq_stepGrid = seq_now();
		seq_sendRealtime(MIDI_START);
		trigger_reset(1);
	}

	/*
	// set start points back to default (happens on start and stop. needs to happen on start
	// in case the user has entered a rotate value while stopped)
	seq_setStepIndexToStart();
	*/
	seq_setStepIndexToStart();

}
//------------------------------------------------------------------------------
uint8_t seq_isSongModeEnabled(void)
{
	return seq_songModeEnabled;
}
//------------------------------------------------------------------------------
void seq_setSongModeEnabled(uint8_t onOff)
{
	seq_songModeEnabled = onOff;
	if(onOff)
	{
		seq_songPositionPointer = -1; //because nextStep() will first increase it
		seq_songRepeatsLeft = 0;
		//the song page shows where the song starts
		menu_songMenuPosition = 0;
		//the first step starts the song (at slot 0) only if no other pattern change is waiting:
		//that one would play first and the song page showed position 255 meanwhile
		if(!seq_running)
			seq_pendingPattern = seq_activePattern;
	}
}
//------------------------------------------------------------------------------
void seq_addPatternToSong(uint8_t patternNr)
{
	seq_activeSong[menu_songMenuPosition] = patternNr;
	if(menu_songMenuPosition < (MAX_SONG_LENGTH-1))
	{
		menu_songMenuPosition++;
	}
}
//------------------------------------------------------------------------------
void seq_deleteRow()
{
	 //seq_songData[NUM_KIT_SLOTS][MAX_SONG_LENGTH];
	for(int i=menu_songMenuPosition; i < MAX_SONG_LENGTH-1 ;i++)
	{
		seq_activeSong[i] = seq_activeSong[i+1];
	}

}
//------------------------------------------------------------------------------
void seq_insertRow()
{
	//for(int i=menu_songMenuPosition; i < MAX_SONG_LENGTH-1 ;i++)
	for(int i=MAX_SONG_LENGTH-1; i > menu_songMenuPosition ;i--)
	{
		seq_activeSong[i] = seq_activeSong[i-1];
	}
}
//------------------------------------------------------------------------------
static uint8_t seq_intIsStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	//return ((seq_patternSet.seq_subStepPattern[patternNr][voice][stepNr].volume & STEP_ACTIVE_MASK) > 0);

	//TODO since we don't have substeps anymore, we can probably ditch the whole active flag/mask thing
	return 1;
}
// above will be inlined, below is for ext linkage
uint8_t seq_isStepActive(uint8_t voice, uint8_t stepNr, uint8_t patternNr)
{
	return seq_intIsStepActive(voice,stepNr,patternNr);
}

//------------------------------------------------------------------------------
static uint8_t seq_intIsMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
	const uint8_t bar = mainStepNr / 16;
	const uint8_t shift = mainStepNr - bar*16;
	return (seq_patternSet.seq_mainSteps[bar][pattern][voice] & (1<<shift)) > 0;
}
// above is inlined below for ext linkage
uint8_t seq_isMainStepActive(uint8_t voice, uint8_t mainStepNr, uint8_t pattern)
{
	return seq_intIsMainStepActive(voice, mainStepNr, pattern);
}

//------------------------------------------------------------------------------
IN_ITCM_RAM void seq_setMute(uint8_t trackNr, uint8_t isMuted)
{
	if(trackNr==7)
	{
		//unmute all
		seq_mutedTracks = 0;
	} else {
		//mute/unmute tracks
		if(isMuted) {
			//mute track
			seq_mutedTracks |= (1<<trackNr);
			// turn off the midi note that may be playing on that track
			voiceControl_noteOff(midi_MidiChannels[trackNr]);
		} else {
			//unmute track
			seq_mutedTracks &= ~(1<<trackNr);
		}
	}
};
//------------------------------------------------------------------------------
uint8_t seq_isTrackMuted(uint8_t trackNr)
{
	if(seq_mutedTracks & (1<<trackNr) )
	{
		return 1;
	}
	return 0;
}
//------------------------------------------------------------------------------
// given a pattern and a track:
// this sends the main step info (which main steps are on/off) in addition
// to the length of the track
/*
void seq_sendMainStepInfoToFront(uint16_t stepNr)
{
	//the absolute number of patterns
	const uint8_t currentPattern	= stepNr / 7;
	const uint8_t currentTrack  	= stepNr - currentPattern*7;

	uint16_t dataToSend = seq_patternSet.seq_mainSteps[currentPattern][currentTrack];

	uart_sendFrontpanelSysExByte(  dataToSend	  & 0x7f); //1st 7 bit
	uart_sendFrontpanelSysExByte( (dataToSend>>7) & 0x7f); //2nd 7 bit
	uart_sendFrontpanelSysExByte( (dataToSend>>14)& 0x7f); //last 2 bit

	// send the track length
	uart_sendFrontpanelSysExByte( seq_patternSet.seq_patternLengthRotate[currentPattern][currentTrack].length);

}
*/
//--------------------------------------------------------------------
/** this one is more complicated than the 14 bit upper/lower nibble when transmitting the requested step number via SysEx.
 * because we have to transmit seven 8-bit values that make up the StepStruct we have to pack the data clever into the 7-bit
 * SysEx packets.
 *
 * first we send the lower 7-bit of all 7 values
 * then we transmit an additional 7-bit value containing all the MSBs from the previous seven values.
 *
 * 1st - (value1 & 0x7f)
 * 2nd - (value2 & 0x7f)
 * 3rd - (value3 & 0x7f)
 * 4th - (value4 & 0x7f)
 * 5th - (value5 & 0x7f)
 * 6th - (value6 & 0x7f)
 * 7th - (value7 & 0x7f)
 *
 * 8th - (0 MSB7 MSB6 MSB5 MSB4 MSB3 MSB2 MSB1)
 */
void seq_sendStepInfoToFront(uint16_t stepNr)
{

	//decode the step number
	//the step number is between 0 and NUMBER_STEPS*NUM_TRACKS*NUM_PATTERN = 64*7*8 = 7168 steps
	// stepNr / 64 = absolute number of patterns (each track has 8 patterns * 7 tracks)
	// numPatterns / 8 = the current track

	//the absolute number of patterns
	const uint8_t absPat 			= stepNr/NUM_STEPS;
	const uint8_t currentTrack 		= absPat / 8;
	const uint8_t currentPattern 	= absPat - currentTrack*8;
	const uint8_t currentStep		= stepNr - absPat*NUM_STEPS;

	//encode the data and send it back
	Step *dataToSend = &seq_patternSet.seq_subStepPattern[currentPattern][currentTrack][currentStep];

	uart_sendFrontpanelSysExByte(dataToSend->volume	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->prob	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->note	& 0x7f);

	uart_sendFrontpanelSysExByte(dataToSend->param1Nr	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->param1Val	& 0x7f);

	uart_sendFrontpanelSysExByte(dataToSend->param2Nr	& 0x7f);
	uart_sendFrontpanelSysExByte(dataToSend->param2Val	& 0x7f);

	//now the MSBs from all 7 values
	uart_sendFrontpanelSysExByte( 	(((dataToSend->volume 	& 0x80)>>7) |
									((dataToSend->prob	 	& 0x80)>>6) |
									((dataToSend->note	 	& 0x80)>>5) |
									((dataToSend->param1Nr	& 0x80)>>4) |
									((dataToSend->param1Val	& 0x80)>>3) |
									((dataToSend->param2Nr	& 0x80)>>2) |
									((dataToSend->param2Val	& 0x80)>>1))&0x7f
									);
}
//-------------------------------------------------------------------------------
void seq_setRoll(uint8_t voice, uint8_t onOff)
{
	if(voice >= 7) return;

	if(onOff) {
		seq_rollState |= (1<<voice);
		//if seq is stopped, always use oneshot mode
		if((seq_rollInterval == 0) || (seq_isRunning() == 0) ) {
			//trigger one shot
			seq_triggerVoice(voice,ROLL_VOLUME,SEQ_DEFAULT_NOTE);
			//record roll notes
			seq_addNote(voice,ROLL_VOLUME,SEQ_DEFAULT_NOTE);
		}
		else if(seq_queuePrimed)
		{
			//the rest of the current step at once: the hits that haven't sounded yet (a swung
			//hit by its swung time)
			const float elapsed = (float)(int32_t)(seq_now() - seq_stepGrid) / SEQ_TIME_SCALE;
			seq_queueRollHits(voice, elapsed < 0.f ? 0.f : elapsed);
		}
	} else {
		seq_rollState &= ~(1<<voice);
		seq_removeRollHits(voice);
	}
};
//--------------------------------------------------------------------------------
//rate: the index in the menu's roll speeds (One, 1/1, 1/2, 1/4, 1/8, 8T, 16, 16T, 32, 32T, 64)
void seq_setRollRate(uint8_t rate)
{
	//96 ppq ticks between the hits; 0: one shot
	static const uint16_t intervals[] = { 0, 384, 192, 96, 48, 32, 24, 16, 12, 8, 6 };
	seq_rollInterval = rate < sizeof(intervals)/sizeof(intervals[0]) ? intervals[rate] : 24;
}
//------------------------------------------------------------------------
/** quantize a step to the seq_quantisation value*/
/*
#define QUANT(x) (NUM_STEPS/x)
static int8_t seq_quantize(int8_t step)
{
	uint8_t quantisationMultiplier=1;
	switch(seq_quantisation)
	{
	case QUANT_8:
		quantisationMultiplier = QUANT(8);
		break;

	case QUANT_16:
		quantisationMultiplier = QUANT(16);
		break;

	case QUANT_32:
		quantisationMultiplier = QUANT(32);
		break;

	case QUANT_64:
		quantisationMultiplier = QUANT(64);
		break;

	case NO_QUANTISATION:
	default:
		return step;
		break;
	}

	//now calc the quantisation
	float frac = step/(float)quantisationMultiplier;
	int8_t itg = (int8_t)frac;
	frac = frac - itg;

	if(frac>=0.5f)
	{
		return ((itg + 1)*quantisationMultiplier)&0x7f;
	}
	return itg*quantisationMultiplier;
}
*/

//------------------------------------------------------------------------
uint8_t seq_isAutomationStepArmed()
{
	if(seq_armedArmedAutomationStep	!= -1) return 1;
	return 0;
}
//------------------------------------------------------------------------
void seq_recordAutomation(uint8_t voice, uint8_t dest, uint8_t value)
{
	//(the step index is -1 while stopped)
	if(seq_recordActive && seq_stepIndex[voice] >= 0)
	{
		//uint8_t quantizedStep = seq_quantize(seq_stepIndex[voice]);
		const uint8_t step = seq_stepIndex[voice];

		//only record to active steps
		/*

		if( seq_intIsMainStepActive(voice,quantizedStep/8,seq_activePattern) &&
				seq_intIsStepActive(voice,quantizedStep,seq_activePattern))
			*/
		if( seq_intIsMainStepActive(voice,step,seq_activePattern) )
		{
      if(seq_activeAutomTrack == 0) {
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param1Nr = dest;
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param1Val = value;
      } else {
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param2Nr = dest;
        seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].param2Val = value;
      }
      
      // seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].paramNr[seq_activeAutomTrack] = dest;
      // seq_patternSet.seq_subStepPattern[seq_activePattern][voice][step].paramVal[seq_activeAutomTrack] = value;
      
		}
	}

	if( (seq_armedArmedAutomationStep	!= -1) && (seq_armedArmedAutomationTrack != -1) )
	{
		//step button is held down
		//-> set step automation parameters
		if(seq_activeAutomTrack == 0) {
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param1Nr = dest;
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param1Val = value;
		} else {
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param2Nr = dest;
			seq_patternSet.seq_subStepPattern[seq_activePattern]
			                                  [seq_armedArmedAutomationTrack]
			                                   [seq_armedArmedAutomationStep].param2Val = value;
		}
	}
}

PatternSet* get_seq_patternSet_p(void)
{
  return &seq_patternSet;
}

//------------------------------------------------------------------------
//live recording: is a hit now nearer to the next step than to the current one? The steps start
//on the straight grid; a step is heard at its start plus its swing delay, so the boundary is the
//middle between the current and the next step's swung position. (The stock
//seq_getElapsedStepTimePercent() read the tick counter, which wraps to 0 % in the last tick of a
//step, before the next step starts: hits played slightly early were recorded one step too early.)
static uint8_t seq_isHitNearerNextStep(const uint8_t trackNr)
{
	//after start, before the first step: the first step is the only one
	if(seq_stepIndex[trackNr] < 0)
		return 1;
	const float current = seq_swingDelay(0, seq_masterStepCnt);
	const float next = 24.f*seq_tickLength() + seq_swingDelay(0, (uint8_t)(seq_masterStepCnt+1));
	const float elapsed = (float)(int32_t)(seq_now() - seq_stepGrid) / SEQ_TIME_SCALE;
	return elapsed >= (current + next)/2.f;
}
//------------------------------------------------------------------------
void seq_addNote(uint8_t trackNr,uint8_t vel, uint8_t note)
{
	uint8_t targetPattern;
	Step *stepPtr;
	//only record notes when seq is running and recording
	if(seq_running && seq_recordActive)
	{
		//const int8_t quantizedStep = seq_quantize(seq_stepIndex[trackNr]);

		uint8_t step = seq_stepIndex[trackNr];
		int8_t quantizedStep = seq_isHitNearerNextStep(trackNr) ? seq_stepIndex[trackNr] + 1 : seq_stepIndex[trackNr];



		// **RECORD fix for recording across patterns
		if(step==0 && seq_stepIndex[trackNr] > (NUM_STEPS/2)) {
			// this means that we hit a note in 2nd half of the bar and quantization pushed
			// the note to position 0 of the next bar.
			// need to see if there is about to be a pattern change so that the note
			// ends up on 0 of the next pattern
			targetPattern=seq_determineNextPattern(0);

		} else
			targetPattern=seq_activePattern;

		//special care must be taken when recording midi notes!
		//since per default the 1st substep of a mainstep cluster is always active
		//we will get double notes when a substep other than ss1 is recorded
		/*
		if(!seq_intIsMainStepActive(trackNr, step, targetPattern))
		{
			//if the mainstep is not active, we clear the 1st substep
			//to prevent double notes while recording
			seq_patternSet.seq_subStepPattern[targetPattern][trackNr][(quantizedStep/8)*8].volume 	&= ~STEP_ACTIVE_MASK;
		}
		*/

		//limit to track length (0 = 16, see seq_getTrackLength)
		const uint8_t trackLength = seq_patternSet.length[targetPattern][trackNr];
		quantizedStep = quantizedStep%(trackLength ? trackLength : 16);
		if(step != quantizedStep)
		{
			step = quantizedStep;
			seq_stepToSkip[trackNr] = step;
		}

		//set the current step in the requested track active
		stepPtr=&seq_patternSet.seq_subStepPattern[targetPattern][trackNr][step];
		stepPtr->note 		= note;				// note (was SEQ_DEFAULT_NOTE)
		stepPtr->volume		= vel;				// new velocity
		stepPtr->prob		= 100;				// 100% probability
		stepPtr->volume 	|= STEP_ACTIVE_MASK;

		//activate corresponding main step
		seq_setMainStep(targetPattern, trackNr, step,1);

		if( (frontParser_shownPattern == targetPattern) && ( frontParser_activeTrack == trackNr) && (menu_activeBar == (step/16)) )
		{

			if(menu_activePage != PERF_PAGE) //do not show active steps on perf. page
			{
				//limit to 16 steps
				uint8_t stepNr = (uint8_t)(step%16);
				uint8_t led = 0;
				switch(stepNr)
				{
					case 0:
						led = LED_STEP1;
						break;
					case 1:
						led = LED_STEP2;
						break;
					case 2:
						led = LED_STEP3;
						break;
					case 3:
						led = LED_STEP4;
						break;
					case 4:
						led = LED_STEP5;
						break;
					case 5:
						led = LED_STEP6;
						break;
					case 6:
						led = LED_STEP7;
						break;
					case 7:
						led = LED_STEP8;
						break;
					case 8:
						led = LED_STEP9;
						break;
					case 9:
						led = LED_STEP10;
						break;
					case 10:
						led = LED_STEP11;
						break;
					case 11:
						led = LED_STEP12;
						break;
					case 12:
						led = LED_STEP13;
						break;
					case 13:
						led = LED_STEP14;
						break;
					case 14:
						led = LED_STEP15;
						break;
					case 15:
						led = LED_STEP16;
						break;


				}
				led_setValue(1,led);
			}
		}
	}
}

//------------------------------------------------------------------------
// **RECORD erase a main step and all it's sub steps on the active pattern
// for the specified voice
/*
static void seq_eraseStepAndSubSteps(const uint8_t voice, const uint8_t mainStep)
{
	uint8_t i;
	// turn off the main step
	seq_setMainStep(seq_activePattern, voice, mainStep,0);

	// turn off all substeps
	for(i=(uint8_t)(mainStep*8);i<(uint8_t)((mainStep+1)*8);i++) {
		seq_resetNote(&seq_patternSet.seq_subStepPattern[seq_activePattern][voice][i]);
	}

	// first substep needs to be made active
	seq_patternSet.seq_subStepPattern[seq_activePattern][voice][(uint8_t)(mainStep*8)].volume |= STEP_ACTIVE_MASK;

	//if( (frontParser_shownPattern == seq_activePattern) && ( frontParser_activeTrack == voice) )
	//{
		// todo if the pattern is shown on the front, update the leds
		// 		figure out. Right now it clears the LED's after shift is released... which is fine for now.
	//}

}
*/

//------------------------------------------------------------------------
void seq_setRecordingMode(uint8_t active)
{
	seq_recordActive = active;
}

void seq_setErasingMode(uint8_t active)
{
	seq_eraseActive = active;
}

//------------------------------------------------------------------------------
// reset a step to it's default state
static void seq_resetNote(Step *step)
{
	step->note 		= SEQ_DEFAULT_NOTE;
	step->param1Nr 	= NO_AUTOMATION;
	step->param1Val = 0;
	step->param2Nr	= NO_AUTOMATION;
	step->param2Val	= 0;
	step->prob		= 100;
	step->volume	= 127; // clears active bit as well
}
//------------------------------------------------------------------------------
void seq_clearTrack(uint8_t trackNr, uint8_t pattern)
{
	int k;
	for(k=0;k<NUM_STEPS;k++)
	{
		seq_resetNote(&seq_patternSet.seq_subStepPattern[pattern][trackNr][k]);

		seq_patternSet.seq_stepTiming[pattern][trackNr][k].flam = 0;
		seq_patternSet.seq_stepTiming[pattern][trackNr][k].shift = 7;   // 7 = no shift
		seq_patternSet.arp[pattern][trackNr][k] = ARP_REPEAT;

		//every 1st step in a substep pattern active
		if( (k%8) == 0)
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].volume |= STEP_ACTIVE_MASK ;
	}

	// all main steps off for this track
	for(uint8_t i=0;i<NUM_BARS;i++)	seq_patternSet.seq_mainSteps[i][pattern][trackNr] = 0;

	//**PATROT all pattern rotations off and all patterns set to length 16
	seq_patternSet.length[pattern][trackNr]=16;

}
//------------------------------------------------------------------------------
void seq_clearPattern(uint8_t pattern)
{
	int i;
	for(i=0;i<NUM_TRACKS;i++)
		seq_clearTrack(i, pattern);
}

//------------------------------------------------------------------------------
void seq_clearAutomation(uint8_t trackNr, uint8_t pattern, uint8_t automTrack)
{
	int k;

	if(automTrack==0)
	{
		for(k=0;k<NUM_STEPS;k++)
		{
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Nr 	= NO_AUTOMATION;
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param1Val 	= 0;
		}
	} else {
		for(k=0;k<NUM_STEPS;k++)
		{
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Nr		= NO_AUTOMATION;
			seq_patternSet.seq_subStepPattern[pattern][trackNr][k].param2Val	= 0;
		}
	}
}
//------------------------------------------------------------------------------
void seq_copyTrack(uint8_t srcNr, uint8_t dstNr, uint8_t pattern)
{
	// todo this fn and the next are very similar. Add a new fn that takes src, dest track and src, dest pat
	// and then have these two fn's call it instead.
	int k;
	Step *src, *dst;
	for(k=0;k<NUM_STEPS;k++)
	{
		dst=&seq_patternSet.seq_subStepPattern[pattern][dstNr][k];
		src=&seq_patternSet.seq_subStepPattern[pattern][srcNr][k];
		dst->note		= src->note;
		dst->param1Nr 	= src->param1Nr;
		dst->param1Val 	= src->param1Val;
		dst->param2Nr	= src->param2Nr;
		dst->param2Val	= src->param2Val;
		dst->prob		= src->prob;
		dst->volume		= src->volume;

		seq_patternSet.seq_stepTiming[pattern][dstNr][k] = seq_patternSet.seq_stepTiming[pattern][srcNr][k];
		seq_patternSet.arp[pattern][dstNr][k] = seq_patternSet.arp[pattern][srcNr][k];
	}

	// copy which main steps are on/off
	for(uint8_t i=0;i<NUM_BARS;i++)	seq_patternSet.seq_mainSteps[i][pattern][dstNr] = seq_patternSet.seq_mainSteps[i][pattern][srcNr];

	// copy length and rotation offset from source track
	seq_patternSet.length[pattern][dstNr] = seq_patternSet.length[pattern][srcNr];


}
//------------------------------------------------------------------------------
uint8_t seq_patternHasSteps(uint8_t pattern)
{
	for(uint8_t bar=0;bar<NUM_BARS;bar++)
	{
		for(uint8_t track=0;track<NUM_TRACKS;track++)
		{
			if(seq_patternSet.seq_mainSteps[bar][pattern][track])
				return 1;
		}
	}
	return 0;
}
//------------------------------------------------------------------------------
void seq_copyPattern(uint8_t src, uint8_t dst)
{
	int k,j;
	Step *psrc, *pdst;
	for(j=0;j<NUM_TRACKS;j++)
	{
		for(k=0;k<NUM_STEPS;k++)
		{
			pdst=&seq_patternSet.seq_subStepPattern[dst][j][k];
			psrc=&seq_patternSet.seq_subStepPattern[src][j][k];
			pdst->note			= psrc->note;
			pdst->param1Nr 		= psrc->param1Nr;
			pdst->param1Val 	= psrc->param1Val;
			pdst->param2Nr		= psrc->param2Nr;
			pdst->param2Val		= psrc->param2Val;
			pdst->prob			= psrc->prob;
			pdst->volume		= psrc->volume;

			seq_patternSet.seq_stepTiming[dst][j][k] = seq_patternSet.seq_stepTiming[src][j][k];
			seq_patternSet.arp[dst][j][k] = seq_patternSet.arp[src][j][k];
		}

		for( uint8_t i=0;i<NUM_BARS;i++)
		{
			seq_patternSet.seq_mainSteps[i][dst][j] = seq_patternSet.seq_mainSteps[i][src][j];
		}

		// copy length and rotation offset from source pattern for the track
		seq_patternSet.length[dst][j] = seq_patternSet.length[src][j];

		seq_patternSet.shuffle[dst] = seq_patternSet.shuffle[src];
		seq_patternSet.kitNr[dst] = seq_patternSet.kitNr[src];
	}
}
//------------------------------------------------------------------------------
void seq_setActiveAutomationTrack(uint8_t trackNr)
{
	seq_activeAutomTrack = trackNr;
}

uint8_t seq_getActiveAutomationTrack(void)
{
  return seq_activeAutomTrack;
}
//------------------------------------------------------------------------------
/*
static uint8_t seq_isNextStepSyncStep()
{
	if(seq_delayedSyncStepFlag)
	{
		seq_delayedSyncStepFlag = 0;
		seq_prescaleCounter = 0;
		return 0;
	}
	if( ((seq_stepIndex[0] & 0x3) % 4) == 3) {
		return 1;
	}
	return 0;
}
*/
//------------------------------------------------------------------------------

void seq_midiNoteOff(uint8_t chan)
{
	uint8_t i;
	MidiMsg msg;

	// we are not filtering according to tx filter because they might have turned that
	// setting on while a note was sustaining

	msg.bits.length=2;
	msg.data2=0;

	if(chan==0xff) { // all notes off
		for(i=0; i<16; i++)
			if((1<<i) & midi_notes_on) {
				msg.status=	NOTE_OFF | i;
				msg.data1=midi_chan_notes[i];
				seq_sendMidi(msg);
			}
		// reset all
		midi_notes_on=0;
		return;
	}
	// The proper way to do a note off is with 0x80. 0x90 with velocity 0 is also used, however I think there is still
	// synth gear out there that doesn't recognize that properly.
	if((1<<chan) & midi_notes_on) {
		msg.status=	NOTE_OFF | chan;
		msg.data1=midi_chan_notes[chan];
		seq_sendMidi(msg);
		// turn off our knowledge of that note playing
		midi_notes_on &= (~(1<<chan));
	}
}

static void seq_sendRealtime(const uint8_t status)
{
	static MidiMsg msg = {0};
	// FILT filter out realtime msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_SYSTEM<<4))==0)
		return;
	msg.status=status;
	seq_sendMidi(msg);

}

/* Send a note on message. This will filter out these messages if appropriate
 */
void seq_sendMidiNoteOn(const uint8_t channel, const uint8_t note, const uint8_t veloc)
{
	static MidiMsg msg = { .bits = { .length = 2 } };  // status + 2 data bytes
	// FILT filter out note msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_NOTES<<4))==0)
		return;

	msg.status=NOTE_ON | channel;
	msg.data1=note;
	msg.data2=veloc;
	seq_sendMidi(msg);


	// keep track of which notes are on so we can turn them off later
	midi_chan_notes[channel]=note;
	midi_notes_on |= (1 << channel);

}


uint8_t seq_getStepIndex(uint8_t trackNr)
{
	return seq_stepIndex[trackNr];
}

/* This will send a prog change on the global channel and will filter
 * out the message if appropriate
 */
static void seq_sendProgChg(const uint8_t ptn)
{
	static MidiMsg msg = { .bits = { .length = 1 } };  // status + 1 data byte

	// FILT filter out PC msgs if appropriate
	if((midiParser_txRxFilter & (MIDI_FILTER_PROG_C<<4))==0)
		return;

	msg.status = PROG_CHANGE | midi_MidiChannels[7];
	msg.data1=ptn;
	msg.bits.length=1;
	seq_sendMidi(msg);
}

/* **PATROT set the step starting index to the position where the pattern rotation would have it start.
 *  A pattern rotation of 0 means start at the beginning of the pattern. max value is 15.
 *  Each value represents a main step interval (which contains 8 substeps)
 *
 *  This is called when the sequencer starts/stops running, also when a pattern change takes place
 */

static void seq_setStepIndexToStart()
{
	/*
	uint8_t len, rot, i;
	for(i=0;i<NUM_TRACKS;i++) {
		// adjust rot in case the pattern length is less than the rotated amount
		// len is 0-15 where a value of 0 means 16
		rot=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].rotate;
		len=seq_patternSet.seq_patternLengthRotate[seq_activePattern][i].length;
		if(len && (rot > len))
			rot = rot % len;

		// this is for external clock sync via trigger expansion kit (the ext tick will adjust this -1)
		seq_lastMasterStep[i] = (8 * rot);

		// -1 here because we increment it first thing when we start
		seq_stepIndex[i] = ( 8 * rot) - 1;

	}
	*/
	for(uint8_t i=0;i<NUM_TRACKS;i++) {
		// this is for external clock sync via trigger expansion kit (the ext tick will adjust this -1)
		seq_lastMasterStep[i] = 0;

		// -1 here because we increment it first thing when we start
		seq_stepIndex[i] =  - 1;

	}

}

