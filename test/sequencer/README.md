# Sequencer timing test (host)

Runs the real sequencer (`Core/Src/Soundengine/Sequencer/sequencer.c` and `clockSync.c`) on the
PC and writes a timeline of everything that has a time: voice triggers (`TRIG track note vel`),
MIDI out (`CLK` for clocks, `MIDI status data1 data2` for the rest), clock output ticks
(`TICK onOff cnt`) and, in recording scenarios, the played hits (`HIT step`) and the steps that were
recorded (`REC step`). Every line starts with the audio block and the system tick it happened in.

```sh
test/sequencer/run.sh             # build, run all scenarios, compare with golden/
test/sequencer/run.sh --capture   # write golden/ (only on purpose: they are the reference)
```

Linux only, like `test/engine` (on a Mac: in Docker, building into the container, not into a mounted
macOS folder). Needs gcc.

Time model, from the firmware: the system tick (TIM14) runs at 108 MHz / 13501 / 2 (0.25 ms), the
audio engine renders 64-sample blocks at 44140 Hz (1.45 ms) and calls `seq_tick()` before each
block; external clocks and recorded hits are handled after `seq_tick()` in the block that reaches
them, like received MIDI. Each scenario runs in its own process, so no state is shared.

Scenarios (`main.c`): straight 16ths; swing 0-127 at 150 bpm (and 64 at 93 bpm) with flams and
micro-shifts on straight and swung steps; a 15-step track; a pattern change; every roll speed (and
1/8 and 16 with swing); MIDI clock and analog clock in; recording, 3 ms early and 3 ms late (internal clock and MIDI clock).

`golden/` was captured from 1.4.1 (the stock sequencer timing) before the note swing work: a change
that keeps the timing must reproduce it exactly. Checked to fail on small changes (swing x15/16
instead of x16/16: the 14 internal-clock swing scenarios differ; MIDI clock one tick later: every
internal-clock scenario differs).

What the 1.4.1 timelines show (stock behaviour):
- Swing stretches the clock: the MIDI clock intervals range 5.8-50.7 ms at swing 64 and 150 bpm
  (16.7 ms without swing) and the flams of swung steps are squeezed.
- Following an external clock with swing drops notes: 52 instead of 61 hat hits (MIDI and analog
  clock, swing 64), the next external step cuts off the last ticks of a swung step.
- Recording: a hit played 3 ms early landed one step too early (any swing); 3 ms late was right.
  The step position counter wrapped to 0 % after the last tick of a step, before the next step
  started. Fixed in both swing modes (recording now measures from the time the step started, on the
  swung grid); the three `record_*_early` timelines were updated for it, on purpose.

`run.sh` also checks every recording scenario directly: the recorded steps must be the played steps.
