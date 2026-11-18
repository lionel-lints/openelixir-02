# Plan: note swing, steady clock, faster rolls (2.0.0)

Agreed 2026-11-18, amended the same day after a review. Base: `lionel/v1.4-fixes` (1.4.1), work on
`lionel/note-swing`.

## Goal

The MIDI clock out and the clock outputs stay steady at any swing setting, while swung notes land
where they do today. Faster and triplet rolls are built on the same foundation.

"Steady" means: no swing in the clock. The clock keeps the jitter every LXR clock has, up to one
audio block (1.45 ms; at 120 bpm the MIDI clock intervals are 20.3-21.75 ms, measured on 1.4.1
without swing). Sub-block clock timing (a hardware timer for the clock bytes) is not part of this
plan; it becomes a separate decision if the DAW test shows the need.

## How the stock timing works (for reference)

- `seq_tick()` runs once per audio block (64 samples at 44140 Hz, 1.45 ms) and advances a 96 ppq
  clock: 24 ticks per 16th step. Tick 0 calls `seq_nextStep()` (step positions, pattern changes,
  rolls); every tick calls `seq_processFlam()` (notes with flam and micro-shift); every 4th tick
  sends a MIDI clock; the clock outputs tick as well.
- Swing stretches the clock: on tick 23 of every odd step the next tick interval is lengthened by
  shuffle/127 x 16 ticks and repaid on the following one. The 2nd and 4th 16th of each beat start
  late, but the MIDI clock and clock outputs swing too (5.8-50.7 ms MIDI clock intervals at swing
  64, 150 bpm), and the ticks after the delay fire back to back (flams and shifts on swung steps get
  squeezed).
- Following an external clock (MIDI clock: `sync_tick()`; analog clock: `trigger_clockReceived` in
  `seq_tick()`), steps start on the external clock; swing postpones swung steps with
  `sync_delayedStepTimer`, and the next external step cuts off a swung step's last ticks (52 of 61
  hat hits at swing 64).
- Rolls fire at step starts when `seq_stepIndex[track] % rate == 0`: the track's own position, so
  on a track that isn't 16 or 32 steps long the roll spacing breaks at the loop.

## Decisions

Swing

1. Note positions are the stock swung positions: the 2nd and 4th 16th of each beat are delayed by
   shuffle/127 x 16 ticks.
2. Global config option `swg` ("Swing mode") next to `lim`, appended to `glo.cfg` after the limiter
   byte: `Not` = note swing (default), `Clk` = stock clock swing. Older settings files (stock, 1.4.x)
   get `Not` too (decided after the review: the straight clock is the point of this release). Because the clock outputs change by default, this is a breaking change: version 2.0.0. The
   stock firmware reads only its own part of `glo.cfg`, as with `lim`.
3. No per-track swing in this round, but all swing goes through one function,
   `swingOffsetTicks(track, step)`, so per-track swing later only changes that function, the
   storage (pattern block version 3) and the UI. No file format change now.
4. Leading and following an external clock (MIDI and analog) use the same note swing; the
   postponing timer stays for `Clk` mode only. In `Not` mode steps start on the external clock and
   their hits are scheduled in time, so the next external step can't cut them off.
5. Exact time: hits are scheduled in system time (0.25 ms) and sound in the first audio block at or
   after it, like the stock swing. The scheduled times of all 128 swing values are distinct; what
   sounds is quantised to blocks (one step of swing is 0.5-0.9 ms, a block 1.45 ms), as in stock.
6. A swung step moves as a whole: its flam/arp hits and micro-shift move together, flam spacing as
   without swing (in stock it is squeezed: an intended change). Swing belongs to the step a note
   comes from, not where it lands.
7. Overlaps, per track (tracks 6 and 7, closed and open hat, are separate tracks here; their choke
   stays in the sound engine): the first hit of a newer step cancels the pending hits of older
   steps. All hits of one step fire, also if two land in the same block. The shared choke flags of
   `seq_processFlam()` (review S-B4) go away in `Not` mode.
8. Live recording snaps to the nearest step on the swung grid: the threshold is the midpoint between
   the current step's swung start and the next step's swung start.
8b. Found by the harness: stock recording put hits played slightly early (within the last tick of a
    step) one step too early. Fixed in both modes: the nearest step is measured in time.

Rolls

9. Speeds, sorted, with their interval in 96 ppq ticks: One (one shot), 1/1 = 384, 1/2 = 192,
   1/4 = 96, 1/8 = 48, 8T = 32, 16 = 24, 16T = 16, 32 = 12, 32T = 8, 64 = 6. The start-up default
   (selected at power-on in `menu.c`, with the sequencer's matching value) stays "16". Nothing stores
   or transmits the roll speed as a number.
10. One to 16 (every hit on a 16th) swing like notes; 8T, 16T, 32, 32T and 64 ignore swing
    completely: they stay on the straight grid, evenly spaced in time.
11. Rolls are locked to the global grid (decided after the review): a roll hit is due when the
    master tick count is a multiple of the interval, counted from the start of the sequencer, not
    from the track's position. The count wraps every 256 steps = 6144 ticks, and every interval
    above divides 6144. On 16- and 32-step tracks nothing changes for the existing speeds.
12. Triplets: rolls only in this round. Exact triplet notes (finer micro-shift or a triplet track
    mode) get their own plan; flam x3 already gives exact 32nd triplets.

Deferred, on purpose

- Continue restarting at step 1 and no Song Position Pointer (review S-B8): they matter when the LXR
  follows and is restarted mid-song, not for the clock it sends.
- Sub-block clock timing (see Goal).

## Scheduler spec (`Not` mode)

`Clk` mode keeps the stock code path unchanged (`seq_processFlam()` with its prev/current/next
patterns and choke flags), so its timelines stay byte-identical by construction. `Not` mode uses
the scheduler:

- Clock: the 96 ppq ticks run straight (no stretch), in both leading and following. MIDI clock and
  clock outputs come from these ticks as before.
- Grid time: the exact system time of a step's tick 0 is `systick at the tick - rest` (the leftover
  time `seq_tick()` already computes), not the block it was handled in.
- Queue: per track a small array of pending hits sorted by time, each `{fireTime (system ticks,
  float), step id (master step count of the source step), vol, note}`. Capacity 16 per track (a
  step has at most 8 flam/arp hits; the cut rule keeps at most 2 steps pending). Full: the oldest
  step's hits are dropped first.
- Enqueue: at tick 0 of step n, the hits of step n+1 of every track are enqueued (one step of look
  ahead, because a negative micro-shift of up to 21 ticks plays before the step's own tick 0). Each
  hit's time = grid time of step n+1 + swing delay (if step n+1 is a 2nd or 4th 16th) + micro-shift
  x 3 ticks + its flam/arp offset. Track wrap and pattern changes: step n+1 is read from the
  pattern and position that will be active then. Probability is decided at enqueue (the dice of the
  step, as now).
- Fire: every `seq_tick()` (once per block) pops all hits with `fireTime <= now`, in time order, and
  triggers them (`seq_triggerVoice()`, which also sends the MIDI note). Firing a hit of step s
  cancels that track's pending hits of steps before s.
- Rolls: due roll hits (decision 11) are enqueued at their grid time, the 16th-and-slower ones plus
  the swing delay of their step. A held roll button is checked at every tick.
- Stop, pattern reload, position reset: all queues are cleared.
- Cost: per block, 7 track queues are checked at their first entry; nothing else unless a hit is due.

## Testing

Sequencer timing harness (`test/sequencer`, done): the real `sequencer.c`/`clockSync.c` with the
firmware's time model, golden timelines from 1.4.1.

- `Clk` mode: byte-identical to the golden timelines (except the deliberate recording fix, 8b).
- `Not` mode: every note event within one block of its time in `Clk` mode; flam/arp spacing on swung
  steps equal to the unswung spacing (within one block); MIDI clock and clock output intervals
  within one block of the nominal interval at every swing, leading and following; no hits dropped
  when following.
- Rolls: every speed on its global grid, 16th-and-slower swung, faster straight, odd track lengths.
- Recording: hits 3 ms early and late, and at the midpoint +-2 ms, on straight and swung steps; at a
  track wrap, a pattern change and with micro-shifted steps.
- External clock: jittered MIDI clock, a tempo ramp, stop and start in the middle of a bar, analog
  clock.

## Delivery

Commits on `lionel/note-swing` (done: plan, image safety check, harness, recording fix):

1. Scheduler and note swing behind `swg` (`Clk` path untouched): queue, enqueue/fire, cut rule,
   following an external clock, recording on the swung grid, the `swg` setting; `Not` mode tests.
2. Rolls: sorted speeds, global grid, swing rule, start-up default; roll tests.
3. Docs: README (features), KNOWN_ISSUES.md (S-B6 now fixed in `Not` mode), config menu and roll
   table, a changelog entry for 2.0.0.
4. Hardware check, merge into `lionel/v1.4-fixes`, version 2.0.0.

## Safety: the bootloader

Nothing here touches the bootloader (0x08000000-0x08007FFF, write protected). A build check fails
the build (verified: exit 1, no image) if any firmware section lies below 0x08008000, if the HAL or
flash interface erase/program/option-byte functions are linked in, or if `UPDATE_BOOTLOADER` is
not 0. It is not exhaustive: code writing the flash controller's registers directly would not be
caught.

## Hardware check before merging

- MIDI clock out into a DAW or other gear at heavy swing: steady tempo; leading and following.
- A swung pattern with flams: the flam spacing is intact.
- Every roll speed, also on an odd-length track; recording a swung part.
- Odd track lengths and a pattern change with swing; `swg` saved and loaded with the settings.
- `swg` = `Clk` still feels like before.
- CPU meter build: the load with dense flams and 64th rolls at a high tempo.
