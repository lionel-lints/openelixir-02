/*
 * analogSetup.h
 *
 * Choosing an instrument of the Analog engine sets up the rest of its voice
 * so it plays like the original instrument (after AnalogModels'
 * modelPresets.c): coarse 0 is the original tuning, the amp EG is a gate with
 * the instrument's decay, transient, filter, drive and the saturator are off.
 *
 * Part of a custom firmware for the Erica Synths LXR-02 (not an official
 * Erica Synths release). Added 2026.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Licensed under the GNU General Public License version 3, with the
 * additional permission and terms in NOTICE.md. See LICENSE and NOTICE.md in
 * the root of the repository.
 */

#ifndef ANALOGSETUP_H_
#define ANALOGSETUP_H_

#include <stdint.h>

//the voice settings of instrument in params[] (a kit's sound parameters) for slot 0-5; the
//parameters it changes go to send (may be NULL)
void analogSetup_set(const uint8_t slot, const uint8_t instrument, uint8_t* params, void (*send)(uint16_t par));

//par was set to value by hand (knob or encoder): if it is the instrument of a slot that runs
//the Analog engine, set up the voice (not while a step is held, where it becomes a parameter
//lock of that step, and not while recording automation)
void analogSetup_onParamChanged(const uint16_t par, const uint8_t value);

#endif /* ANALOGSETUP_H_ */
