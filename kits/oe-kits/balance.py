"""Scale each voice's volume (linear: value/127) so its peak lands on a target; capped at 127."""
import csv, re, sys, collections
ROOT, KITDIR, TARGET = sys.argv[1], sys.argv[2], float(sys.argv[3])
files = sys.argv[4:]
src = open(ROOT + '/Core/Inc/Parameters.h', encoding='latin-1').read()
src = re.sub(r'//[^\n]*', '', src); src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
body = src[src.index('{', src.index('enum')) + 1:]; body = body[:body.index('}')]
P = {}; val = -1
for item in [x.strip() for x in body.split(',') if x.strip()]:
    if '=' in item:
        n, v = [t.strip() for t in item.split('=', 1)]; val = P[v] if v in P else int(eval(v, {}, P))
    else:
        n = item; val += 1
    P[n] = val
rows = list(csv.DictReader(open(f"{KITDIR}/levels.csv")))
peak = collections.defaultdict(dict)
for r in rows: peak[r['file']][int(r['track'])] = float(r['peak_db'])
for f in files:
    b = bytearray(open(f"{KITDIR}/{f}", 'rb').read())
    changes = []
    for slot in range(6):
        tracks = (6, 7) if slot == 5 else (slot + 1,)
        p = max(peak[f][t] for t in tracks)
        target = TARGET - (1.0 if slot == 5 else 0.0)
        idx = 8 + P['PAR_VOL1'] + slot
        old = b[idx]
        if p < -100 or old == 0: continue
        new = min(127, max(1, round(old * 10 ** ((target - p) / 20))))
        b[idx] = new; changes.append(f"v{slot+1} {old}->{new}")
    open(f"{KITDIR}/{f}", 'wb').write(b)
    print(f, ", ".join(changes))
