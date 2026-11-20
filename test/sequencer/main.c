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
//weak: the harness also runs on the stock sequencer (the reference timelines in golden-stock/)
extern void seq_setClockSwing(uint8_t on) __attribute__((weak));
extern uint16_t seq_queueDropped __attribute__((weak));

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
static int rollOn[2] = { 2, 10 }, rollOff[2] = { 21, 10 };
static void eventRoll(int step, int tick)
{
	if(step == rollOn[0] && tick == rollOn[1]) { seq_setRollRate(rollRate); seq_setRoll(1, 1); }
	if(step == rollOff[0] && tick == rollOff[1]) seq_setRoll(1, 0);
}

//live recording on track 2, played like a person. Hits arrive like MIDI notes, after seq_tick()
//in the block that reaches them; each is reported with the step it is meant for (HIT).
//  _early/_late:  steps 2-15 at their (swung) position -3 ms / +3 ms
//  _midlo/_midhi: 2 ms before / after the middle between steps n and n+1 (n = 2-15): meant
//                 for n / n+1 (n+1 = 16 wraps to step 1 of the next loop)
//  _start:        one hit 1 ms after start: meant for the first step
static void eventRecord(int step, int tick)
{
	if(step == 0 && tick == 0) seq_setRecordingMode(1);
	(void)tick;
}
#define MAX_HITS 20
static double hitTime[MAX_HITS];
static int hitStep[MAX_HITS], numHits;
static void planHits(const char* name, const double step_s, const uint8_t swing)
{
	const double late = swing / 127.0 * 16.0 * step_s / 24.0;
	#define POS(n) ((n) * step_s + (((n) & 1) ? late : 0.0))
	numHits = 0;
	if(strstr(name, "_start")) { hitTime[0] = 0.001; hitStep[0] = 0; numHits = 1; return; }
	for(int n = 2; n < 16; n++)
	{
		const double mid = (POS(n) + POS(n + 1)) / 2.0;
		if(strstr(name, "_early"))      { hitTime[numHits] = POS(n) - 0.003; hitStep[numHits] = n; }
		else if(strstr(name, "_late"))  { hitTime[numHits] = POS(n) + 0.003; hitStep[numHits] = n; }
		else if(strstr(name, "_midlo")) { hitTime[numHits] = mid - 0.002;    hitStep[numHits] = n; }
		else                            { hitTime[numHits] = mid + 0.002;    hitStep[numHits] = (n + 1) % 16; }
		numHits++;
	}
	#undef POS
}

//stop in the middle of a bar (step 9, tick 7) and start again (step 13 in the old count)
static void eventStopStart(int step, int tick)
{
	if(step == 9 && tick == 7) { seq_setRunning(0); seq_resetPosition(); timeline_event("STOP", -1, -1, -1); }
	if(step == 13 && tick == 0) { seq_setRunning(1); timeline_event("START", -1, -1, -1); }
}

//external clock pulse times: steady, jittered (+-1 ms, deterministic) or a tempo ramp (to +25 %)
#define MAX_PULSES 1200
static double pulseTime[MAX_PULSES];
static void planPulses(const char* name, const int ppStep, const int steps, const uint16_t bpm)
{
	const int n = ppStep * (steps + 3) < MAX_PULSES ? ppStep * (steps + 3) : MAX_PULSES;
	double t = 0;
	for(int k = 0; k < n; k++)
	{
		double b = bpm;
		if(strstr(name, "_ramp")) b = bpm * (1.0 + 0.25 * k / (double)(ppStep * steps));
		const double period = 60.0 / b / 4.0 / ppStep;
		double jitter = 0;
		if(strstr(name, "_jitter") && k > 0) jitter = ((k * 7919) % 11 - 5) * 0.0002;
		pulseTime[k] = t + jitter;
		t += period;
	}
	for(int k = n; k < MAX_PULSES; k++) pulseTime[k] = 1e9;
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
	//rolls: roll<menu index>_<speed> (One, 1/1, 1/2, 1/4, 1/8, 8T, 16, 16T, 32, 32T, 64)
	S("roll00_one",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll01_1-1",            120,   0, CLOCK_INTERNAL, 48, patternBasic,     eventRoll),
	S("roll02_1-2",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll03_1-4",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll04_1-8",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll05_8T",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll06_16",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll07_16T",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll08_32",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll09_32T",            120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll10_64",             120,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll10_64_150",         150,   0, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll04_1-8_swing064",   120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll05_8T_swing064",    120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll06_16_swing064",    120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll08_32_swing064",    120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll10_64_swing064",    120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll03_1-4_oddlength",  120,   0, CLOCK_INTERNAL, 48, patternOddLength, eventRoll),
	S("roll08_32_midiclock_swing064", 120, 64, CLOCK_MIDI, 32, patternBasic,   eventRoll),
	S("midiclock_swing000",    120,   0, CLOCK_MIDI,     32, patternTiming,    NULL),
	S("midiclock_swing064",    120,  64, CLOCK_MIDI,     32, patternTiming,    NULL),
	S("analogclock_swing000",  120,   0, CLOCK_ANALOG,   32, patternTiming,    NULL),
	S("analogclock_swing064",  120,  64, CLOCK_ANALOG,   32, patternTiming,    NULL),
	S("record_swing000_early", 120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing000_late",  120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_early", 120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_late",  120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing096_early", 120,  96, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing096_late",  120,  96, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_midiclock_swing064_early", 120, 64, CLOCK_MIDI, 20, NULL,       eventRecord),
	S("record_midiclock_swing064_late",  120, 64, CLOCK_MIDI, 20, NULL,       eventRecord),
	S("record_swing000_midlo", 120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing000_midhi", 120,   0, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_midlo", 120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_midhi", 120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing096_midhi", 120,  96, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing064_start", 120,  64, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	//maximum swing: a swung 16th sounds 2/3 of a step late, just before the next straight one
	S("record_swing127_early", 120, 127, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing127_late",  120, 127, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing127_midlo", 120, 127, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_swing127_midhi", 120, 127, CLOCK_INTERNAL, 20, NULL,             eventRecord),
	S("record_bpm060_swing127_early", 60, 127, CLOCK_INTERNAL, 20, NULL,       eventRecord),
	S("record_bpm060_swing127_late",  60, 127, CLOCK_INTERNAL, 20, NULL,       eventRecord),
	S("record_bpm060_swing127_midlo", 60, 127, CLOCK_INTERNAL, 20, NULL,       eventRecord),
	S("record_bpm060_swing127_midhi", 60, 127, CLOCK_INTERNAL, 20, NULL,       eventRecord),
	S("midiclock_jitter_swing064", 120, 64, CLOCK_MIDI,  32, patternTiming,    NULL),
	S("midiclock_ramp_swing064",   120, 64, CLOCK_MIDI,  32, patternTiming,    NULL),
	S("analogclock_jitter_swing064", 120, 64, CLOCK_ANALOG, 32, patternTiming, NULL),
	S("stopstart_swing064",    120,  64, CLOCK_INTERNAL, 32, patternTiming,    eventStopStart),
	//swing mode Clk (name ending in _clk): the clock outputs swing along, the notes are the same
	S("swing064_150_clk",      150,  64, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("swing127_150_clk",      150, 127, CLOCK_INTERNAL, 32, patternTiming,    NULL),
	S("roll06_16_swing064_clk",120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll06_16_swing127_tap",120, 127, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("roll06_16_swing127_tap_060", 60, 127, CLOCK_INTERNAL, 32, patternBasic, eventRoll),
	S("roll06_16_swing064_tap",120,  64, CLOCK_INTERNAL, 32, patternBasic,     eventRoll),
	S("midiclock_swing064_clk",120,  64, CLOCK_MIDI,     32, patternTiming,    NULL),
	S("analogclock_swing064_clk",120,64, CLOCK_ANALOG,   32, patternTiming,    NULL),
};
#define NUM_SCENARIOS (int)(sizeof(scenarios) / sizeof(scenarios[0]))

//------------------------------------------------------------------ run
//the scenario at the top of its timeline, for test/sequencer/check.py: settings, the active
//steps with their flam and shift, track lengths, events
static void writeHeader(const Scenario* sc)
{
	static const char* const clocks[] = { "internal", "midi", "analog" };
	fprintf(out, "# scenario %s bpm %u swing %u clock %s clockswing %d steps %d block %.12f systick %.12f\n",
			sc->name, sc->bpm, sc->swing, clocks[sc->clock], strstr(sc->name, "_clk") != NULL, sc->steps,
			BLOCK_S, SYSTICK_S);
	for(int p = 0; p < 2; p++)
		for(int t = 0; t < NUM_TRACKS; t++)
		{
			const uint8_t len = seq_patternSet.length[p][t];
			fprintf(out, "# length %d %d %d\n", p, t, len ? len : 16);
			for(int st = 0; st < NUM_STEPS; st++)
				if(seq_isMainStepActive(t, st, p))
					fprintf(out, "# step %d %d %d %d %d\n", p, t, st, seq_patternSet.seq_stepTiming[p][t][st].flam,
							seq_patternSet.seq_stepTiming[p][t][st].shift - 7);
		}
	if(sc->event == eventPatternChange) fprintf(out, "# patternchange queued 6 pattern 1 shuffle %u\n", seq_patternSet.shuffle[1]);
	if(sc->event == eventRoll) fprintf(out, "# roll track 1 rate %d on %d %d off %d %d\n", rollRate, rollOn[0], rollOn[1], rollOff[0], rollOff[1]);
	if(sc->event == eventRecord) fprintf(out, "# record\n");
	if(sc->event == eventStopStart) fprintf(out, "# stopstart\n");
}

static void run(const Scenario* sc)
{
	const double step_s = 60.0 / sc->bpm / 4.0;
	const double tick_s = step_s / 24.0;
	const long blocks = (long)((sc->steps + 2) * step_s / BLOCK_S);

	seq_init();
	seq_setBpm(sc->bpm);
	seq_setShuffle(sc->swing / 127.f);
	if(sc->pattern) sc->pattern();
	if(strstr(sc->name, "_clk") && seq_setClockSwing) seq_setClockSwing(1);
	if(sc->event == eventRoll) rollRate = atoi(sc->name + 4);	//roll<menu index>_...
	//_tap: a short press during a swung step (the 2nd 16th: step 3), after its straight start and
	//before its swung hit, released after it: like playing that 16th live (it used to be lost)
	if(sc->event == eventRoll && strstr(sc->name, "_tap")) { rollOn[0] = 3; rollOn[1] = 4; rollOff[0] = 3; rollOff[1] = 23; }
	writeHeader(sc);
	if(sc->event == eventRecord) planHits(sc->name, step_s, sc->swing);
	int nextHit = 0;

	//external clock: MIDI 24 ppq, analog one pulse per 16th; both start at time 0
	planPulses(sc->name, sc->clock == CLOCK_MIDI ? 6 : 1, sc->steps, sc->bpm);
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
		while(sc->event == eventRecord && nextHit < numHits && hitTime[nextHit] <= t)
		{
			timeline_event("HIT", hitStep[nextHit], -1, -1);
			seq_addNote(2, 100, 60);
			nextHit++;
		}

		//received in the same interrupt, after seq_tick()
		while(sc->clock != CLOCK_INTERNAL && ext_count < MAX_PULSES && pulseTime[ext_count] <= t)
		{
			timeline_event("EXT", (int)ext_count, -1, -1);
			if(sc->clock == CLOCK_MIDI) seq_sync();
			else trigger_clockReceived++;
			ext_count++;
		}
	}
	if(sc->event == eventRecord) dumpRecorded();
	if(&seq_queueDropped) timeline_event("DROPPED", seq_queueDropped, -1, -1);
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
