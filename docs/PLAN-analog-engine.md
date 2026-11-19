# Plan: the Analog engine (808/909 and acoustic instruments)

Drafted 2026-11-19. Base: `lionel/note-swing` (2.0 work incl. the Modal engine), work on
`lionel/analog-engine`. Source: [AnalogModels](https://github.com/G3nzalo/analogmodels-firmware)
0.5 by G3nzalo (GPL-3.0), `Core/Src/Soundengine/DSPAudio/AnalogModels.c` and its headers.

## Goal

A sixth voice engine, "analog", selectable per voice slot with SHIFT + KIT like the Modal engine:
the 38 instruments of AnalogModels, sounding exactly as in AnalogModels, played through the
voice's usual filter, transient, amp envelope, drive, LFO, velocity, pan, outputs and FX.

## What AnalogModels provides

- 25 instruments modelled after the TR-808 and TR-909 circuits (bass drums, snares, toms, congas,
  rim shots, claves, claps, maracas, cowbell, hats, cymbal, crash, ride) and 13 acoustic ones
  (gamelan saron, gender, bonang, gong; djembe bass, tone, slap; conga open and slap; taiko
  odaiko, nagado, shime, rim). Each makes its own pitch sweeps, clicks, noise and envelopes.
- Interface: `model_trigger(osc, vel)` starts a hit, `model_calcBlock(osc, buf, size, gain, sat)`
  renders a block instead of the oscillator. The instrument uses from the voice: the instrument
  number, its state (`ModelState`, about 170 bytes), the pitch (`phaseInc`), the amp envelope's
  level and decay per block (the decay knob sets the instrument's length), the velocity (accent:
  more click and snap), and `sat`, a tube saturator (0 = off).
- Choosing an instrument sets up the voice so it plays like the original (`modelPresets.c`):
  coarse/fine 0 = original tuning, amp envelope as a gate with the instrument's decay, pitch EG,
  FM, transient, filter and drive off.
- Measured cost (AnalogModels' `tools/m7_bench`): 11k-15k instructions per block for most
  instruments on a drum voice, 16k-24k for the heaviest, against about 313k cycles per block.

## Decisions

1. **An engine, not extra waveforms.** AnalogModels adds the instruments to every waveform list;
   here they become engine 5, "analog" (`ana`), with no stock slot, Drum 1's page as the template,
   its oscillator waveform and PWM hidden (as Modal). Kits with it load on older OpenElixir and the
   stock firmware with the slot's stock engine (AnalogModels: the oscillator plays silent there).
2. **Two knobs of its own** (roles): `ins` instrument (a list of the 38, AnalogModels' 3-character
   names: BD8 BD9 SD8 SD9 LT8 MT8 HT8 LT9 MT9 HT9 LC8 MC8 HC8 RS8 RS9 CL8 CP8 CP9 MA8 CB8 HH8 HH9 CY8
   CR9 RD9 SRN GND BNG GNG DJB DJT DJS CGO CGS ODK NGD SHM TKR), and `sat` tube saturator (0-127,
   default 0). The other five roles are unused (hidden). Decay, coarse, fine, velocity: the
   voice's own knobs, as in AnalogModels.
3. **Acoustic instruments included** (decided 2026-11-19).
4. **Choosing an instrument by hand sets up the voice** like AnalogModels (its `modelPresets.c`),
   not while a step is held or automation is recorded. Switching the engine to analog starts with
   instrument BD8 and that setup. (`menu/analogSetup.c`)
4a. **`ins` and `sat` on the first page**, in the places of the hidden oscillator waveform and PWM
   (a new `moved` list in the engine description), not on the FM and Mod pages where roles 0 and 1
   sit on the drum page.
4b. **A new instrument starts on the next hit**: a ringing hit keeps its instrument (its state means
   something else to another instrument). Step locks work with this: the lock is applied before the
   step's trigger.
5. **The DSP stays as it is.** `AnalogModels.c` is taken over with the smallest change: its state
   comes from the engine's voice instead of `OscInfo`. Rendered output must be bit-identical to
   AnalogModels' code for the same input.
6. **Slot 6** (closed/open hat): track 6 plays with decay A, track 7 with decay B, as with the
   other engines; the instrument is the slot's.
7. **One instrument per step** through the existing step automation on the `ins` parameter: works,
   verified by `render --analog`.
8. **Out of scope:** AnalogModels' 8 parameter locks, slendro/pelog scales, voice FX, its
   stock-firmware fixes (the ones that matter are in this build already).

## Tests

- Port fidelity: every instrument (38) rendered on the host, several pitches, velocities, decays
  and saturator settings, by AnalogModels' original code and by the ported engine code with the same
  input (same random generator): bit-identical.
- Engine: `test/engine --engines` covers the analog engine in every slot (it sounds, no NaN, slot 6
  decay A/B, track 6 cuts track 7), the existing engines render bit-identical to before.
- Kits: a kit with the analog engine saves and loads; without the engine block it loads stock.
- Step automation on `ins`: a step plays another instrument.
- All other suites stay green (sequencer timing, Euclid, settings, image safety, parser).
- Hardware: CPU with the analog engine on all six voices at once (CPU meter build), listening.

## Delivery

On `lionel/analog-engine`:

1. The DSP (AnalogModels.c and headers, decoupled from `OscInfo`) and the bit-exact port test.
2. The engine: voice wrapper (after `ModalVoice.c`), engine table entry, role names and the
   instrument list, NRPN 98 values, instrument setup on selection.
3. Engine, kit and automation tests; docs (ENGINES.md, README, release notes, credits).
4. Test image 1.99.5, CPU meter build, hardware check, merge.

Credits: AnalogModels by G3nzalo, GPL-3.0; noted in NOTICE.md and the docs.

Found on the way: the UI decoded a received NRPN 98 only for the engines 0-3, so a received modal
(24-29) switched the sound but not the menu. Both sides now share `slot_decodeEngineCommand()`.

Note on lengths: the decays of the setup are AnalogModels'; its comments ("BD8 220 ms") describe the
audible length, the instrument reaches -60 dB at the end of the amp EG (BD8: about 1.5 s).
