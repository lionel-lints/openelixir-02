/*
 * Sequencer timing harness: runs the real sequencer.c and clockSync.c on the host and
 * writes a timeline of every voice trigger, MIDI message out and clock output tick, with
 * the audio block and system tick it happened in.
 *
 *   seqtime --list               names of all scenarios
 *   seqtime <scenario> <file>    run one scenario, write its timeline
 *
 * Time model (from the firmware): the system tick (TIM14) runs at 108 MHz / 13501 / 2,
 * the audio engine renders 64-sample blocks at 44140 Hz and calls seq_tick() before each
 * block (TIM2 interrupt); MIDI received in that interrupt is handled after seq_tick().
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f7xx_hal.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/Sequencer/clockSync.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "timeline.h"

#define SYSTICK_S	(2.0 * 13501.0 / 108e6)	//0.25 ms
#define BLOCK_S		(64.0 / 44140.0)		//1.45 ms

extern volatile uint32_t systick_ticks;
extern uint8_t trigger_clockReceived;
void seq_tick(void);
void seq_sync(void);

//------------------------------------------------------------------ timeline
static FILE* out;
static long block;
void timeline_event(const char* name, int a, int b, int c)
{
	fprintf(out, "%ld %u %s", block, (unsigned)systick_ticks, name);
	if(a >= 0) fprintf(out, " %d", a);
	if(b >= 0) fprintf(out, " %d", b);
	if(c >= 0) fprintf(out, " %d", c);
	fputc('\n', out);
}

//------------------------------------------------------------------ scenarios
enum Clock { CLOCK_INTERNAL, CLOCK_MIDI, CLOCK_ANALOG };
typedef struct
{
	const char* name;
	uint16_t bpm;
	uint8_t swing;			//0-127, like the shuffle knob
	enum Clock clock;
	int steps;				//16ths to run
	void (*pattern)(void);
	void (*event)(int step, int tick);	//called on every tick of the run (step = 16th, tick 0-23)
} Scenario;

static void steps(uint8_t track, const int* list, int n)
{
	for(int i = 0; i < n; i++)
		seq_setMainStep(0, track, list[i], 1);
}
static void timing(uint8_t track, int step, uint8_t flam, int shift)
{
	seq_patternSet.seq_stepTiming[0][track][step].flam = flam;
	seq_patternSet.seq_stepTiming[0][track][step].shift = 7 + shift;	//7 = no shift
}

//kick on the quarters, snare on 2 and 4, closed hat on every 16th
static void patternBasic(void)
{
	const int kick[] = { 0, 4, 8, 12 }, snare[] = { 4, 12 };
	steps(0, kick, 4);
	steps(3, snare, 2);
	for(int s = 0; s < 16; s++) seq_setMainStep(0, 5, s, 1);
}
//the basic pattern plus flams and micro-shifts, also on swung steps (odd 16ths)
static void patternTiming(void)
{
	patternBasic();
	timing(5, 1, 3, 0);		//x4 on a swung step
	timing(5, 5, 5, 0);		//x8 on a swung step
	timing(5, 2, 2, 0);		//x3 on a straight step
	timing(5, 3, 0, 3);		//+9 ticks, swung step
	timing(5, 9, 0, -2);	//-6 ticks, swung step
	timing(5, 10, 0, 5);	//+15 ticks into the swung step 11
	timing(3, 12, 1, -7);	//x2, -21 ticks
}
//a 15-step track against 16 steps
static void patternOddLength(void)
{
	patternBasic();
	for(int s = 0; s < 15; s++) seq_setMainStep(0, 1, s, 1);
	seq_patternSet.length[0][1] = 15;
	seq_patternSet.length[0][5] = 15;
}
//pattern 1 is different; the change to it is queued during the first bar
static void patternTwo(void)
{
	patternBasic();
	const int kick[] = { 0, 3, 6, 10 };
	for(int i = 0; i < 4; i++) seq_setMainStep(1, 0, kick[i], 1);
}
static void eventPatternChange(int step, int tick)
{
	if(step == 6 && tick == 5) seq_setNextPattern(1);
}

//rolls on track 1 from step 2 to step 21
static int rollRate;
static void eventRoll(int step, int tick)
{
	if(step == 2 && tick == 10) { seq_setRollRate(rollRate); seq_setRoll(1, 1); }
	if(step == 21 && tick == 10) seq_setRoll(1, 0);
}

//live recording on track 2, played like a person: steps 2-15, each hit at the step's
//(swung) position plus recordOffsetMs (early, on time, late). Hits arrive like MIDI notes,
//after seq_tick() in the block that reaches them; each is reported with its intended step.
static int recordOffsetMs;
static void eventRecord(int step, int tick)
{
	if(step == 0 && tick == 0) seq_setRecordingMode(1);
	(void)tick;
}
static double recordHitTime(const double step_s, const uint8_t swing, const int step)
{
	const double late = (step & 1) ? swing / 127.0 * 16.0 * step_s / 24.0 : 0.0;
	return step * step_s + late + recordOffsetMs / 1000.0;
}
static void dumpRecorded(void)
{
	for(int s = 0; s < 16; s++)
		if(seq_patternSet.seq_mainSteps[0][0][2] & (1 << s))
			timeline_event("REC", s, -1, -1);
}

#define S(n, b, sw, clk, st, pat, ev) { n, b, sw, clk, st, pat, ev }
static Scenario scenarios[] =
{
	S("straight_120",          120,   0, CLOCK_INTERNAL, 32, patternBasic,     NULL),
	S("swing000_150",          150,   0, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing020_150",          150,  20, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing040_150",          150,  40, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing064_150",          150,  64, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing096_150",          150,  96, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing127_150",          150, 127, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing064_093",           93,  64, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("oddlength_swing064",    120,  64, CLOCK_INTERNAL, 48, patternOddLength, NULL),
	S("patternchange_swing040",120,  40, CLOCK_INTERNAL, 48, patternTwo,       eventPatternChange),
	S("roll0_one",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll1_1-1",             120,   0, CLOCK_INTERNAL, 48, patternBasic,     eventRoll),
	S("roll2_1-2",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll3_1-4",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll4_1-8",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll5_16",              120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll4_1-8_swing064",    120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll5_16_swing064",     120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("midiclock_swing000",    120,   0, CLOCK_MIDI,     32, patternTiming,    NULL),
	S("midiclock_swing064",    120,  64, CLOCK_MIDI,     32, patternTiming,    NULL),
	S("analogclock_swing064",  120,  64, CLOCK_ANALOG,   32, patternTiming,    NULL),
	S("record_swing000_early", 120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing000_late",  120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_early", 120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_late",  120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing096_early", 120,  96, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing096_late",  120,  96, CLOCK_INTERNAL, 20, NULL,             eventRecord),
};
#define NUM_SCENARIOS (int)(sizeof(scenarios) / sizeof(scenarios[0]))

//------------------------------------------------------------------ run
static void run(const Scenario* sc)
{
	const double step_s = 60.0 / sc->bpm / 4.0;
	const double tick_s = step_s / 24.0;
	const long blocks = (long)((sc->steps + 2) * step_s / BLOCK_S);

	seq_init();
	seq_setBpm(sc->bpm);
	seq_setShuffle(sc->swing / 127.f);
	if(sc->pattern) sc->pattern();
	//recording: 3 ms early or 3 ms late
	if(sc->event == eventRecord) recordOffsetMs = strstr(sc->name, "_early") ? -3 : 3;
	int nextHitStep = 2;

	//external clock: MIDI 24 ppq, analog one pulse per 16th; both start at time 0
	const double ext_period = sc->clock == CLOCK_MIDI ? step_s / 6.0 : step_s;
	long ext_count = 0;
	if(sc->clock != CLOCK_INTERNAL)
	{
		seq_setExtSync(sc->clock == CLOCK_MIDI ? 2 : 1);
		if(sc->clock == CLOCK_MIDI) { sync_midiResetPosition(); sync_midiStartStop(1); }
		else seq_setRunning(1);
	}
	else
	{
		seq_setRunning(1);
	}

	long last_tick = -1;
	for(block = 0; block < blocks; block++)
	{
		const double t = block * BLOCK_S;
		systick_ticks = (uint32_t)(t / SYSTICK_S);

		//scenario events at their nominal (unswung) time, before the block that reaches it
		if(sc->event)
		{
			const long tick = (long)(t / tick_s);
			for(long k = last_tick + 1; k <= tick; k++)
				sc->event((int)(k / 24), (int)(k % 24));
			last_tick = tick;
		}

		seq_tick();

		//recorded hits arrive like MIDI notes: after seq_tick()
		while(sc->event == eventRecord && nextHitStep < 16 && recordHitTime(step_s, sc->swing, nextHitStep) <= t)
		{
			timeline_event("HIT", nextHitStep, -1, -1);
			seq_addNote(2, 100, 60);
			nextHitStep++;
		}

		//received in the same interrupt, after seq_tick()
		while(sc->clock != CLOCK_INTERNAL && ext_count * ext_period <= t)
		{
			if(sc->clock == CLOCK_MIDI) seq_sync();
			else trigger_clockReceived++;
			ext_count++;
		}
	}
	if(sc->event == eventRecord) dumpRecorded();
}

int main(int argc, char** argv)
{
	if(argc == 2 && strcmp(argv[1], "--list") == 0)
	{
		for(int i = 0; i < NUM_SCENARIOS; i++) puts(scenarios[i].name);
		return 0;
	}
	if(argc != 3)
	{
		fprintf(stderr, "usage: %s --list | <scenario> <output file>\n", argv[0]);
		return 2;
	}
	for(int i = 0; i < NUM_SCENARIOS; i++)
	{
		if(strcmp(argv[1], scenarios[i].name) != 0) continue;
		out = fopen(argv[2], "w");
		if(!out) { perror(argv[2]); return 1; }
		run(&scenarios[i]);
		fclose(out);
		return 0;
	}
	fprintf(stderr, "unknown scenario %s\n", argv[1]);
	return 2;
}
