/*
 * Host render harness for the OpenElixir-02 audio engine.
 *
 * Drives the engine only through entry points that stay stable while the
 * engine is refactored:
 *   soundengine_init()            engine start-up
 *   frontParser_parseUartData()   UI -> engine bytes: parameters, LFO and velocity targets
 *   voiceControl_noteOn()         triggers (tracks 1-7)
 *   mixer_calcNextSampleBlock()   rendering
 * and writes every rendered sample of both DACs to a file, so the output of two
 * source trees can be compared with cmp. Usage: render <output file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
#include "stm32f7xx_hal.h"
#include "main.h"
#include "Soundengine/Soundengine.h"
#include "Soundengine/DSPAudio/mixer.h"
#include "Soundengine/MIDI/MidiVoiceControl.h"
#include "Soundengine/MIDI/CortexParser.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Parameters.h"
#include "menu/menu.h"
#include "Preset/InitKit.h"
#include "Soundengine/MIDI/ParameterArray.h"
#include "Soundengine/DSPAudio/wavetable.h"
#include "Soundengine/DSPAudio/transientTables.h"

#define NUM_KITS		32
#define BLOCKS_PER_KIT	400

//---------------------------------------------------------------- environment
static uint32_t rngState = 1;
static uint32_t rnd(uint32_t n)	//deterministic, independent of the engine's RNG
{
	rngState = rngState * 1103515245u + 12345u;
	return (rngState >> 8) % n;
}

static uint32_t hwRngState = 0x12345678;
uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *h)
{
	(void)h;
	hwRngState ^= hwRngState << 13;
	hwRngState ^= hwRngState >> 17;
	hwRngState ^= hwRngState << 5;
	return hwRngState;
}

static float faders[6];
float adc_getFaderVolume(uint8_t idx)
{
	return faders[idx];
}

//map the peripheral and core register ranges as plain memory (GPIO jack
//switches, DWT, FPU, timers are read/written by the engine)
static void mapPeripherals(void)
{
	const struct { uintptr_t base; size_t size; } regions[] = {
		{ 0x40000000u, 0x20000000u },
		{ 0xE0000000u, 0x00100000u },
	};
	for(unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
	{
		void* p = mmap((void*)regions[i].base, regions[i].size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if(p != (void*)regions[i].base)
		{
			perror("mmap peripheral region");
			exit(2);
		}
	}
}

static void setJackSwitches(uint32_t mask)
{
	*(volatile uint32_t*)&switch_l1_GPIO_Port->IDR = ((mask & 1) ? switch_l1_Pin : 0) | ((mask & 2) ? switch_r1_Pin : 0);
	*(volatile uint32_t*)&switch_l2_GPIO_Port->IDR = ((mask & 4) ? switch_l2_Pin : 0) | ((mask & 8) ? switch_r2_Pin : 0);
}

//---------------------------------------------------------------- UI messages
static void sendFront(uint8_t status, uint8_t data1, uint8_t data2)
{
	frontParser_parseUartData(status);
	frontParser_parseUartData(data1);
	frontParser_parseUartData(data2);
}

static void sendParam(uint16_t par, uint8_t value)
{
	//the UI keeps the kit in activeParameters; the engine reads it back when it
	//resets modulation targets to the unmodulated values
	activeParameters[par] = value;
	if(par < 128)
	{
		sendFront(MIDI_CC, (uint8_t)par, value);
	}
	else
	{
		sendFront(FRONT_CC_2, (uint8_t)(par - 128), value);
	}
}

static void sendTarget(uint8_t status, uint8_t voice, uint8_t param)
{
	sendFront(status, (uint8_t)(((param & 0x80) >> 7) | (voice << 1)), param & 0x7f);
}

//parameters that are not plain sound parameters on the engine side
static int skipParam(uint16_t par)
{
	return par == PAR_NONE || par == PAR_NRPN_DATA_ENTRY_COARSE || par == PAR_NRPN_FINE
		|| par == PAR_NRPN_COARSE || par == PAR_BANK_CHANGE
		|| (par >= PAR_VEL_DEST_1 && par <= PAR_VEL_DEST_6)
		|| (par >= PAR_TARGET_LFO1 && par <= PAR_TARGET_LFO6)
		|| (par >= PAR_VOICE_LFO1 && par <= PAR_VOICE_LFO6);
}

//a random value in the range the UI allows for this parameter (ranges_gen.c)
void harness_valueRange(uint16_t par, uint8_t* min, uint8_t* max);
static uint8_t randomValue(uint16_t par)
{
	uint8_t min, max;
	harness_valueRange(par, &min, &max);
	return (uint8_t)(min + rnd(max - min + 1u));
}

//a random modulation target the UI could send (it only offers parameters the
//engine can modulate, i.e. with a parameterArray pointer)
static uint8_t randomTarget(void)
{
	uint16_t par;
	do { par = (uint16_t)(1 + rnd(END_OF_SOUND_PARAMETERS - 1)); } while(parameterArray[par].ptr == NULL);
	return (uint8_t)par;
}

static void loadKit(int kit)
{
	for(uint16_t par = 0; par < END_OF_SOUND_PARAMETERS; par++)
	{
		if(skipParam(par)) continue;
		const uint8_t init = par < INIT_KIT_SIZE - 8 ? initKit[8 + par] : 0;	//the init kit lacks the last parameter
		sendParam(par, kit == 0 ? init : randomValue(par));
	}
	for(uint8_t v = 0; v < 6; v++)
	{
		sendTarget(FRONT_CC_LFO_TARGET, v, randomTarget());
		sendTarget(FRONT_CC_VELO_TARGET, v, randomTarget());
	}
	for(uint8_t i = 0; i < 6; i++)
	{
		const float f = (float)rnd(128) / 127.f;
		faders[i] = kit == 0 ? 1.f : f * f;
	}
	setJackSwitches(kit == 0 ? 0xf : rnd(16));
}

//---------------------------------------------------------------- integrity
//the constant tables in DTCM are writable memory on the target: an out-of-bounds
//write from a neighbouring buffer would silently change them (and the sound)
static uint32_t fnv(const void* p, size_t n)
{
	const uint8_t* b = p;
	uint32_t h = 2166136261u;
	while(n--) h = (h ^ *b++) * 16777619u;
	return h;
}
static uint32_t tablesHash(void)
{
	return fnv(transientData, sizeof(transientData)) ^ fnv(transientVolumeTable, sizeof(transientVolumeTable)) * 3u
		^ fnv(sine_table, sizeof(sine_table)) * 7u;
}

//---------------------------------------------------------------- engine switching
#if __has_include("Soundengine/DSPAudio/VoiceSlot.h")
#include "Soundengine/DSPAudio/VoiceSlot.h"
#define HAVE_VOICE_SLOTS 1
#if __has_include("Preset/KitEngines.h")
#include "Preset/KitEngines.h"
#include "Soundengine/MIDI/MidiParser.h"
#ifdef KIT_ENGINES_BLOCK_SIZE	//the engine checks need the current KitEngines API (older trees only render)
#define HAVE_KIT_ENGINES 1
#endif

//the UI sends to the sound engine like this in the firmware too
void frontPanel_sendData(uint8_t status, uint8_t data1, uint8_t data2)
{
	sendFront(status, data1, data2);
}
#endif

static int16_t engOut[OUTPUT_DMA_SIZE * 2], engOut2[OUTPUT_DMA_SIZE * 2];

static int slotHasNan(void)
{
	for(int s = 0; s < NUM_VOICE_SLOTS; s++)
	{
		const VoiceCommon* c = &voiceSlots[s].u.c;
		if(isnan(c->egValueOscVol) || isnan(c->filter.s1) || isnan(c->filter.s2) || isnan(c->combinedVolume)) return 1;
	}
	return 0;
}

//render until silent; returns the number of blocks that had sound, peak in *peak
static int renderSound(int maxBlocks, int* peak, int* nan)
{
	int last = -1;
	*peak = 0;
	for(int b = 0; b < maxBlocks; b++)
	{
		mixer_calcNextSampleBlock(engOut, engOut2);
		for(int i = 0; i < OUTPUT_DMA_SIZE * 2; i++)
		{
			const int a = abs(engOut[i]) > abs(engOut2[i]) ? abs(engOut[i]) : abs(engOut2[i]);
			if(a > *peak) *peak = a;
			if(a > 32) last = b;
		}
		*nan |= slotHasNan();
	}
	return last + 1;
}

static int nrpnFail;
static void switchEngine(uint8_t slot, uint8_t engine)
{
#ifdef HAVE_KIT_ENGINES
	//like a received NRPN 98: the sound engine switches, then the UI applies the switch rules
	MidiMsg m = { 0 };
	m.status = MIDI_CC2;
	m.data1 = PAR_SLOT_ENGINE - 128;
	m.data2 = (uint8_t)(slot * 4 + engine);
#ifdef SLOT_ENGINE_EXT_BASE
	if(engine >= 4)	//engines added later: 24 + slot + 6*(engine-4)
		m.data2 = (uint8_t)(SLOT_ENGINE_EXT_BASE + slot + NUM_VOICE_SLOTS * (engine - 4));
#endif
	midiParser_ccHandler(m, 1, 0, 0.f);
	if(voiceSlots[slot].engine != engine)
	{
		printf("NRPN 98: slot %u didn't get engine %u\n", slot + 1, engine);
		nrpnFail = 1;
	}
	kitEngines_assign(slot, engine);
#else
	slot_setEngine(slot, engine);
#endif
}

#ifdef HAVE_KIT_ENGINES
//Voice level checks render a copy of a slot's voice directly (no mixer, FX or
//other voices), so every run starts from the same state (oscillator phases,
//noise sequence) and only the voice itself is measured.
static void voiceTrigger(VoiceSlot* c, uint8_t slot, uint8_t second)
{
	switch(c->engine)
	{
	case ENGINE_SNARE:	Snare_trigger(&c->u.snare, 127, 60);			break;
	case ENGINE_CYMBAL:	Cymbal_trigger(&c->u.cymbal, 127, 60);			break;
	case ENGINE_HIHAT:	HiHat_trigger(&c->u.hat, 127, second, 60);		break;
#ifdef HAVE_ENGINE_MODAL
	case ENGINE_MODAL:	Modal_trigger(&c->u.modal, 127, 60);			break;
#endif
#ifdef HAVE_ENGINE_ANALOG
	case ENGINE_ANALOG:	Analog_trigger(&c->u.analog, 127, 60);			break;
#endif
	default:			Drum_trigger(&c->u.drum, 127, 60);				break;
	}
	if(slot == NUM_VOICE_SLOTS - 1) c->u.c.oscVolEg.decay = c->decay[second];	//as slot_trigger()
}

//render blocks; returns a hash of the output, *len = number of blocks with sound
static uint32_t voiceRender(VoiceSlot* c, int blocks, int* len)
{
	int16_t buf[OUTPUT_DMA_SIZE];
	uint32_t h = 2166136261u;
	int last = -1;
	for(int b = 0; b < blocks; b++)
	{
		SVF_recalcFreq(&c->u.c.filter);
		switch(c->engine)
		{
		case ENGINE_SNARE:	Snare_calcAsync(&c->u.snare);	Snare_calcSyncBlock(&c->u.snare, buf, OUTPUT_DMA_SIZE);		break;
		case ENGINE_CYMBAL:	Cymbal_calcAsync(&c->u.cymbal);	Cymbal_calcSyncBlock(&c->u.cymbal, buf, OUTPUT_DMA_SIZE);	break;
		case ENGINE_HIHAT:	HiHat_calcAsync(&c->u.hat);		HiHat_calcSyncBlock(&c->u.hat, buf, OUTPUT_DMA_SIZE);		break;
#ifdef HAVE_ENGINE_MODAL
		case ENGINE_MODAL:	Modal_calcAsync(&c->u.modal);	Modal_calcSyncBlock(&c->u.modal, buf, OUTPUT_DMA_SIZE);	break;
#endif
#ifdef HAVE_ENGINE_ANALOG
		case ENGINE_ANALOG:	Analog_calcAsync(&c->u.analog);	Analog_calcSyncBlock(&c->u.analog, buf, OUTPUT_DMA_SIZE);	break;
#endif
		default:			calcDrumVoiceAsync(&c->u.drum);	calcDrumVoiceSyncBlock(&c->u.drum, buf, OUTPUT_DMA_SIZE);	break;
		}
		h = fnv(buf, sizeof(buf)) ^ h * 16777619u;
		for(int i = 0; i < OUTPUT_DMA_SIZE; i++) if(abs(buf[i]) > 32) last = b;
	}
	if(len) *len = last + 1;
	return h;
}

static uint32_t slotSoundHash(uint8_t slot)
{
	static VoiceSlot c;
	c = voiceSlots[slot];
	hwRngState = 0x12345678;
	voiceTrigger(&c, slot, 0);
	return voiceRender(&c, 200, NULL);
}

//slot 6: track 7 must use decay B (longer here) and cut a sounding track 7 the
//other way round: track 6 after track 7 ends no later than track 6 alone
static int decayTest(void)
{
	static VoiceSlot c;
	int lenA, lenB, lenCut;
	const uint8_t slot = NUM_VOICE_SLOTS - 1;
	c = voiceSlots[slot]; hwRngState = 0x12345678;
	voiceTrigger(&c, slot, 0); voiceRender(&c, 3000, &lenA);
	c = voiceSlots[slot]; hwRngState = 0x12345678;
	voiceTrigger(&c, slot, 1); voiceRender(&c, 3000, &lenB);
	c = voiceSlots[slot]; hwRngState = 0x12345678;
	voiceTrigger(&c, slot, 1); voiceRender(&c, 5, NULL);
	voiceTrigger(&c, slot, 0); voiceRender(&c, 3000, &lenCut);
	printf("  slot 6 decay A %d, B %d, B cut by A %d", lenA, lenB, lenCut);
	return !(lenB > lenA && lenCut <= lenA);
}

//every engine specific parameter of a slot must change the sound of the engine
//the slot runs, except the roles that engine doesn't have
static int roleTest(int round)
{
	int fail = 0;
	printf("  roles (x = changes the sound):");
	for(uint8_t s = 0; s < NUM_VOICE_SLOTS; s++)
	{
		const uint8_t engine = voiceSlots[s].engine;
		printf("  %d:", s + 1);
		//an active setting first, so that turning one role to 0 must change the
		//sound: roles 0-5 at 100 (FM amount > 0, waveform PWM, ...), role 6 at 1
		//(mix mode on, 1 repeat; many repeats can silence a short attack), and
		//the slot's PWM at 64 (PWM at pulse width 0 is silent) and its main
		//oscillator a sine (a noise carrier ignores the FM modulators)
		static const uint8_t oscWave[NUM_VOICE_SLOTS] = { PAR_OSC_WAVE_DRUM1, PAR_OSC_WAVE_DRUM2, PAR_OSC_WAVE_DRUM3, PAR_OSC_WAVE_SNARE, PAR_WAVE1_CYM, PAR_WAVE1_HH };
		uint8_t saved[NUM_SLOT_ROLES];
		const uint8_t savedPwm = activeParameters[PAR_PWM_1 + s];
		const uint8_t savedWave = activeParameters[oscWave[s]];
		sendParam(PAR_PWM_1 + s, 64);
		sendParam(oscWave[s], SINE);
		for(uint8_t role = 0; role < NUM_SLOT_ROLES; role++)
		{
			saved[role] = activeParameters[slot_rolePar[s][role]];
			sendParam(slot_rolePar[s][role], role == 6 ? 1 : 100);
		}
		for(uint8_t role = 0; role < NUM_SLOT_ROLES; role++)
		{
			const uint8_t par = slot_rolePar[s][role];
			const uint32_t a = slotSoundHash(s);
			sendParam(par, 0);
			const uint32_t b = slotSoundHash(s);
			sendParam(par, role == 6 ? 1 : 100);
			int unused = (engine == ENGINE_SNARE && role == 0) || (engine == ENGINE_HIHAT && role == 6);
#ifdef HAVE_ENGINE_ANALOG
			unused |= engine == ENGINE_ANALOG && role >= 2;	//instrument and saturator only
#endif
			printf("%c", a != b ? 'x' : '.');
			if((a != b) == unused)
			{
				fail = 1;
			}
		}
		for(uint8_t role = 0; role < NUM_SLOT_ROLES; role++)
		{
			sendParam(slot_rolePar[s][role], saved[role]);
		}
		sendParam(PAR_PWM_1 + s, savedPwm);
		sendParam(oscWave[s], savedWave);
	}
	printf("\n");
	(void)round;
	return fail;
}
#endif

#ifdef HAVE_KIT_ENGINES
//switching an engine clears step locks of the slot's engine specific parameters (they would change
//another setting now) in every pattern, and only those; the scan of all steps renders the audio
//block that comes due after each pattern (it used to hold up the audio for about 8 ms)
extern unsigned stub_seqTicks;
static int stepLockTest(void)
{
	const uint8_t slot = 1;
	kitEngines_setStock(kitEngines);
	kitEngines_apply();
	memset(&seq_patternSet.seq_subStepPattern, 0, sizeof(seq_patternSet.seq_subStepPattern));
	#define STORED(par) ((uint8_t)((par) < 128 ? (par) + 1 : (par)))	//as seq_automationTarget() reads it
	Step* roleFirst = &seq_patternSet.seq_subStepPattern[0][0][0];
	Step* roleLast = &seq_patternSet.seq_subStepPattern[NUM_PATTERN - 1][NUM_TRACKS - 1][NUM_STEPS - 1];
	Step* common = &seq_patternSet.seq_subStepPattern[5][2][7];
	Step* otherSlot = &seq_patternSet.seq_subStepPattern[9][1][3];
	roleFirst->param1Nr = STORED(slot_rolePar[slot][0]); roleFirst->param1Val = 50;
	roleLast->param2Nr = STORED(slot_rolePar[slot][6]); roleLast->param2Val = 60;
	common->param1Nr = STORED(PAR_COARSE2); common->param1Val = 70;
	otherSlot->param2Nr = STORED(slot_rolePar[slot + 1][0]); otherSlot->param2Val = 80;
	#undef STORED
	const unsigned ticks = stub_seqTicks;
	kitEngines_assign(slot, ENGINE_MODAL);
	const unsigned yields = stub_seqTicks - ticks;
	const int bad = roleFirst->param1Nr != NO_AUTOMATION || roleFirst->param1Val != 0
			|| roleLast->param2Nr != NO_AUTOMATION || roleLast->param2Val != 0
			|| common->param1Val != 70 || otherSlot->param2Val != 80 || yields < NUM_PATTERN;
	printf("engine switch clears its slot's role step locks (%u render yields): %s\n", yields, bad ? "FAILED" : "ok");
	return bad;
}

//the engine block in kit files: round trip, and stock engines for anything else
static int blockTest(void)
{
	static const uint8_t engines[NUM_VOICE_SLOTS] = { ENGINE_HIHAT, ENGINE_SNARE, ENGINE_DRUM, ENGINE_CYMBAL, ENGINE_DRUM, ENGINE_SNARE };
	uint8_t stock[NUM_VOICE_SLOTS], out[NUM_VOICE_SLOTS], block[32];
	int fail = 0;
	kitEngines_setStock(stock);
	const uint8_t size = kitEngines_encode(engines, block);
	kitEngines_decode(block, size, out);
	fail |= memcmp(out, engines, sizeof(out)) != 0;
	kitEngines_decode(block, (uint16_t)(size + 10), out);		//more bytes after the block
	fail |= memcmp(out, engines, sizeof(out)) != 0;
	kitEngines_decode(block, (uint16_t)(size - 1), out);		//truncated
	fail |= memcmp(out, stock, sizeof(out)) != 0;
	kitEngines_decode(block, 0, out);							//no block (stock firmware kit)
	fail |= memcmp(out, stock, sizeof(out)) != 0;
	block[1] = 'X';												//not a block
	kitEngines_decode(block, size, out);
	fail |= memcmp(out, stock, sizeof(out)) != 0;
	block[1] = 'E'; block[5] = 9;								//invalid engine: that slot stock
	kitEngines_decode(block, size, out);
	fail |= out[0] != stock[0] || memcmp(&out[1], &engines[1], NUM_VOICE_SLOTS - 1) != 0;
	printf("engine block in kit files: %s\n", fail ? "FAILED" : "ok");
	return fail;
}

#if KIT_ENGINES_API >= 2
//parameter mapping between slots (menu pages and value types of switched
//slots): every parameter of a slot belongs to it and maps back and forth
static int mapTest(void)
{
	int fail = 0;
	for(uint16_t par = 1; par < END_OF_SOUND_PARAMETERS; par++)
	{
		const uint8_t slot = kitEngines_slotOfParam(par);
		if(slot >= NUM_VOICE_SLOTS)
			continue;
		for(uint8_t to = 0; to < NUM_VOICE_SLOTS; to++)
		{
			const uint16_t mapped = kitEngines_mapParam(par, slot, to);
			const uint16_t back = kitEngines_mapParam(mapped, to, slot);
			const uint16_t expect = (par == PAR_VELOD6_OPEN && to != slot) ? PAR_VELOD6_CLOSED : par;	//decay B -> decay
			if(kitEngines_slotOfParam(mapped) != to || back != expect)
			{
				printf("  map: par %u slot %u -> %u gives %u, back %u\n", par, slot + 1, to + 1, mapped, back);
				fail = 1;
			}
		}
	}
	printf("parameter mapping between slots: %s\n", fail ? "FAILED" : "ok");
	return fail;
}
#endif
#endif

#if KIT_ENGINES_API >= 3
//loading a voice from another kit (LOAD + voice): the slot gets the other kit's
//values and engine, the other voices stay as they are, the LFO target only
//comes along if that LFO modulated the voice itself
static int loadVoiceTest(void)
{
	static uint8_t kit[END_OF_SOUND_PARAMETERS], before[END_OF_SOUND_PARAMETERS];
	int fail = 0, peak, nan = 0;
	for(uint8_t slot = 0; slot < NUM_VOICE_SLOTS; slot++)
	{
		for(uint8_t engine = 0; engine < NUM_ENGINES; engine++)
		{
			loadKit(0);
			kitEngines_setStock(kitEngines);
			kitEngines_apply();
			for(uint16_t par = 1; par < END_OF_SOUND_PARAMETERS; par++)
				kit[par] = skipParam(par) ? 0 : randomValue(par);
			const uint8_t lfoOnItself = (slot + engine) % 2;
			kit[PAR_VOICE_LFO1 + slot] = lfoOnItself ? slot + 1 : (slot + 1) % NUM_VOICE_SLOTS + 1;
			kit[PAR_TARGET_LFO1 + slot] = pgm_read_byte(&modTargetVoiceOffsets[kit[PAR_VOICE_LFO1 + slot] - 1].start) + 2;
			kit[PAR_VEL_DEST_1 + slot] = pgm_read_byte(&modTargetVoiceOffsets[slot].start) + 1;
			memcpy(before, activeParameters, sizeof(before));

			kitEngines_loadVoice(slot, engine, kit);

			int bad = voiceSlots[slot].engine != engine || kitEngines[slot] != engine;
			for(uint16_t par = 1; par < END_OF_SOUND_PARAMETERS; par++)
			{
				const uint8_t owner = kitEngines_slotOfParam(par);
				if(par == PAR_TARGET_LFO1 + slot)
					bad |= activeParameters[par] != (lfoOnItself ? kit[par] : 0);
				else if(par == PAR_VOICE_LFO1 + slot)
					bad |= activeParameters[par] != slot + 1;
				else if(owner == slot)
					bad |= activeParameters[par] != kit[par];
				else if(!(par >= PAR_TARGET_LFO1 && par <= PAR_TARGET_LFO6) && !(par >= PAR_VEL_DEST_1 && par <= PAR_VEL_DEST_6))
					bad |= activeParameters[par] != before[par];	//other voices (their targets may be turned off)
			}
			voiceControl_noteOn(slot, 60, 127);
			bad |= renderSound(800, &peak, &nan) == 0 && kit[PAR_VOL1 + slot] > 10;
			if(bad)
			{
				printf("  load voice: slot %u engine %u FAILED\n", slot + 1, engine);
				fail = 1;
			}
		}
	}
	printf("load voice from another kit: %s\n", fail || nan ? "FAILED" : "ok");
	return fail || nan;
}
#endif

//every slot gets every engine: each track must sound, track 7 must use decay B,
//track 6 must cut track 7, and the engine specific parameters must reach the engine
#ifdef HAVE_ENGINE_ANALOG
static int pageTest(void);
#endif
static int engineTest(void)
{
	static const char* const names[NUM_ENGINES] = { "drum", "snare", "cymbal", "hihat",
#ifdef HAVE_ENGINE_MODAL
			"modal",
#endif
#ifdef HAVE_ENGINE_ANALOG
			"analog",
#endif
	};
	int fail = 0, peak, nan = 0;
#if KIT_ENGINES_API >= 3
	fail |= loadVoiceTest();
#endif
	//pitch EG slope 127 made the EG output NaN and stopped the oscillator
	{
		loadKit(0);
		sendParam(PAR_PITCH_SLOPE1, 127);
		sendParam(PAR_MODAMNT1, 100);
		renderSound(400, &peak, &nan);
		voiceControl_noteOn(0, 60, 127);
		const int len = renderSound(800, &peak, &nan);
		const int bad = len == 0 || isnan(voiceSlots[0].u.drum.oscPitchEg.value) || isnan(voiceSlots[0].u.drum.osc.pitchMod);
		printf("pitch EG slope 127: %s\n", bad ? "FAILED" : "ok");
		fail |= bad;
	}
#ifdef HAVE_KIT_ENGINES
	fail |= blockTest();
#if KIT_ENGINES_API >= 2
	fail |= mapTest();
#endif
#endif
	for(int round = 0; round < NUM_ENGINES; round++)
	{
#ifdef HAVE_KIT_ENGINES
		kitEngines_setStock(kitEngines);	//like loading a stock kit
		kitEngines_apply();
#endif
		loadKit(0);
		for(uint8_t v = 0; v < 6; v++)
		{
			sendTarget(FRONT_CC_LFO_TARGET, v, 0);
			sendTarget(FRONT_CC_VELO_TARGET, v, 0);
		}
		for(uint8_t s = 0; s < NUM_VOICE_SLOTS; s++)
		{
			switchEngine(s, (s + round) % NUM_ENGINES);
		}
		sendParam(PAR_VELOD6_CLOSED, 40);
		sendParam(PAR_VELOD6_OPEN, 90);
		renderSound(400, &peak, &nan);

		printf("round %d:", round);
		int len[7];
		for(uint8_t track = 0; track < 7; track++)
		{
			voiceControl_noteOn(track, 60, 127);
			len[track] = renderSound(1500, &peak, &nan);
			printf("  %d %s %d/%d", track + 1, names[voiceSlots[track < 6 ? track : 5].engine], len[track], peak);
			if(len[track] == 0) fail = 1;
		}
		printf("\n");
#ifdef HAVE_KIT_ENGINES
		fail |= decayTest();
		fail |= roleTest(round);
#endif
	}
	if(nan) printf("NaN in a voice\n");
	fail |= nrpnFail;
#ifdef HAVE_ENGINE_ANALOG
	fail |= pageTest();
#endif
#ifdef HAVE_KIT_ENGINES
	fail |= stepLockTest();
#endif
	printf(fail || nan ? "ENGINE TEST FAILED\n" : "engine test passed\n");
	return fail || nan;
}
#endif

#ifdef HAVE_ENGINE_ANALOG
#include "menu/analogSetup.h"
#include "Soundengine/DSPAudio/automationNode.h"
#include "Soundengine/DSPAudio/SlopeEg2.h"
//Analog engine (docs/ENGINES.md): the voice is checked against what its instruments are defined
//to do (AnalogModels.c), and the UI's setup (menu/analogSetup.c) against its rules

static const char* const anNames[NUM_ANALOG_MODELS] =
{
	"BD8", "BD9", "SD8", "SD9", "LT8", "MT8", "HT8", "LT9", "MT9", "HT9", "LC8", "MC8", "HC8",
	"RS8", "RS9", "CL8", "CP8", "CP9", "MA8", "CB8", "HH8", "HH9", "CY8", "CR9", "RD9",
	"SRN", "GND", "BNG", "GNG", "DJB", "DJT", "DJS", "CGO", "CGS", "ODK", "NGD", "SHM", "TKR",
};

//the UI state analogSetup_onParamChanged() looks at (the weak stubs in stubs.c otherwise)
static int8_t anArmedStep = -1;
int8_t buttonHandler_getArmedAutomationStep(void) { return anArmedStep; }

#define AN_BLOCK_S		((float)OUTPUT_DMA_SIZE / REAL_FS)
#define AN_MAX_BLOCKS	9000	//13 s: the longest instrument (gong, 10 s to -60 dB) and 20 % more
static int16_t anBuf[AN_MAX_BLOCKS * OUTPUT_DMA_SIZE];
static VoiceSlot anVoice;
static uint32_t anHash = 2166136261u;	//of everything rendered: compare.sh compares it between trees

static void anTrigger(VoiceSlot* c, uint8_t slot, uint8_t second, uint8_t vel, uint8_t note)
{
	Analog_trigger(&c->u.analog, vel, note);
	if(slot == NUM_VOICE_SLOTS - 1) c->u.c.oscVolEg.decay = c->decay[second];	//as slot_trigger()
}
static void anRender(VoiceSlot* c, int16_t* out, int blocks)
{
	if(blocks > AN_MAX_BLOCKS)
	{
		printf("analog test: %d blocks don't fit\n", blocks);
		exit(3);
	}
	for(int b = 0; b < blocks; b++)
	{
		SVF_recalcFreq(&c->u.c.filter);
		Analog_calcAsync(&c->u.analog);
		Analog_calcSyncBlock(&c->u.analog, out + b * OUTPUT_DMA_SIZE, OUTPUT_DMA_SIZE);
		anHash = fnv(out + b * OUTPUT_DMA_SIZE, OUTPUT_DMA_SIZE * sizeof(int16_t)) ^ anHash * 16777619u;
	}
}
//a hit of a copy of the slot's voice, alone, into anBuf
static void anCapture(uint8_t slot, uint8_t second, uint8_t vel, uint8_t note, int blocks)
{
	anVoice = voiceSlots[slot];
	hwRngState = 0x12345678;
	anTrigger(&anVoice, slot, second, vel, note);
	anRender(&anVoice, anBuf, blocks);
}
static float anBlockDb(int b)
{
	double e = 0;
	for(int i = 0; i < OUTPUT_DMA_SIZE; i++) e += (double)anBuf[b * OUTPUT_DMA_SIZE + i] * anBuf[b * OUTPUT_DMA_SIZE + i];
	return 10.f * log10f((float)(e / OUTPUT_DMA_SIZE) + 1e-3f);
}
typedef struct { int peakBlock; float peakDb, t60, tau, hz; } AnShape;
//the level curve of anBuf: -60 dB point (last block within 60 dB of the loudest) and the time
//constant of the decay between -20 and -50 dB
static AnShape anShape(int blocks)
{
	AnShape s = { 0, -200.f, 0, 0, 0 };
	for(int b = 0; b < blocks; b++)
	{
		const float db = anBlockDb(b);
		if(db > s.peakDb) { s.peakDb = db; s.peakBlock = b; }
	}
	int b20 = -1, b50 = -1, f0 = -1, f1 = -1;
	for(int b = s.peakBlock; b < blocks; b++)
	{
		const float rel = anBlockDb(b) - s.peakDb;
		if(rel >= -60.f) s.t60 = (b + 1) * AN_BLOCK_S;
		if(b20 < 0 && rel <= -20.f) b20 = b;
		if(b50 < 0 && rel <= -50.f) b50 = b;
		if(f0 < 0 && rel <= -15.f) f0 = b;
		if(f1 < 0 && rel <= -35.f) f1 = b;
	}
	if(b20 >= 0 && b50 > b20)
		s.tau = (b50 - b20) * AN_BLOCK_S * 8.6859f / 30.f;
	(void)f1;
	s.hz = f0;	//the block where the pitch is measured (anPitch)
	return s;
}
//the pitch near expect [Hz]: the strongest autocorrelation for periods of expect/2 .. 2*expect,
//over 4096 samples from the -15 dB point (after the attack's pitch sweeps, while the tone is
//well above any noise the instrument adds)
static float anPitch(const AnShape* s, float expect)
{
	const int start = (int)s->hz * OUTPUT_DMA_SIZE, len = 4096;
	const int lo = (int)(REAL_FS / (2.f * expect)), hi = (int)(REAL_FS / (0.5f * expect));
	double best = -1e30;
	int bestLag = lo;
	static double r[2048];
	for(int lag = lo; lag <= hi && lag < 2048; lag++)
	{
		double acc = 0, e0 = 0, e1 = 0;
		for(int i = 0; i < len; i++)
		{
			const double a = anBuf[start + i], b = anBuf[start + i + lag];
			acc += a * b; e0 += a * a; e1 += b * b;
		}
		r[lag] = acc / sqrt(e0 * e1 + 1e-9);
		if(r[lag] > best) { best = r[lag]; bestLag = lag; }
	}
	//parabolic interpolation between the neighbouring lags
	double frac = 0;
	if(bestLag > lo && bestLag < hi && bestLag < 2047)
	{
		const double y0 = r[bestLag - 1], y1 = r[bestLag], y2 = r[bestLag + 1];
		const double d = y0 - 2 * y1 + y2;
		if(d < 0) frac = 0.5 * (y0 - y2) / d;
	}
	return (float)(REAL_FS / (bestLag + frac));
}
//the amp EG's length on a slot (the instruments have faded to -60 dB when it ends; at most 10 s)
static float anEgLength(uint8_t slot, uint8_t second)
{
	const float d = slot == NUM_VOICE_SLOTS - 1 ? (float)voiceSlots[slot].decay[second] : (float)voiceSlots[slot].u.c.oscVolEg.decay;
	const float t = OUTPUT_DMA_SIZE / (d * REAL_FS);
	return t < 10.f ? t : 10.f;
}
//let the mixer run, so that the voices' smoothed settings (volume, fader, drive) have arrived, as
//on the running device (the voice copies would start their volume from 0 otherwise)
static void anSettle(void)
{
	int peak, nan = 0;
	renderSound(300, &peak, &nan);
}
//choose an instrument like turning the ins knob
static void anChoose(uint8_t slot, uint8_t ins)
{
	const uint16_t par = slot_rolePar[slot][0];
	sendParam(par, ins);
	analogSetup_onParamChanged(par, ins);
	anSettle();
}
static int anCheck(int bad, const char* what)
{
	if(bad) printf("  FAIL: %s\n", what);
	return bad;
}

static int analogTest(void)
{
	int fail = 0;
	kitEngines_setStock(kitEngines);
	kitEngines_apply();
	loadKit(0);
	switchEngine(0, ENGINE_ANALOG);
	switchEngine(NUM_VOICE_SLOTS - 1, ENGINE_ANALOG);

	//--- switching to the engine: BD8, set up
	fail |= anCheck(activeParameters[slot_rolePar[0][0]] != MODEL_BD8 || activeParameters[PAR_VOL_SLOPE1] != 127,
			"a slot switched to analog starts with BD8, set up");

	//--- every instrument chosen by hand
	printf("instrument  setup  tau set/measured (* checked)  -60 dB at / amp EG end  retrigger: fade start/last sample, after 1.45 ms\n");
	for(uint8_t ins = 0; ins < NUM_ANALOG_MODELS; ins++)
	{
		int bad = 0;
		//something else first, so that the setup has to set every value
		sendParam(PAR_COARSE1, 70); sendParam(PAR_FINE1, 20); sendParam(PAR_VELOA1, 30); sendParam(PAR_VOL_SLOPE1, 10);
		sendParam(slot_rolePar[0][1], 90); sendParam(PAR_TRANS1_VOL, 90); sendParam(PAR_DRIVE1, 90); sendParam(PAR_FILTER_TYPE_1, 1);
		anSettle();
		anChoose(0, ins);
		const int setupBad = activeParameters[PAR_COARSE1] != 60 || activeParameters[PAR_FINE1] != 63 || activeParameters[PAR_VELOA1] != 0
				|| activeParameters[PAR_VOL_SLOPE1] != 127 || activeParameters[slot_rolePar[0][1]] != 0 || activeParameters[PAR_TRANS1_VOL] != 0
				|| activeParameters[PAR_DRIVE1] != 0 || activeParameters[PAR_FILTER_TYPE_1] != 7
				|| voiceSlots[0].u.c.oscVolEg.decay != slopeEg2_calcDecay(activeParameters[PAR_VELOD1]);
		bad |= setupBad;

		//the decay: the instrument falls with the time constant the amp decay sets and is at
		//-60 dB when the amp EG ends (the amp EG is a gate)
		const float T = anEgLength(0, 0);
		const int blocks = (int)(T * 1.2f / AN_BLOCK_S) + 20;	//T is at most 10 s
		anCapture(0, 0, 127, 60, blocks);
		const AnShape s = anShape(blocks);
		const float tauSet = T / 6.9078f;
		bad |= s.t60 > T * 1.03f + 3 * AN_BLOCK_S || s.t60 < T * 0.25f || anVoice.u.analog.inst.waveform != ins;

		//a retrigger while it rings: the instrument fades the old hit out from the voice's last
		//sample over about 1.5 ms (the same retrigger rendered without that fade: the difference)
		anVoice = voiceSlots[0];
		hwRngState = 0x12345678;
		anTrigger(&anVoice, 0, 0, 127, 60);
		const int ring = (int)(0.15f * T / AN_BLOCK_S) + 2;
		anRender(&anVoice, anBuf, ring);
		const int last = anBuf[ring * OUTPUT_DMA_SIZE - 1];
		static VoiceSlot noFade;
		static int16_t noFadeBuf[OUTPUT_DMA_SIZE];
		noFade = anVoice;
		noFade.u.analog.inst.model.lastOut = 0.f;
		anTrigger(&anVoice, 0, 0, 127, 60);
		anTrigger(&noFade, 0, 0, 127, 60);
		anRender(&anVoice, anBuf, 1);
		anRender(&noFade, noFadeBuf, 1);
		const int fade0 = anBuf[0] - noFadeBuf[0], fadeEnd = anBuf[OUTPUT_DMA_SIZE - 1] - noFadeBuf[OUTPUT_DMA_SIZE - 1];
		bad |= abs(fade0 - last) > abs(last) / 20 + 64 || abs(fadeEnd) > abs(last) * 45 / 100 + 64;

		//the decay knob sets the time constant of the instruments whose body is one decay
		static const uint8_t oneDecay[] = { MODEL_BD8, MODEL_BD9, MODEL_LT8, MODEL_MT8, MODEL_HT8, MODEL_LT9, MODEL_MT9, MODEL_HT9,
				MODEL_LC8, MODEL_MC8, MODEL_HC8, MODEL_RS8, MODEL_RS9, MODEL_CL8, MODEL_HH8, MODEL_HH9, MODEL_SRN, MODEL_BNG };
		int checkTau = 0;
		for(unsigned k = 0; k < sizeof(oneDecay); k++) checkTau |= oneDecay[k] == ins;
		bad |= checkTau && fabsf(s.tau / tauSet - 1.f) > 0.12f;

		printf("  %s %3u   %s  %7.0f/%7.0f ms %c  %5.0f/%5.0f ms  %6d/%6d %6d%s\n", anNames[ins], activeParameters[PAR_VELOD1],
				setupBad ? "FAIL" : "ok  ", tauSet * 1000.f, s.tau * 1000.f, checkTau ? '*' : ' ', s.t60 * 1000.f, T * 1000.f,
				fade0, last, fadeEnd, bad ? "  FAIL" : "");
		fail |= bad;
	}

	//--- pitch: coarse 0 is the instrument's original tuning, the step note and coarse transpose
	{
		static const struct { uint8_t ins; float hz; } tuned[] =
		{
			{ MODEL_BD8, 52.f }, { MODEL_BD9, 54.f }, { MODEL_LT8, 90.f }, { MODEL_MT8, 135.f }, { MODEL_HT8, 185.f },
			{ MODEL_LT9, 110.f }, { MODEL_MT9, 160.f }, { MODEL_HT9, 220.f },
			{ MODEL_LC8, 185.f }, { MODEL_MC8, 250.f }, { MODEL_HC8, 400.f },
		};
		printf("pitch (Hz, defined / measured):");
		for(unsigned k = 0; k < sizeof(tuned) / sizeof(tuned[0]); k++)
		{
			anChoose(0, tuned[k].ins);
			const int blocks = (int)(anEgLength(0, 0) / AN_BLOCK_S) + 10;
			anCapture(0, 0, 127, 60, blocks);
			const AnShape s = anShape(blocks);
			const float hz = anPitch(&s, tuned[k].hz);
			const int bad = fabsf(hz / tuned[k].hz - 1.f) > 0.04f;
			printf(" %s %.0f/%.1f%s", anNames[tuned[k].ins], tuned[k].hz, hz, bad ? " FAIL" : "");
			fail |= bad;
		}
		printf("\n");
		anChoose(0, MODEL_BD8);
		const int blocks = (int)(anEgLength(0, 0) / AN_BLOCK_S) + 10;
		anCapture(0, 0, 127, 72, blocks);
		AnShape s = anShape(blocks);
		const float noteUp = anPitch(&s, 104.f);
		sendParam(PAR_COARSE1, 72);
		anSettle();
		anCapture(0, 0, 127, 60, blocks);
		s = anShape(blocks);
		const float coarseUp = anPitch(&s, 104.f);
		sendParam(PAR_COARSE1, 60);
		const int bad = fabsf(noteUp / 104.f - 1.f) > 0.04f || fabsf(coarseUp / 104.f - 1.f) > 0.04f;
		printf("BD8 an octave up (104 Hz): step note %.1f Hz, coarse +12 %.1f Hz%s\n", noteUp, coarseUp, bad ? "  FAIL" : "");
		fail |= bad;
	}

	//--- velocity is the accent of these instruments (more click and snap), the others ignore it;
	//with velocity to volume off (vvo, the init kit's setting) only the accent remains
	{
		static const uint8_t accented[] = { MODEL_BD8, MODEL_BD9, MODEL_SD8, MODEL_SD9, MODEL_HH8, MODEL_HH9, MODEL_CR9, MODEL_RD9,
				MODEL_SRN, MODEL_GND, MODEL_BNG, MODEL_GNG, MODEL_DJB, MODEL_DJT, MODEL_DJS, MODEL_CGO, MODEL_CGS,
				MODEL_ODK, MODEL_NGD, MODEL_SHM, MODEL_TKR };
		static int16_t hard[40 * OUTPUT_DMA_SIZE];
		int bad = 0;
		sendParam(PAR_VOLUME_MOD_ON_OFF1, 0);
		printf("accent (largest difference soft/hard hit, %% of the peak):");
		for(uint8_t ins = 0; ins < NUM_ANALOG_MODELS; ins++)
		{
			int isAccented = 0;
			for(unsigned k = 0; k < sizeof(accented); k++) isAccented |= accented[k] == ins;
			anChoose(0, ins);
			anCapture(0, 0, 127, 60, 40);
			memcpy(hard, anBuf, sizeof(hard));
			anCapture(0, 0, 40, 60, 40);
			int peak = 1, diff = 0;
			for(int i = 0; i < 40 * OUTPUT_DMA_SIZE; i++)
			{
				peak = abs(hard[i]) > peak ? abs(hard[i]) : peak;
				diff = abs(hard[i] - anBuf[i]) > diff ? abs(hard[i] - anBuf[i]) : diff;
			}
			const int b = isAccented ? diff * 100 < peak * 3 : diff != 0;
			printf(" %s %d%s", anNames[ins], diff * 100 / peak, b ? " FAIL" : "");
			bad |= b;
		}
		//with velocity to volume on, a soft hit of an instrument without accent is the hard one, quieter
		sendParam(PAR_VOLUME_MOD_ON_OFF1, 1);
		anChoose(0, MODEL_LT8);
		anCapture(0, 0, 127, 60, 40);
		memcpy(hard, anBuf, sizeof(hard));
		anCapture(0, 0, 40, 60, 40);
		int peak = 1, diff = 0;
		for(int i = 0; i < 40 * OUTPUT_DMA_SIZE; i++)
		{
			peak = abs(hard[i]) > peak ? abs(hard[i]) : peak;
			diff = abs(hard[i] - anBuf[i] * 127 / 40) > diff ? abs(hard[i] - anBuf[i] * 127 / 40) : diff;
		}
		sendParam(PAR_VOLUME_MOD_ON_OFF1, 0);
		const int b = diff * 100 > peak;
		printf("\n  vvo on, LT8 soft hit scaled up: %d %% off%s\n", diff * 100 / peak, b ? " FAIL" : "");
		fail |= bad | b;
	}

	//--- saturator: denser, no hard clipping
	{
		anChoose(0, MODEL_BD8);
		const int blocks = 150;
		//denser: a lower peak to average ratio; the peaks stay below full scale
		float crest[2];
		int peak[2], clipped = 0;
		for(int on = 0; on < 2; on++)
		{
			sendParam(slot_rolePar[0][1], on ? 127 : 0);
			anCapture(0, 0, 127, 60, blocks);
			double e = 0;
			peak[on] = 1;
			for(int i = 0; i < blocks * OUTPUT_DMA_SIZE; i++)
			{
				e += (double)anBuf[i] * anBuf[i];
				peak[on] = abs(anBuf[i]) > peak[on] ? abs(anBuf[i]) : peak[on];
				clipped += on && abs(anBuf[i]) >= 32767;
			}
			crest[on] = 20.f * log10f(peak[on] / sqrtf((float)(e / (blocks * OUTPUT_DMA_SIZE))));
		}
		sendParam(slot_rolePar[0][1], 0);
		const int bad = crest[1] > crest[0] - 2.f || clipped > 0;
		printf("saturator on BD8: peak to average %.1f dB -> %.1f dB, peak %d -> %d, %d samples at full scale%s\n",
				crest[0], crest[1], peak[0], peak[1], clipped, bad ? "  FAIL" : "");
		fail |= bad;
	}

	//--- slot 6: HH8 and HH9 closed on track 6 (decay A), open on track 7 (decay B)
	{
		const uint8_t slot = NUM_VOICE_SLOTS - 1;
		for(uint8_t ins = MODEL_HH8; ins <= MODEL_HH9; ins++)
		{
			anChoose(slot, ins);
			const float tA = anEgLength(slot, 0), tB = anEgLength(slot, 1);
			anCapture(slot, 0, 127, 60, (int)(tB * 1.2f / AN_BLOCK_S) + 20);
			const float closed = anShape((int)(tB * 1.2f / AN_BLOCK_S) + 20).t60;
			anCapture(slot, 1, 127, 60, (int)(tB * 1.2f / AN_BLOCK_S) + 20);
			const float open = anShape((int)(tB * 1.2f / AN_BLOCK_S) + 20).t60;
			const int bad = activeParameters[PAR_VELOD6_OPEN] == activeParameters[PAR_VELOD6_CLOSED] || open < 3.f * closed
					|| closed > tA * 1.03f + 3 * AN_BLOCK_S || open > tB * 1.03f + 3 * AN_BLOCK_S;
			printf("%s on slot 6: closed %.0f ms (decay A %u), open %.0f ms (decay B %u)%s\n", anNames[ins], closed * 1000.f,
					activeParameters[PAR_VELOD6_CLOSED], open * 1000.f, activeParameters[PAR_VELOD6_OPEN], bad ? "  FAIL" : "");
			fail |= bad;
		}
	}

	//--- the setup only for the instrument chosen by hand on a slot running the engine
	{
		anChoose(0, MODEL_BD8);
		sendParam(PAR_VELOD1, 100);
		anArmedStep = 3;
		anChoose(0, MODEL_SD8);
		anArmedStep = -1;
		int bad = anCheck(activeParameters[PAR_VELOD1] != 100, "no setup while a step is held");
		seq_recordActive = 1;
		anChoose(0, MODEL_CP8);
		seq_recordActive = 0;
		bad |= anCheck(activeParameters[PAR_VELOD1] != 100, "no setup while recording");
		sendParam(slot_rolePar[0][1], 50);
		analogSetup_onParamChanged(slot_rolePar[0][1], 50);
		bad |= anCheck(activeParameters[PAR_VELOD1] != 100, "no setup for the saturator");
		const uint8_t drumDecay = activeParameters[PAR_VELOD2];
		sendParam(slot_rolePar[1][0], 3);	//slot 2 runs the drum engine: its mod osc waveform
		analogSetup_onParamChanged(slot_rolePar[1][0], 3);
		bad |= anCheck(activeParameters[PAR_VELOD2] != drumDecay, "no setup on a slot with another engine");
		printf("setup only by hand on an analog slot: %s\n", bad ? "FAILED" : "ok");
		fail |= bad;
	}

	//--- a step locked to another instrument, then a step without lock
	{
		const uint16_t par = slot_rolePar[0][0];
		anChoose(0, MODEL_BD8);
		const uint8_t decay = activeParameters[PAR_VELOD1];
		AutomationNode node;
		autoNode_init(&node);
		autoNode_setDestination(&node, par < 128 ? par + 1 : par);	//stored as the sequencer does (seq_automationTarget)
		autoNode_updateValue(&node, MODEL_CP8);
		slot_trigger(0, 127, 60);
		int peak, nan = 0;
		renderSound(1, &peak, &nan);
		const uint8_t locked = voiceSlots[0].u.analog.inst.waveform;
		autoNode_setDestination(&node, NO_AUTOMATION);
		slot_trigger(0, 127, 60);
		renderSound(1, &peak, &nan);
		const uint8_t after = voiceSlots[0].u.analog.inst.waveform;
		const int bad = locked != MODEL_CP8 || after != MODEL_BD8 || activeParameters[PAR_VELOD1] != decay || nan;
		printf("step lock of the instrument: %s\n", bad ? "FAILED" : "ok");
		fail |= bad;
	}

	printf("analog voice renders: hash %08x\n", anHash);
	printf(fail ? "ANALOG TEST FAILED\n" : "analog test passed\n");
	return fail;
}
#endif

#ifdef HAVE_ENGINE_ANALOG
#include "menu/menuPages.h"
//the menu pages of the engines without stock slot, built as menu_updateVoicePages() does: each named
//role exactly once, no hidden parameter, no first screen empty while the second has parameters, the
//encoder reaches every setting;
//the analog engine: ins and sat on the first page, velocity on the Mod page's first screen, FM empty
static int pageTest(void)
{
	int fail = 0;
	for(uint8_t engine = 0; engine < NUM_ENGINES; engine++)
	{
		const KitEngineInfo* info = &kitEngines_info[engine];
		if(info->stockSlot != ENGINE_NO_STOCK_SLOT)
			continue;
		Page pages[NUM_SUB_PAGES];
		int seen[NUM_SLOT_ROLES] = { 0 }, bad = 0;
		for(uint8_t sub = 0; sub < NUM_SUB_PAGES; sub++)
		{
			pages[sub] = menuPages[VOICE1_PAGE + info->templateSlot][sub];
			uint16_t* par = &pages[sub].bot1;
			uint8_t* text = &pages[sub].top1;
			kitEngines_templatePage(engine, par, text);
			int first = 0, second = 0;
			for(uint8_t i = 0; i < 8; i++)
			{
				if(par[i] == PAR_NONE) continue;
				if(i < 4) first++; else second++;
				for(uint8_t h = 0; h < info->numHidden; h++) bad |= par[i] == info->hidden[h];
				for(uint8_t role = 0; role < NUM_SLOT_ROLES; role++)
					if(par[i] == slot_rolePar[info->templateSlot][role])
					{
						seen[role]++;
						bad |= text[i] != info->roleText[role];
					}
			}
			bad |= first == 0 && second > 0;
			//the encoder steps from setting to setting and stops at an empty place: every setting
			//shown must come before the first empty place (places marked skip are stepped over)
			int lastShown = -1;
			for(int i = 0; i < 8; i++) if(par[i] != PAR_NONE) lastShown = i;
			for(int i = 0; i < lastShown; i++) bad |= text[i] == TEXT_EMPTY;
		}
		for(uint8_t role = 0; role < NUM_SLOT_ROLES; role++)
			bad |= seen[role] != (info->roleText[role] != TEXT_EMPTY);
#ifdef HAVE_ENGINE_ANALOG
		if(engine == ENGINE_ANALOG)
		{
			bad |= pages[0].bot1 != PAR_COARSE1 || pages[0].bot3 != slot_rolePar[0][0] || pages[0].bot4 != slot_rolePar[0][1];
			bad |= pages[2].bot1 != PAR_VEL_DEST_1 || pages[2].bot2 != PAR_VELO_MOD_AMT_1 || pages[2].bot3 != PAR_VOLUME_MOD_ON_OFF1;
			const uint16_t* fm = &pages[3].bot1;
			for(uint8_t i = 0; i < 8; i++) bad |= fm[i] != PAR_NONE;
		}
#endif
		printf("menu pages of %s: %s\n", info->name, bad ? "FAILED" : "ok");
		fail |= bad;
	}
	return fail;
}
#endif

#ifdef HAVE_ENGINE_MODAL
//Modal engine regression: renders of the voice alone over its exciters, partials and settings,
//pitches and velocities (with velocity to volume off and on), as a hash (compare.sh compares it between trees, mutate.sh uses it)
static int modalTest(void)
{
	kitEngines_setStock(kitEngines);
	kitEngines_apply();
	loadKit(0);
	switchEngine(0, ENGINE_MODAL);
	int peak, nan = 0;
	renderSound(300, &peak, &nan);
	uint32_t h = 2166136261u;
	int16_t buf[OUTPUT_DMA_SIZE];
	static VoiceSlot c;
	int silent = 0, k = 0;
	for(uint8_t exc = 0; exc < 3; exc++)
		for(uint8_t prt = 0; prt < 4; prt++)
			for(uint8_t v = 0; v < 3; v++, k++)
			{
				static const uint8_t vals[3] = { 10, 64, 120 };
				sendParam(slot_rolePar[0][0], exc);
				sendParam(slot_rolePar[0][6], prt);
				for(uint8_t role = 1; role < 6; role++)	//damping, structure, brightness, position, hardness
					sendParam(slot_rolePar[0][role], vals[(v + role) % 3]);
				sendParam(PAR_VOLUME_MOD_ON_OFF1, k % 2);	//velocity to volume (vvo) off and on
				renderSound(20, &peak, &nan);
				c = voiceSlots[0];
				hwRngState = 0x12345678;
				Modal_trigger(&c.u.modal, (uint8_t)(40 + 40 * v), (uint8_t)(48 + k % 24));
				int last = -1;
				for(int b = 0; b < 300; b++)
				{
					SVF_recalcFreq(&c.u.c.filter);
					Modal_calcAsync(&c.u.modal);
					Modal_calcSyncBlock(&c.u.modal, buf, OUTPUT_DMA_SIZE);
					h = fnv(buf, sizeof(buf)) ^ h * 16777619u;
					for(int i = 0; i < OUTPUT_DMA_SIZE; i++) if(abs(buf[i]) > 32) last = b;
				}
				silent += last < 0;
			}
	printf("modal voice renders: %d settings, %d silent, hash %08x\n", k, silent, h);
	const int fail = nan || silent;
	printf(fail ? "MODAL TEST FAILED\n" : "modal test passed\n");
	return fail;
}
#endif

//------------------------------------------------------------ UI message checks
//The UI's messages carry 7-bit values that the sequencer uses as array indices.
//A request for the Euclid settings of a track must read the settings of tracks
//1-7 only: selecting a pattern sent the menu page number (18 and more) instead
//of the track, which read past the 7-entry Euclid arrays.
extern int harness_euklidMaxTrack;	//stubs.c: highest track the Euclid getters were asked for
static int euklidRequestTest(void)
{
	int fail = 0;
	for(uint8_t track = 0; track < NUM_TRACKS; track++)
	{
		harness_euklidMaxTrack = -1;
		sendFront(FRONT_SEQ_CC, FRONT_SEQ_REQUEST_EUKLID_PARAMS, track);
		const int bad = activeParameters[PAR_EUKLID_LENGTH] != 10 + track
			|| activeParameters[PAR_EUKLID_STEPS] != 20 + track
			|| activeParameters[PAR_EUKLID_ROTATION] != 30 + track;
		if(bad) printf("euklid request track %d: wrong settings arrived\n", track);
		fail |= bad;
	}
	//out of range: the last valid answer must stay, and nothing may be read
	const uint8_t invalid[] = { NUM_TRACKS, PATTERN_LOAD_PAGE, 127 };
	for(unsigned i = 0; i < sizeof(invalid); i++)
	{
		harness_euklidMaxTrack = -1;
		sendFront(FRONT_SEQ_CC, FRONT_SEQ_REQUEST_EUKLID_PARAMS, invalid[i]);
		const int bad = harness_euklidMaxTrack >= NUM_TRACKS
			|| activeParameters[PAR_EUKLID_LENGTH] != 10 + NUM_TRACKS - 1;
		if(bad) printf("euklid request track %d: read past the %d tracks\n", invalid[i], NUM_TRACKS);
		fail |= bad;
	}
	printf("euklid request: %s\n", fail ? "FAILED" : "ok");
	return fail;
}

static int parserTest(void)
{
	const int fail = euklidRequestTest();
	printf(fail ? "PARSER TEST FAILED\n" : "parser test passed\n");
	return fail;
}

//1 = limiter, 0 = stock mix (OpenElixir-02 config "lim"); weak: older trees don't have it
extern uint8_t mixer_limiterOn __attribute__((weak));

//---------------------------------------------------------------- main
int main(int argc, char** argv)
{
	//--no-limiter: render with the stock mix (compare with trees before the 32-bit mix)
	if(argc == 3 && strcmp(argv[1], "--no-limiter") == 0)
	{
		if(&mixer_limiterOn)
			mixer_limiterOn = 0;
		argv++;
		argc--;
	}
#ifdef HAVE_VOICE_SLOTS
	if(argc == 2 && strcmp(argv[1], "--engines") == 0)
	{
		mapPeripherals();
		for(int i = 0; i < 6; i++) faders[i] = 1.f;
		setJackSwitches(0xf);
		soundengine_init();
		return engineTest();
	}
#endif
#ifdef HAVE_ENGINE_MODAL
	if(argc == 2 && strcmp(argv[1], "--modal") == 0)
	{
		mapPeripherals();
		for(int i = 0; i < 6; i++) faders[i] = 1.f;
		setJackSwitches(0xf);
		soundengine_init();
		return modalTest();
	}
#endif
#ifdef HAVE_ENGINE_ANALOG
	if(argc == 2 && strcmp(argv[1], "--analog") == 0)
	{
		mapPeripherals();
		for(int i = 0; i < 6; i++) faders[i] = 1.f;
		setJackSwitches(0xf);
		soundengine_init();
		return analogTest();
	}
#endif
	if(argc == 2 && strcmp(argv[1], "--parser") == 0)
	{
		mapPeripherals();
		soundengine_init();
		return parserTest();
	}
	if(argc != 2)
	{
		fprintf(stderr, "usage: %s [--no-limiter] <output file> | --engines | --modal | --analog | --parser\n", argv[0]);
		return 2;
	}
	mapPeripherals();
	FILE* out = fopen(argv[1], "wb");
	if(!out)
	{
		perror(argv[1]);
		return 2;
	}

	for(int i = 0; i < 6; i++) faders[i] = 1.f;
	setJackSwitches(0xf);
	soundengine_init();

	int16_t output[OUTPUT_DMA_SIZE * 2];
	int16_t output2[OUTPUT_DMA_SIZE * 2];

	const uint32_t tables = tablesHash();
	for(int kit = 0; kit < NUM_KITS; kit++)
	{
		loadKit(kit);
		uint32_t hash = 2166136261u;
		for(int block = 0; block < BLOCKS_PER_KIT; block++)
		{
			//triggers: dense, every track, random notes and velocities
			for(uint8_t track = 0; track < 7; track++)
			{
				if(rnd(8) == 0)
				{
					voiceControl_noteOn(track, (uint8_t)(24 + rnd(72)), (uint8_t)(1 + rnd(127)));
				}
			}
			//parameter changes while voices sound (knob turns)
			if(kit > 0 && rnd(4) == 0)
			{
				uint16_t par;
				do { par = (uint16_t)rnd(END_OF_SOUND_PARAMETERS); } while(skipParam(par));
				sendParam(par, randomValue(par));
			}

			mixer_calcNextSampleBlock(output, output2);
			fwrite(output, sizeof(output), 1, out);
			fwrite(output2, sizeof(output2), 1, out);
			for(unsigned i = 0; i < sizeof(output) / 2; i++)
			{
				hash = (hash ^ (uint16_t)output[i]) * 16777619u;
				hash = (hash ^ (uint16_t)output2[i]) * 16777619u;
			}
		}
		printf("kit %2d %08x\n", kit, hash);
		if(tablesHash() != tables)
		{
			fprintf(stderr, "kit %d: constant tables in DTCM were overwritten (out-of-bounds write)\n", kit);
			return 3;
		}
	}
	fclose(out);
	return 0;
}
