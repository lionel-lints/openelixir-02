/*
 * GlobalDefaults.h
 *
 * The settings (glo.cfg, the global part of a project) that a file of an older version
 * doesn't have: they get their defaults, the ones it has stay.
 *
 * Copyright (C) 2026 OpenElixir-02 contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenElixir-02, based on the LXR firmware. It is licensed under the
 * GNU General Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef PRESET_GLOBALDEFAULTS_H_
#define PRESET_GLOBALDEFAULTS_H_

#include <stdint.h>
#include "Parameters.h"

//params: activeParameters; bytesRead: how many settings bytes the file had (from
//PAR_BEGINNING_OF_GLOBALS on). Stock files end before the limiter, 1.4 files before the
//swing mode.
static inline void preset_defaultMissingGlobals(uint8_t* params, const uint16_t bytesRead)
{
	if(bytesRead <= PAR_MIXER_LIMITER - PAR_BEGINNING_OF_GLOBALS)
		params[PAR_MIXER_LIMITER] = 1;				//on
	if(bytesRead <= PAR_SWING_MODE - PAR_BEGINNING_OF_GLOBALS)
		params[PAR_SWING_MODE] = 0;					//Not: clock outputs straight
	if(params[PAR_SWING_MODE] > 1)
		params[PAR_SWING_MODE] = 0;
}

#endif /* PRESET_GLOBALDEFAULTS_H_ */
