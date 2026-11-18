/*
 * Stand-ins for everything the sequencer calls outside sequencer.c and clockSync.c.
 * Calls that are timing results are written to the timeline (see timeline.h): voice
 * triggers, MIDI out and the clock output ticks. Everything else does nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "Parameters.h"
#include "menu/menu.h"
#include "Preset/PresetManager.h"
#include "Soundengine/FxBus.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "timeline.h"

//--- state the sequencer reads or writes
uint8_t activeParameters[NUM_PARAMS];
FxBus fxBus;
uint8_t frontParser_activeTrack;
uint8_t frontParser_shownPattern;
uint8_t menu_activePage, menu_playedPattern, menu_shownPattern, menu_activeBar, menu_songMenuPosition;
uint8_t menu_selectedPresetNr[NUM_PRESET_LOCATIONS];
uint8_t menu_loadedPresetNr[NUM_CURRENT];
uint8_t midi_MidiChannels[8];
uint8_t midi_process_block;
uint8_t midiParser_txRxFilter = 0xff;
volatile uint32_t systick_ticks;
uint8_t trigger_clockReceived;

//--- timing results: written to the timeline
void voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel) { timeline_event("TRIG", voice, note, vel); }
void voiceControl_noteOff(uint8_t voice) { timeline_event("OFF", voice, -1, -1); }
void uart_sendMidi(MidiMsg msg)
{
	if(msg.status == MIDI_CLOCK)
		timeline_event("CLK", -1, -1, -1);
	else
		timeline_event("MIDI", msg.status, msg.data1, msg.data2);
}
void usb_sendMidi(MidiMsg msg) { (void)msg; }	//same messages as uart_sendMidi
void trigger_clockTick(uint8_t onOff, uint8_t cnt) { timeline_event("TICK", onOff, cnt, -1); }
void trigger_reset(uint8_t value) { timeline_event("TRESET", value, -1, -1); }

//--- deterministic randomness (probability is 100 unless a scenario changes it)
static uint32_t rng = 1;
uint32_t GetRngValue() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

//--- UI, presets, LFO: not part of the timing
void autoNode_init(void* n) { (void)n; }
void autoNode_setDestination(void* n, uint16_t d) { (void)n; (void)d; }
void autoNode_updateValue(void* n, uint8_t v) { (void)n; (void)v; }
uint8_t buttonHandler_getMode() { return 0; }
void led_blinkHandler() { }
void led_clearActive_step() { }
void led_clearAllBlinkLeds() { }
void led_clearSequencerLeds() { }
uint8_t led_idx2StepLed(uint8_t idx) { return idx; }
void led_initPatternSelectLeds() { }
void led_resetBlinkTimer() { }
void led_setActive_step(uint8_t stepNr) { (void)stepNr; }
void led_setBlinkLed(const uint8_t ledNr, const uint8_t onOff) { (void)ledNr; (void)onOff; }
void led_setValue(uint8_t val, uint8_t ledNr) { (void)val; (void)ledNr; }
void led_updateSeqLeds() { }
void lfo_recalcSync() { }
uint8_t menu_getActiveVoice() { return 0; }
uint8_t menu_getViewedPattern() { return menu_shownPattern; }
void menu_repaint() { }
void menu_repaintAll() { }
void menu_setShownPattern(uint8_t patternNr) { menu_shownPattern = patternNr; }
void midiParser_checkMtc() { }
void preset_loadDrumsetRam(uint8_t presetNr, uint8_t isMorph, uint8_t sendImmediate) { (void)presetNr; (void)isMorph; (void)sendImmediate; }
void preset_patternActivated(void) { }
void preset_touchTimer(PresetTimer type) { (void)type; }
void uart_sendFrontpanelByte(uint8_t data) { (void)data; }
void uart_sendFrontpanelSysExByte(uint8_t data) { (void)data; }
