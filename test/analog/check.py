#!/usr/bin/env python3
"""test/analog: every instrument sounds in every case and doesn't hit full scale for long."""
import struct, sys
NAMES = "BD8 BD9 SD8 SD9 LT8 MT8 HT8 LT9 MT9 HT9 LC8 MC8 HC8 RS8 RS9 CL8 CP8 CP9 MA8 CB8 HH8 HH9 CY8 CR9 RD9 SRN GND BNG GNG DJB DJT DJS CGO CGS ODK NGD SHM TKR".split()
CASES, BLOCKS, N = 7, 300, 64
data = open(sys.argv[1], 'rb').read()
s = struct.unpack('<%dh' % (len(data) // 2), data)
per = BLOCKS * N
assert len(s) == len(NAMES) * CASES * per, len(s)
# CL8 (one resonator at 2.5 kHz) three octaves up rings at 20 kHz, above the models' limit
# of 0.45 x the sample rate: silent, as in AnalogModels
SILENT = {("CL8", 5)}
bad = 0
for i, name in enumerate(NAMES):
	peaks = []
	for k in range(CASES):
		x = s[(i * CASES + k) * per:(i * CASES + k + 1) * per]
		peak = max(abs(v) for v in x)
		clipped = sum(1 for v in x if abs(v) >= 32767)
		peaks.append(peak)
		if (peak < 300 and (name, k) not in SILENT) or clipped > 64:
			print("FAIL %s case %d: peak %d, %d samples at full scale" % (name, k, peak, clipped)); bad += 1
	print("%s peaks %s" % (name, " ".join("%5d" % p for p in peaks)))
sys.exit(1 if bad else 0)
