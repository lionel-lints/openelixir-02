/*
 * analogSetup.c
 *
 * Part of a custom firmware for the Erica Synths LXR-02 (not an official
 * Erica Synths release). Added 2026. The decays are AnalogModels' (modelPresets.c).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Licensed under the GNU General Public License version 3, with the
 * additional permission and terms in NOTICE.md. See LICENSE and NOTICE.md in
 * the root of the repository.
 */
#include "menu/analogSetup.h"
#include "menu/menu.h"
#include "Parameters.h"
#include "Preset/KitEngines.h"
#include "buttonHandler.h"
#include "frontPanelParser_ui.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Soundengine/Sequencer/sequencer.h"

#define TUNING_ORIGINAL	60		//coarse 0 on the display: the instrument's original pitch
#define FINE_CENTER		63
#define SLOPE_GATE		127		//the amp EG stays open until it closes at the end
#define FILTER_OFF		7

//amp decay (closed, open for the hats on slot 6: 0 = the same)
typedef struct { uint8_t decay, decayOpen; } AnalogPreset;
static const AnalogPreset analogPresets[NUM_ANALOG_MODELS] =
{
	{ 65,  0 },	// BD8 220 ms
	{ 47,  0 },	// BD9 120 ms after a 10 ms hold
	{ 26,  0 },	// SD8  55 ms snappy
	{ 32,  0 },	// SD9  70 ms snappy
	{ 44,  0 },	// LT8 110 ms
	{ 37,  0 },	// MT8  85 ms
	{ 32,  0 },	// HT8  70 ms
	{ 51,  0 },	// LT9 140 ms
	{ 46,  0 },	// MT9 120 ms
	{ 41,  0 },	// HT9 100 ms
	{ 32,  0 },	// LC8  70 ms
	{ 28,  0 },	// MC8  60 ms
	{ 24,  0 },	// HC8  50 ms
	{  6,  0 },	// RS8  10 ms
	{  8,  0 },	// RS9  15 ms
	{  8,  0 },	// CL8  14 ms
	{ 29,  0 },	// CP8  60 ms tail
	{ 35,  0 },	// CP9  75 ms tail
	{ 12,  0 },	// MA8  22 ms
	{ 31,  0 },	// CB8  70 ms tail
	{  7, 44 },	// HH8  12 ms closed, 110 ms open
	{  8, 55 },	// HH9  15 ms closed, 160 ms open
	{ 79,  0 },	// CY8 350 ms
	{ 87,  0 },	// CR9 450 ms
	{ 89,  0 },	// RD9 500 ms
	{ 84,  0 },	// SRN 400 ms
	{ 93,  0 },	// GND 570 ms
	{ 74,  0 },	// BNG 290 ms
	{120,  0 },	// GNG 1.45 s (10 s to -60 dB, the longest)
	{ 37,  0 },	// DJB  87 ms
	{ 41,  0 },	// DJT 100 ms
	{ 25,  0 },	// DJS  52 ms
	{ 49,  0 },	// CGO 130 ms
	{ 22,  0 },	// CGS  44 ms
	{ 80,  0 },	// ODK 360 ms
	{ 58,  0 },	// NGD 180 ms
	{ 25,  0 },	// SHM  52 ms
	{ 10,  0 },	// TKR  18 ms
};

//---------------------------------------------------
static void analogSetup_param(uint8_t* params, const uint16_t par, const uint8_t value, void (*send)(uint16_t par))
{
	params[par] = value;
	if(send)
		send(par);
}
//---------------------------------------------------
void analogSetup_set(const uint8_t slot, const uint8_t instrument, uint8_t* params, void (*send)(uint16_t par))
{
	if(slot >= NUM_VOICE_SLOTS || instrument >= NUM_ANALOG_MODELS)
		return;
	const AnalogPreset* p = &analogPresets[instrument];
	analogSetup_param(params, PAR_COARSE1 + 2*slot, TUNING_ORIGINAL, send);
	analogSetup_param(params, PAR_FINE1 + 2*slot, FINE_CENTER, send);
	analogSetup_param(params, PAR_VELOA1 + 2*slot, 0, send);
	analogSetup_param(params, PAR_VELOD1 + 2*slot, p->decay, send);
	if(slot == NUM_VOICE_SLOTS-1)
		analogSetup_param(params, PAR_VELOD6_OPEN, p->decayOpen ? p->decayOpen : p->decay, send);
	analogSetup_param(params, PAR_VOL_SLOPE1 + slot, SLOPE_GATE, send);
	analogSetup_param(params, slot_rolePar[slot][1], 0, send);	//tube saturator
	analogSetup_param(params, PAR_TRANS1_VOL + slot, 0, send);
	analogSetup_param(params, PAR_DRIVE1 + slot, 0, send);
	analogSetup_param(params, PAR_FILTER_TYPE_1 + slot, FILTER_OFF, send);
}
//---------------------------------------------------
static void analogSetup_send(uint16_t par)
{
	if(par < 128)
		frontPanel_sendData(MIDI_CC, (uint8_t)par, activeParameters[par]);
	else if(par < END_OF_SOUND_PARAMETERS)
		frontPanel_sendData(CC_2, (uint8_t)(par-128), activeParameters[par]);
}
//---------------------------------------------------
void analogSetup_onParamChanged(const uint16_t par, const uint8_t value)
{
	const uint8_t slot = kitEngines_rolePar2Slot(par);
	if(slot >= NUM_VOICE_SLOTS || kitEngines[slot] != ENGINE_ANALOG || slot_rolePar[slot][0] != par)
		return;
	if((buttonHandler_getArmedAutomationStep() != NO_STEP_SELECTED) || seq_recordActive)
		return;
	analogSetup_set(slot, value, activeParameters, analogSetup_send);
	menu_repaintAll();
}
