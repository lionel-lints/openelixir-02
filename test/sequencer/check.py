#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check sequencer timelines against the timing rules of docs/PLAN-note-swing.md.

    check.py <timeline dir> <stock reference dir>

Independent of the firmware code: from the scenario written at the top of each timeline (tempo,
swing, clock, active steps with flam and micro-shift, track lengths, events) it computes when
every hit and clock pulse has to sound and compares with the timeline, within one audio block.

  A  notes: grid + swing (2nd/4th 16th) + micro-shift + flam/arp position; a newer step's first
     hit cuts older steps' pending repeats; hits of one track due in the same block: the newest
     step's sound; following an external clock, steps start on the clock
  B  clock outputs, Not mode: like the same scenario without swing, and on the straight tick grid
  C  clock outputs, Clk mode: the pulses of a swung step later by swing x (1 - tick/24);
     notes identical to Not mode
  D  no queue overflow
  E  scenarios without swing on the internal clock: identical to the stock timeline (reported)
"""
import math
import os
import sys

FLAM_SUBSTEPS = [1, 2, 3, 4, 6, 8]
ROLL_STEPS = {1: 16, 2: 8, 3: 4, 4: 2, 5: 1}       # menu value -> steps between roll hits
WINDOW = 8                                          # MIDI clock tempo estimate (clockSync.c)


class Timeline:
    def __init__(self, path):
        self.name = os.path.basename(path)[:-4]
        self.steps = {}            # (pattern, track) -> {step: (flam, shift)}
        self.length = {}           # (pattern, track) -> length
        self.events = []           # (block, systick, name, args)
        self.flags = set()
        self.roll = None
        self.dropped = None
        for line in open(path):
            p = line.split()
            if p[0] == "#":
                kind = p[1]
                if kind == "scenario":
                    kv = dict(zip(p[3::2], p[4::2]))
                    self.bpm = int(kv["bpm"]); self.swing = int(kv["swing"]); self.clock = kv["clock"]
                    self.clockswing = int(kv["clockswing"]); self.nsteps = int(kv["steps"])
                    self.block_s = float(kv["block"]); self.systick_s = float(kv["systick"])
                elif kind == "length":
                    self.length[(int(p[2]), int(p[3]))] = int(p[4])
                elif kind == "step":
                    self.steps.setdefault((int(p[2]), int(p[3])), {})[int(p[4])] = (int(p[5]), int(p[6]))
                elif kind == "patternchange":
                    self.flags.add(kind)
                    self.change_shuffle = int(p[7])     # the swing stored in the new pattern
                elif kind == "roll":
                    self.roll = dict(rate=int(p[5]), on=(int(p[7]), int(p[8])), off=(int(p[10]), int(p[11])))
                else:
                    self.flags.add(kind)
                continue
            ev = (int(p[0]), int(p[1]), p[2], tuple(int(x) for x in p[3:]))
            if ev[2] == "DROPPED":
                self.dropped = ev[3][0]
            else:
                self.events.append(ev)

    #--- time model (test/sequencer/main.c)
    def systick(self, block):
        return int(block * self.block_s / self.systick_s)

    def block_due(self, t, quantum=16):
        """first block whose system tick reaches time t (system ticks); hits are rounded up to 1/16"""
        target = math.ceil(t * quantum - 1e-9) / quantum
        b = max(0, int(target * self.systick_s / self.block_s) - 2)
        while self.systick(b) < target - 1e-12:
            b += 1
        return b

    def block_reaching(self, seconds):
        """first block whose start time reaches `seconds` (external clock delivery, events)"""
        b = max(0, int(seconds / self.block_s) - 2)
        while b * self.block_s < seconds:
            b += 1
        return b

    def run_blocks(self):
        step_s = 60.0 / self.bpm / 4.0
        return int((self.nsteps + 2) * step_s / self.block_s)

    def of(self, name):
        return [e for e in self.events if e[2] == name]


#------------------------------------------------------------------ expected notes (A)
def step_starts(tl):
    """grid time (system ticks) and tick length of every step, per the clock source"""
    tick_nom = 2500.0 / tl.bpm
    step_s = 60.0 / tl.bpm / 4.0
    starts = []
    if tl.clock == "internal":
        for n in range(tl.nsteps + 2):
            starts.append((n * 24 * tick_nom, tick_nom))
        return starts
    pulses = [e[0] for e in tl.events if e[2] == "EXT"]     # the blocks the pulses arrived in
    if tl.clock == "analog":
        for b in pulses:
            starts.append((float(tl.systick(b + 1)), tick_nom))   # handled in the next block's seq_tick
        return starts
    # MIDI clock: steps on every 6th pulse; the tempo is estimated from the last 8 pulse intervals
    tl.tempo_updates = []          # (block, bpm): the estimate after each pulse
    window = [0.0] * WINDOW
    wpos = 0
    last = 0.0
    bpm = tl.bpm
    for k, b in enumerate(pulses):
        if k % 6 == 0:
            starts.append((float(tl.systick(b)), 2500.0 / bpm))
        t = tl.systick(b)
        window[wpos] = t - last
        wpos = (wpos + 1) % WINDOW
        last = t
        avg = sum(window) / WINDOW
        est = 60000.0 / (24.0 * (avg / 4.0)) if avg > 0 else 0
        bpm = int(est) if est > 0 else 1
        bpm = bpm if bpm > 0 else 1
        tl.tempo_updates.append((b, bpm))
    return starts


def tick_length_at(tl, block, default):
    """the tempo the firmware uses for a tick handled in this block (before that block's MIDI)"""
    updates = getattr(tl, "tempo_updates", None)
    if not updates:
        return default
    bpm = tl.bpm
    for b, v in updates:
        if b >= block:
            break
        bpm = v
    return 2500.0 / bpm


def schedule(tl):
    """the steps: (id, n = steps since the sequencer started, grid time, tick length, drop_from):
    hits not sounded by block drop_from are dropped (the sequencer was stopped)"""
    starts = step_starts(tl)
    if "stopstart" not in tl.flags:
        return [(n, n, g, t, None) for n, (g, t) in enumerate(starts)]
    stop = next(e[0] for e in tl.events if e[2] == "STOP")
    start = next(e[0] for e in tl.events if e[2] == "START")
    tick = starts[0][1]
    steps = [(n, n, g, t, stop) for n, (g, t) in enumerate(starts) if tl.block_due(g) < stop]
    # after the start the clock continues from where it was: its exact phase from the ticks
    ticks = [e[0] for e in tl.events if e[2] == "TICK" and e[0] >= start]
    lo = max(tl.systick(b - 1) - m * tick for m, b in enumerate(ticks))
    hi = min(tl.systick(b) - m * tick for m, b in enumerate(ticks))
    base = (lo + hi) / 2.0
    n = 0
    while base + n * 24 * tick < tl.run_blocks() * tl.block_s / tl.systick_s:
        steps.append((1000 + n, n, base + n * 24 * tick, tick, None))
        n += 1
    return steps


def expected_hits(tl):
    """per track: list of (time, step, first, roll, drop_from) the rules say should be queued"""
    starts = step_starts(tl)
    change_at = 16 if "patternchange" in tl.flags else None
    hits = {t: [] for t in range(7)}
    prev = None
    for sid, n, grid, tick, drop in schedule(tl):
        # a step that plays early (negative micro-shift) is queued at the step before: its start
        # is predicted from that step's start and tempo (an external clock may come a bit off)
        early_grid, early_tick = grid, tick
        if prev is not None and prev[0] == sid - 1:
            early_grid, early_tick = prev[1] + 24 * prev[2], prev[2]
        prev = (sid, grid, tick)
        pattern = 1 if change_at is not None and n >= change_at else 0
        base = change_at if change_at is not None and n >= change_at else 0
        swing = tl.change_shuffle if pattern == 1 else tl.swing     # a pattern change loads its swing
        swung = (n + 1) % 2 == 0
        for track in range(7):
            length = tl.length.get((pattern, track), 16)
            pos = (n - base) % length
            st = tl.steps.get((pattern, track), {}).get(pos)
            if st is None:
                continue
            flam, shift = st
            g, t = (early_grid, early_tick) if shift < 0 else (grid, tick)
            delay = swing / 127.0 * 16.0 * t if swung else 0.0
            num = FLAM_SUBSTEPS[flam]
            for k in range(num):
                hits[track].append((g + delay + shift * 3 * t + k * (24 // num) * t, sid, k == 0, False, drop))
    if tl.roll:
        rate = tl.roll["rate"]
        tick_s = 60.0 / tl.bpm / 4.0 / 24.0
        if rate == 0:       # one shot at the press
            b = tl.block_reaching((tl.roll["on"][0] * 24 + tl.roll["on"][1]) * tick_s)
            hits[1].append((float(tl.systick(b)), -1, True, True, None))
        else:
            for n in range(tl.roll["on"][0] + 1, tl.roll["off"][0] + 1):
                if (n % 16) % ROLL_STEPS[rate] == 0:
                    grid, tick = starts[n]
                    swung = (n + 1) % 2 == 0
                    delay = tl.swing / 127.0 * 16.0 * tick if swung else 0.0
                    hits[1].append((grid + delay, n, True, True, None))
    return hits


def play(tl, queued):
    """apply the fire rules block by block: returns per track the blocks hits sound in.
    Each fire pass sounds the newest step among the due hits; with MIDI clock a step arrives
    after seq_tick() in its block, so its own hits due then are a second pass in that block."""
    out = {}
    end = tl.run_blocks()
    arrival = {}
    if tl.clock == "midi":
        arrival = {sid: tl.block_due(g) for sid, n, g, t, d in schedule(tl)}
    for track, hits in queued.items():
        pending = sorted(((tl.block_due(t), n, first, roll) for (t, n, first, roll, drop) in hits
                          if drop is None or tl.block_due(t) < drop))
        fired = []
        while pending:
            b = pending[0][0]
            if b >= end:
                break
            late = lambda h: arrival.get(h[1]) == b         # hits of the step arriving in block b
            for second in (False, True):
                due = [h for h in pending if h[0] <= b and late(h) == second]
                if not due:
                    continue
                newest = max(h[1] for h in due)
                sounding = [h for h in due if h[1] == newest]
                fired += [b] * len(sounding)
                cut = any(h[2] and not h[3] for h in sounding)
                pending = [h for h in pending if not (h[0] <= b and late(h) == second)
                           and not (cut and not h[2] and h[1] < newest)]
        out[track] = fired
    return out


def check_notes(tl, errors):
    expected = play(tl, expected_hits(tl))
    for track in range(7):
        got = sorted(e[0] for e in tl.of("TRIG") if e[3][0] == track)
        exp = expected.get(track, [])
        if len(got) != len(exp):
            errors.append(f"A track {track}: {len(got)} hits, expected {len(exp)}")
            continue
        worst = max((abs(g - e) for g, e in zip(got, exp)), default=0)
        if worst > 1:
            bad = [(e, g) for g, e in zip(got, exp) if abs(g - e) > 1][:3]
            errors.append(f"A track {track}: hit {worst} blocks off (expected, got: {bad})")


#------------------------------------------------------------------ clock outputs (B, C)
def clock_events(tl):
    return [(e[0], e[2], e[3]) for e in tl.events if e[2] in ("TICK", "CLK")]


def check_clock_straight(tl, errors):
    if tl.clock != "internal" or "stopstart" in tl.flags:
        return
    tick = 2500.0 / tl.bpm
    ticks = tl.of("TICK")
    for i, e in enumerate(ticks):
        want = tl.block_due(i * tick, quantum=1e6)
        if abs(e[0] - want) > 1:
            errors.append(f"B clock tick {i} in block {e[0]}, straight grid {want}")
            return
    clks = tl.of("CLK")
    for i, e in enumerate(clks):
        want = tl.block_due(i * 4 * tick, quantum=1e6)
        if abs(e[0] - want) > 1:
            errors.append(f"B MIDI clock {i} in block {e[0]}, straight grid {want}")
            return


def check_clock_swung(tl, straight, errors):
    """Clk mode against the Not-mode timeline of the same scenario"""
    end = tl.run_blocks()
    starts = step_starts(tl)
    a = [e for e in straight.events if e[2] == "TICK"]
    b = [e for e in tl.events if e[2] == "TICK"]
    want = []
    # a tick belongs to the last step that started at or before its block (following an external
    # clock, the internal clock can tick once before the first step: that tick has no step)
    start_blocks = [tl.block_due(g) for g, _ in starts]
    for x in a:
        cnt = x[3][1]
        step = max([n for n, sb in enumerate(start_blocks) if sb <= x[0]], default=-1)
        delay = 0.0
        if step >= 0 and (step + 1) % 2 == 0:
            d = tl.swing / 127.0 * 16.0 * tick_length_at(tl, x[0], starts[step][1])
            delay = d - cnt * d / 24.0
        # the Not-mode tick sounded in block x: its exact time lies after the previous block's
        # system tick and at most at this one's
        lo = tl.block_due(tl.systick(x[0] - 1) + delay + 1e-6) if delay else x[0]
        hi = tl.block_due(tl.systick(x[0]) + delay + 1e-6) if delay else x[0]
        if hi < end - 1:         # (pulses delayed past the end of the run aren't in the timeline)
            want.append(((lo, hi), step, cnt))
    if not len(want) - 2 <= len(b) <= len(want) + 2:     # (+-2: pulses at the end of the run)
        errors.append(f"C {len(b)} clock ticks, expected {len(want)}")
        return
    for ((lo, hi), st, cnt), y in zip(want, b):
        if not lo - 1 <= y[0] <= hi + 1:
            errors.append(f"C step {st} tick {cnt} in block {y[0]}, expected {lo}-{hi}")
            return
    # MIDI clock (internal clock only): pulse i is tick 4i
    a = [e for e in straight.events if e[2] == "CLK"]
    b = [e for e in tl.events if e[2] == "CLK"]
    if not (len(a) - 2 <= len(b) <= len(a)):
        errors.append(f"C {len(b)} MIDI clocks, Not mode has {len(a)}")
        return
    for i, (x, y) in enumerate(zip(a, b)):
        step, cnt = divmod(4 * i, 24)
        delay = 0.0
        if (step + 1) % 2 == 0 and step < len(starts):
            d = tl.swing / 127.0 * 16.0 * starts[step][1]
            delay = d - cnt * d / 24.0
        lo = tl.block_due(tl.systick(x[0] - 1) + delay + 1e-6) if delay else x[0]
        hi = tl.block_due(tl.systick(x[0]) + delay + 1e-6) if delay else x[0]
        if not lo - 1 <= y[0] <= hi + 1:
            errors.append(f"C MIDI clock {i} (step {step} tick {cnt}) in block {y[0]}, expected {lo}-{hi}")
            return
    if [e for e in tl.events if e[2] == "TRIG"] != [e for e in straight.events if e[2] == "TRIG"]:
        errors.append("C notes differ from Not mode")


#------------------------------------------------------------------ main
def main():
    tdir, sdir = sys.argv[1], sys.argv[2]
    tls = {f[:-4]: Timeline(os.path.join(tdir, f)) for f in sorted(os.listdir(tdir)) if f.endswith(".txt")}
    failed = 0
    identical, differing = [], []
    for name, tl in tls.items():
        errors = []
        if tl.dropped:
            errors.append(f"D {tl.dropped} hits dropped (queue full)")
        if "record" not in tl.flags:
            check_notes(tl, errors)
        if tl.clockswing:
            base = tls.get(name[:-4])
            if base is None:
                errors.append("C no Not-mode scenario to compare with")
            else:
                check_clock_swung(tl, base, errors)
        else:
            check_clock_straight(tl, errors)
            if tl.swing:
                sibling = name.replace(f"swing{tl.swing:03d}", "swing000")
                if sibling != name and sibling in tls and clock_events(tls[sibling]) != clock_events(tl):
                    errors.append(f"B clock outputs differ from {sibling} (swing must not move them)")
        if tl.swing == 0 and tl.clock == "internal":
            ref = os.path.join(sdir, name + ".txt")
            if os.path.exists(ref):
                strip = lambda path: [l for l in open(path) if not l.startswith("#") and " DROPPED " not in l]
                (identical if strip(ref) == strip(os.path.join(tdir, name + ".txt")) else differing).append(name)
        if errors:
            failed += 1
            print(f"FAIL {name}")
            for e in errors:
                print(f"     {e}")
    print(f"E identical to stock: {', '.join(identical) or '-'}")
    if differing:
        print(f"E differ from stock (within the checks above): {', '.join(differing)}")
    print(f"timing rules: {len(tls) - failed} of {len(tls)} scenarios pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
