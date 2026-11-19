# Engine render test (host)

Compiles the audio engine (`Core/Src/Soundengine/...`) for the PC and renders 32 kits
(the init kit and 31 random kits within the UI's value ranges) with dense random triggers
on all 7 tracks and parameter changes while voices sound. Every sample of both DACs is
written to a file, so two source trees can be compared bit for bit. Used to prove that
engine refactorings don't change the sound.

```sh
test/engine/compare.sh            # working tree vs. HEAD
test/engine/compare.sh v0.1       # working tree vs. a tag/commit
```

`render --no-limiter <file>` renders with the stock mix (config "lim" off); compared with a tree
from before the 32-bit mix bus (bf9efc7 plus the ping-pong delay fix) it is bit-identical.

`render --engines` (built by `build.sh`, e.g. `build/engine-test/out-work/render --engines`)
checks engine switching instead: every slot gets every engine (switched like a received
NRPN 98, including the UI side in `Preset/KitEngines.c`). Each track must sound, no voice
state may become NaN, track 7 must use decay B, track 6 must cut track 7, and each of the
slot's 7 engine specific parameters must change the sound of the engine it runs (except
the roles that engine doesn't have). The voice level checks render a copy of the voice,
so they don't depend on the mixer, FX or the other voices.

`render --analog` checks the Analog engine against what its instruments are defined to do, from
the rendered voice (alone, after the mixer's smoothing has settled):
- every instrument chosen by hand (as turning `ins`) gets the setup (coarse/fine 0, attack 0, its
  decay, slope 127, saturator, transient, drive and filter off), and nothing else triggers it: not
  while a step is held or recording, not the saturator, not a slot with another engine;
- the decay: the time constant set by the decay knob (measured between -20 and -50 dB, within 12 %
  for the instruments with one decay: kicks, toms, congas, rims, claves, hats, saron, bonang), and
  -60 dB no later than the end of the amp EG;
- a retrigger while ringing fades the old hit out from the voice's last sample (compared with the
  same retrigger without the fade): no click;
- pitch: the defined frequencies of the kicks, toms and congas at coarse 0, an octave up by the step
  note and by coarse;
- velocity: the accent changes exactly the instruments that have one, the others render identically
  at any velocity (velocity to volume off); with it on, a soft hit is the hard one, quieter;
- saturator: denser (lower peak to average), no clipping; slot 6: HH8/HH9 closed on track 6, open
  on track 7; a step lock of the instrument plays it on that step only.
It prints a hash of everything it rendered; `compare.sh` compares it between the two trees when
both have the engine.

`render --modal` renders the Modal voice alone over its 3 exciters, 4 partial counts and 3 sets of
the other settings, at several pitches and velocities (velocity to volume off and on), and prints a
hash; every setting must sound.
`compare.sh` compares the hash between the trees like the Analog one.

`render --parser` checks how the sequencer side handles the UI's messages: their 7-bit values
are used as array indices, so out-of-range values must be ignored. A request for the Euclid
settings of a track must answer with that track's settings for tracks 1-7 and read nothing for
any other value (selecting a pattern used to send the menu page number).

Comparing with a tree from before 2.0 (e.g. `compare.sh v1.4`): those trees call `__CLZ(0)` in
`Oscillator.c` for every oscillator below 440 Hz. That is undefined; the hardware gives table 4,
the host (x86) gave table 5, so their host renders differ from 2.0 there while the hardware sound
doesn't. Add `if(val == 0) return 255;` to `fast_log2()` in the old tree: then the renders are
bit-identical.

How it works:
- The engine is driven only through entry points that stay stable across refactorings:
  `soundengine_init()`, `frontParser_parseUartData()` (the UI's byte stream: parameters,
  LFO and velocity targets), `voiceControl_noteOn()` and `mixer_calcNextSampleBlock()`.
- Strict IEEE floating point (no `-ffast-math`), so code that keeps the arithmetic gives
  identical bits. On the target, `-ffast-math` may round differently; listen there.
- `shim/host_cmsis.h` replaces the ARM intrinsics (`__QADD16`, `__SSAT`, ...) with C
  versions; peripheral register ranges are mapped as plain memory (`mmap`).
- `stubs.c`: no-op stand-ins for the sequencer, UI, UART, USB and codec. If the link fails
  with new undefined references outside the engine, add stubs there.
- Value ranges are extracted from `menu.c` at build time (`gen_ranges.py`).
- The harness keeps `activeParameters` (the UI's copy of the kit) up to date, because the
  engine reads it back when it resets modulation targets.
- After every kit the constant tables in DTCM (transient samples, sine table) must be
  unchanged; otherwise `render` stops with exit status 3. An out-of-bounds write into them
  changes the sound depending on the memory layout. (This found the ping-pong delay bug.)
- `mutate.sh`: checks that a tiny change in each voice engine is detected (the Modal and Analog
  voices through `render --modal` and `render --analog`: the kit renders use the stock engines).
- `stats.py`: level statistics per kit of a render file.

Linux only (uses `mmap` with fixed addresses). Needs gcc and Python 3.

## Modal engine prototype

`test/engine/modal.sh [dir]` builds the resonator of the Modal engine
(`Core/Src/Soundengine/DSPAudio/ModalResonator.c`) with `modal/modal_render.c` and renders parameter
sweeps as WAV files (default `build/modal-test`): structure, damping, brightness, position, hardness,
noise burst, sustained exciter, partials and a pitch run. It prints the struct size, the level of
each hit before the soft limit and the number of modes computed.
