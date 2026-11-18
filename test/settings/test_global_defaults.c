/*
 * Settings files of older versions (glo.cfg, the global part of projects): the settings they
 * don't have get their defaults, the ones they have stay. Host test:
 *     test/settings/run.sh
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include "Preset/GlobalDefaults.h"

//the file formats: the OpenElixir settings follow the stock ones in this order, and projects
//keep all settings in a 64-byte block
_Static_assert(PAR_SWING_MODE == PAR_MIXER_LIMITER + 1, "settings appended in order");
_Static_assert(NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS <= 64, "settings fit the project block");

static uint8_t params[NUM_PARAMS];
static int fails;

static void check(const char* name, uint16_t bytesInFile, uint8_t limiterInFile, uint8_t swingInFile,
		uint8_t limiterWant, uint8_t swingWant)
{
	memset(params, 0xAA, sizeof(params));		//what was there before loading
	if(bytesInFile > PAR_MIXER_LIMITER - PAR_BEGINNING_OF_GLOBALS) params[PAR_MIXER_LIMITER] = limiterInFile;
	if(bytesInFile > PAR_SWING_MODE - PAR_BEGINNING_OF_GLOBALS) params[PAR_SWING_MODE] = swingInFile;
	preset_defaultMissingGlobals(params, bytesInFile);
	const int ok = params[PAR_MIXER_LIMITER] == limiterWant && params[PAR_SWING_MODE] == swingWant;
	printf("%-36s limiter %d swing %d: %s\n", name, params[PAR_MIXER_LIMITER], params[PAR_SWING_MODE], ok ? "ok" : "FAILED");
	fails += !ok;
}

int main(void)
{
	const uint16_t stockBytes = PAR_MIXER_LIMITER - PAR_BEGINNING_OF_GLOBALS;
	const uint16_t oe14Bytes = PAR_SWING_MODE - PAR_BEGINNING_OF_GLOBALS;
	const uint16_t allBytes = NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS;
	check("stock file", stockBytes, 0, 0, 1, 0);
	check("1.4 file, limiter off (kept)", oe14Bytes, 0, 0, 0, 0);
	check("1.4 file, limiter on", oe14Bytes, 1, 0, 1, 0);
	check("2.0 file, limiter off, Clk", allBytes, 0, 1, 0, 1);
	check("2.0 file, Not", allBytes, 1, 0, 1, 0);
	check("2.0 file, damaged swing value", allBytes, 1, 7, 1, 0);
	printf(fails ? "SETTINGS TEST FAILED\n" : "settings test passed\n");
	return fails != 0;
}
