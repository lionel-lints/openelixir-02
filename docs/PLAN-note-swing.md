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
   byte. It only decides how the clock outputs (MIDI clock and the clock jacks) are paced:
   `Not` = straight (default), `Clk` = swung like the notes. Notes, rolls and recording are the same
   in both modes (one timing path, decided after the review: two paths would double every feature
   and make switching mid-play fragile). Older settings files (stock, 1.4.x)
   get `Not` too (decided after the review: the straight clock is the point of this release). The
   stock firmware reads exactly its own number of settings bytes and ignores the rest (checked in
   its `preset_readGlobalData()`), so the appended byte is safe for it. Because the clock outputs change by default, this is a breaking change: version 2.0.0. The
   stock firmware reads only its own part of `glo.cfg`, as with `lim`.
3. No per-track swing in this round, but all swing goes through one function,
   `swingOffsetTicks(track, step)`, so per-track swing later only changes that function, the
   storage (pattern block version 3) and the UI. No file format change now.
4. Leading and following an external clock (MIDI and analog) use the same note swing. Steps start
   on the straight clock (internal or external), the postponing timer goes, and hits are scheduled
   in time, so the next external step can't cut them off.
4b. `Clk` clock outputs: the clock pulses of a swung step start late by the swing delay and are
   spread evenly over the rest of that step, so the next step's first pulse is on time. (Stock
   delayed the first pulse and then fired the rest back to back.) Switching `swg` while playing only
   changes the pulse times of the next step.
5. Exact time: hits are scheduled in system time (0.25 ms) and sound in the first audio block at or
   after it, like the stock swing. The scheduled times of all 128 swing values are distinct; what
   sounds is quantised to blocks (one step of swing is 0.5-0.9 ms, a block 1.45 ms), as in stock.
6. A swung step moves as a whole: its flam/arp hits and micro-shift move together, flam spacing as
   without swing (in stock it is squeezed: an intended change). Swing belongs to the step a note
   comes from, not where it lands.
7. Overlaps, per track (tracks 6 and 7, closed and open hat, are separate tracks here; their choke
   stays in the sound engine): when a newer step's first hit fires, the pending flam/arp repeats
   of older steps are cancelled; an older step's first hit (its note, wherever micro-shift puts it)
   always plays, also after the newer step's first hit (decided after the second review: otherwise
   late micro-shifted notes would vanish; stock plays them). All hits of one step fire, also if two
   land in the same block. The shared choke flags of
   `seq_processFlam()` (review S-B4) go away.
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
    completely: they stay on the straight grid, evenly spaced in time. One switch in the code, so the
    hardware listening test (swing 64, 32 and 64 rolls) can change it to "start with the step's
    swing, even inside" without rework.
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

## Scheduler spec (both modes)

One timing path: the scheduler replaces the stock note playback (`seq_processFlam()` with its
prev/current/next patterns and choke flags) and the clock stretch.

- Clock: the 96 ppq ticks run straight (no stretch), in both leading and following.
- Clock outputs: the MIDI clock (every 4th tick) and the clock jack ticks are sent at their tick in
  `Not` mode. In `Clk` mode the pulses of a swung step are scheduled at grid time + swing delay +
  k x (step - swing delay) / 24 for its tick k (decision 4b), through the same fire loop.
- Units: system ticks (0.25 ms) everywhere in the scheduler, as a 32-bit integer plus an 8-bit
  fraction (a float would lose the 0.25 ms resolution after about 70 minutes of running); the
  conversions from tempo (ms per 96 ppq tick) are in one function.
- Grid time: the exact system time of a step's tick 0 is `systick at the tick - rest` (the leftover
  time `seq_tick()` already computes), not the block it was handled in.
- Queue: per track a small array of pending hits sorted by time, each `{fireTime, step id (master
  step count of the source step), first-hit flag, vol, note}`. Capacity 16 per track (a step has at
  most 8 flam/arp hits). Full: the oldest step's repeats are dropped first; drops are counted, and
  the tests require 0.
- Enqueue: at tick 0 of step n, the hits of step n+1 of every track are enqueued (one step of look
  ahead, because a negative micro-shift of up to 21 ticks plays before the step's own tick 0). Each
  hit's time = grid time of step n+1 + swing delay (if step n+1 is a 2nd or 4th 16th) + micro-shift
  x 3 ticks + its flam/arp offset. Step n+1 is each track's own next position (its own length and
  wrap) in the pattern that will be active then (a pending pattern change at the end of the
  pattern). Probability is decided at enqueue, with that step's dice (rolled one step earlier than
  now). At start and after a reset the first step is enqueued before it plays (priming).
- Fire: every `seq_tick()` (once per block) pops all hits with `fireTime <= now`, in time order, and
  triggers them (`seq_triggerVoice()`, which also sends the MIDI note). Firing the first hit of
  step s cancels that track's pending repeats (not first hits) of steps before s.
- Rolls: due roll hits (decision 11) are enqueued at their grid time, the 16th-and-slower ones plus
  the swing delay of their step. A held roll button is checked at every tick.
- Stop, pattern reload, position reset: all queues are cleared, and the recording reference (the
  time the current step started) is reset.
- Cost: per block, 7 track queues are checked at their first entry; nothing else unless a hit is due.

## Testing

Sequencer timing harness (`test/sequencer`, done): the real `sequencer.c`/`clockSync.c` with the
firmware's time model, golden timelines from 1.4.1.

- Notes, both modes: every note event within one block of its time in the 1.4.1 golden timeline,
  except the intended changes (flam/arp spacing on swung steps equal to the unswung spacing, within
  one block; no hits dropped when following). Note events identical in `Not` and `Clk` mode.
- Clock outputs: `Not`: MIDI clock and clock jack intervals within one block of the nominal interval
  at every swing, leading and following. `Clk`: the pulses of swung steps at their spread times
  (4b), within one block; the first pulse of every step on time.
- Scenarios without swing: every event within one block of the golden timeline (target: identical;
  every difference is listed and explained in the commit).
- Rolls: every speed on its global grid, 16th-and-slower swung, faster straight, odd track lengths.
- Recording: hits 3 ms early and late, and at the midpoint +-2 ms, on straight and swung steps; at a
  track wrap, a pattern change, with micro-shifted steps and right after start.
- Overlaps: a late micro-shifted note after the next step's first hit still plays; flam repeats of
  an older step stop at the newer step's first hit; no queue overflow.
- External clock: jittered MIDI clock, a tempo ramp, stop and start in the middle of a bar, analog
  clock.

## Delivery

Commits on `lionel/note-swing` (done: plan, image safety check, harness, recording fix, scheduler
and note swing f4e01fe, rolls bccc50b, docs; also found on the way: Euclid 02f39c0, oscillator
ae2784c):

1. Scheduler and note swing: queue, enqueue/fire, cut rule, following an external clock, recording
   on the swung grid, clock outputs per `swg`, the `swg` setting; tests for both modes.
2. Rolls: sorted speeds, global grid, swing rule, start-up default; roll tests.
3. Docs: README (features), KNOWN_ISSUES.md (S-B6 now fixed in `Not` mode), config menu and roll
   table, a changelog entry for 2.0.0 that says first: the clock outputs are now straight by
   default, `swg` = `Clk` swings them again.
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
- `swg` = `Clk`: notes feel as before, gear synced to the LXR swings along (evenly).
- Swing 64 with 32 and 64 rolls: do straight fast rolls sit right against the swung notes?
- An SD card with stock settings: `swg` defaults to `Not`, the stock firmware still loads it.
- CPU meter build: the load with dense flams and 64th rolls at a high tempo.
