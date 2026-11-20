"""The OE Kits: kit specs for render --makekit (parameter numbers from Parameters.h)."""
import re, sys
ROOT = sys.argv[1]; OUT = sys.argv[2]
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
ROLE = [  # slot_rolePar (VoiceSlot.c)
    ['PAR_MOD_WAVE_DRUM1', 'PAR_MOD_EG1', 'PAR_MODAMNT1', 'PAR_PITCH_SLOPE1', 'PAR_FMAMNT1', 'PAR_FM_FREQ1', 'PAR_MIX_MOD_1'],
    ['PAR_MOD_WAVE_DRUM2', 'PAR_MOD_EG2', 'PAR_MODAMNT2', 'PAR_PITCH_SLOPE2', 'PAR_FMAMNT2', 'PAR_FM_FREQ2', 'PAR_MIX_MOD_2'],
    ['PAR_MOD_WAVE_DRUM3', 'PAR_MOD_EG3', 'PAR_MODAMNT3', 'PAR_PITCH_SLOPE3', 'PAR_FMAMNT3', 'PAR_FM_FREQ3', 'PAR_MIX_MOD_3'],
    ['PAR_UNUSED_3', 'PAR_MOD_EG4', 'PAR_MODAMNT4', 'PAR_PITCH_SLOPE4', 'PAR_MIX1', 'PAR_NOISE_FREQ1', 'PAR_REPEAT4'],
    ['PAR_WAVE2_CYM', 'PAR_WAVE3_CYM', 'PAR_MOD_OSC_F2_CYM', 'PAR_MOD_OSC_GAIN2_CYM', 'PAR_MOD_OSC_GAIN1_CYM', 'PAR_MOD_OSC_F1_CYM', 'PAR_REPEAT5'],
    ['PAR_WAVE2_HH', 'PAR_WAVE3_HH', 'PAR_MOD_OSC_F2', 'PAR_MOD_OSC_GAIN2', 'PAR_MOD_OSC_GAIN1', 'PAR_MOD_OSC_F1', 'PAR_UNUSED_4'],
]
INS = "BD8 BD9 SD8 SD9 LT8 MT8 HT8 LT9 MT9 HT9 LC8 MC8 HC8 RS8 RS9 CL8 CP8 CP9 MA8 CB8 HH8 HH9 CY8 CR9 RD9 SRN GND BNG GNG DJB DJT DJS CGO CGS ODK NGD SHM TKR".split()
MODAL, ANALOG = 4, 5

def coarse(slot): return P['PAR_COARSE1'] + 2 * slot
def analog(*ins):
    out = []
    for slot, i in enumerate(ins):
        out += [f"engine {slot} {ANALOG}", f"analog {slot} {INS.index(i)}"]
    return out
def modal(slot, note, exc=0, damp=64, struct=0, bright=64, pos=40, hard=64, prt=2):
    r = ROLE[slot]
    return [f"engine {slot} {MODAL}", f"par {coarse(slot)} {note}",
            f"par {P['PAR_VELOA1'] + 2*slot} 0", f"par {P['PAR_VELOD1'] + 2*slot} 110", f"par {P['PAR_VOL_SLOPE1'] + slot} 127",
            *(f"par {P[r[k]]} {v}" for k, v in enumerate((exc, damp, struct, bright, pos, hard, prt)))]
def tune(slot, note): return [f"par {coarse(slot)} {note}"]

# structure: 0 string, ~42 bar, ~85 membrane, 127 bell (ModalResonator: blends between 4 materials)
KITS = [
    ("808 Kit",  analog("BD8", "CP8", "LT8", "SD8", "CB8", "HH8")),
    ("909 Kit",  analog("BD9", "CP9", "LT9", "SD9", "CR9", "HH9")),
    ("TR Mix",   analog("BD8", "RS9", "MT9", "SD9", "RD9", "HH8")),
    ("808 Perc", analog("LC8", "MC8", "HC8", "RS8", "CL8", "MA8")),
    ("909 Toms", analog("BD9", "LT9", "MT9", "HT9", "CR9", "HH9")),
    ("Gamelan",  analog("GNG", "BNG", "SRN", "GND", "SRN", "GND") + tune(4, 67) + tune(5, 72)),
    ("Djembe",   analog("DJB", "DJT", "DJS", "CGS", "CGO", "MA8")),
    ("Taiko",    analog("ODK", "NGD", "SHM", "TKR", "GNG", "TKR") + tune(5, 67)),
    ("Bells",    modal(0, 48, struct=127, damp=96, bright=80, hard=70) + modal(1, 55, struct=127, damp=96, bright=80, hard=70)
                 + modal(2, 60, struct=127, damp=90, bright=85) + modal(3, 64, struct=127, damp=90, bright=85)
                 + modal(4, 67, struct=127, damp=86, bright=90) + modal(5, 72, struct=127, damp=80, bright=90)),
    ("Woodbar",  modal(0, 48, struct=42, damp=50, bright=60, hard=80) + modal(1, 55, struct=42, damp=50, bright=64, hard=80)
                 + modal(2, 60, struct=42, damp=48, bright=70, hard=85) + modal(3, 64, struct=42, damp=46, bright=70, hard=85)
                 + modal(4, 67, struct=42, damp=44, bright=75, hard=90) + modal(5, 72, struct=42, damp=40, bright=80, hard=95)),
    ("Skins",    modal(0, 36, struct=85, damp=50, bright=40, hard=40, pos=20) + modal(1, 43, struct=85, damp=48, bright=50, hard=55)
                 + modal(2, 50, struct=85, damp=46, bright=55, hard=60) + modal(3, 57, exc=1, struct=85, damp=36, bright=90, hard=90)
                 + modal(4, 79, struct=110, damp=40, bright=100, hard=110, prt=3) + modal(5, 86, exc=1, struct=127, damp=24, bright=110, hard=120, prt=3)),
    ("Hybrid",   analog("BD9") + modal(1, 45, struct=85, damp=48, bright=55, hard=60)
                 + modal(2, 72, struct=127, damp=80, bright=85) + ["engine 3 5", f"analog 3 {INS.index('SD8')}"]
                 + modal(4, 84, exc=1, struct=110, damp=36, bright=100, hard=110, prt=3) + ["engine 5 5", f"analog 5 {INS.index('HH9')}"]),
]
for n, (name, lines) in enumerate(KITS):
    open(f"{OUT}/{n:02d}.spec", "w").write("\n".join([f"name {name}"] + lines) + "\n")
print(len(KITS), "specs written")
