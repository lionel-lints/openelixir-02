/*
 * Timeline of a sequencer run: every event with the audio block and system tick it
 * happened in. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef TEST_SEQUENCER_TIMELINE_H_
#define TEST_SEQUENCER_TIMELINE_H_

//write "block systick name a b c" (fields of -1 are left out)
void timeline_event(const char* name, int a, int b, int c);

#endif
