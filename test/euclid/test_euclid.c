/*
 * Euclidean generator: k hits spread as evenly as possible over a length, through the same
 * calls the menu makes (euklid_setLength, euklid_setSteps). Compiled with the undefined
 * behaviour sanitizer: the generator used __CLZ(0) (undefined), which GCC 15 turned into
 * "k hits in a row" on the LXR. Host test, Linux:
 *     test/euclid/run.sh
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include "stm32f7xx_hal.h"
#include "Soundengine/Sequencer/sequencer.h"
PatternSet seq_patternSet;
PatternBackupBuffer patternBackupBuffer;
#include "../../Core/Src/Soundengine/Sequencer/EuklidGenerator.c"

static int fails;

static void pattern(int len, int k, char* out)
{
	euklid_setLength(0, len);
	euklid_setSteps(0, k, 0);
	for(int s = 0; s < len; s++)
		out[s] = (seq_patternSet.seq_mainSteps[0][0][0] & (1 << s)) ? 'x' : '.';
	out[len] = 0;
}

static void expect(int len, int k, const char* want)
{
	char got[17];
	pattern(len, k, got);
	if(strcmp(got, want)) { printf("len %d steps %d: %s, expected %s\n", len, k, got, want); fails++; }
}

int main(void)
{
	euklid_init();
	expect(16, 2, "x.......x.......");		//the reported case: steps 1 and 9
	expect(16, 3, "x....x....x.....");
	expect(16, 4, "x...x...x...x...");
	expect(8, 3, "x..x..x.");

	//every length 1-16 and number of steps: k hits, the first on step 1 (rotation 0), the
	//gaps between hits (around the loop) differ by at most one step, the track length is set
	for(int len = 1; len <= 16; len++)
		for(int k = 1; k <= len; k++)
		{
			char p[17];
			pattern(len, k, p);
			int hits = 0, first = -1, prev = -1, minGap = 99, maxGap = 0;
			for(int s = 0; s < len; s++)
			{
				if(p[s] != 'x') continue;
				hits++;
				if(first < 0) first = s;
				if(prev >= 0) { int g = s - prev; minGap = g < minGap ? g : minGap; maxGap = g > maxGap ? g : maxGap; }
				prev = s;
			}
			int wrap = first + len - prev;
			minGap = wrap < minGap ? wrap : minGap; maxGap = wrap > maxGap ? wrap : maxGap;
			if(hits != k || first != 0 || maxGap - minGap > 1 || seq_patternSet.length[0][0] != len)
			{
				printf("len %d steps %d: %s (hits %d, gaps %d-%d, length %d)\n", len, k, p, hits, minGap, maxGap, seq_patternSet.length[0][0]);
				fails++;
			}
		}
	printf(fails ? "EUCLID TEST FAILED\n" : "euclid test passed (136 patterns)\n");
	return fails != 0;
}
