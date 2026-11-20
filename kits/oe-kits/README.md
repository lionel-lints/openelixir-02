# OE Kits

Twelve kits for the Modal and Analog engines, installed in project slot 9 ("OE Kits") of the SD card.

| Slot | Kit | Engines | Voices 1-6 (voice 6: tracks 6 closed / 7 open) |
|---|---|---|---|
| 00 | 808 Kit | Analog | BD8, CP8, LT8, SD8, CB8, HH8 |
| 01 | 909 Kit | Analog | BD9, CP9, LT9, SD9, CR9, HH9 |
| 02 | TR Mix | Analog | BD8, RS9, MT9, SD9, RD9, HH8 |
| 03 | 808 Perc | Analog | LC8, MC8, HC8, RS8, CL8, MA8 |
| 04 | 909 Toms | Analog | BD9, LT9, MT9, HT9, CR9, HH9 |
| 05 | Gamelan | Analog | GNG, BNG, SRN, GND, SRN (+7), GND (+12) |
| 06 | Djembe | Analog | DJB, DJT, DJS, CGS, CGO, MA8 |
| 07 | Taiko | Analog | ODK, NGD, SHM, TKR, GNG, TKR (+7) |
| 08 | Bells | Modal | bell structure, C3 G3 C4 E4 G4 C5 |
| 09 | Woodbar | Modal | bar structure (marimba-like), C3 G3 C4 E4 G4 C5 |
| 10 | Skins | Modal | membranes (kick, toms), noise-excited snare, metal, noise hat |
| 11 | Hybrid | Analog + Modal | BD9, modal tom, modal bell, SD8, modal metal, HH9 |

Built with the firmware's own code (`test/engine` `render --makekit`): each voice's engine set as
SHIFT + KIT does, Analog instruments chosen as turning `ins` does (with their setup), no LFO or
velocity modulation. Levels balanced with `render --kitfile` measurements: each voice's peak at about
-5 dBFS (voice 6 -6) by its volume alone. The Modal voices are already at full volume and stay 3-12 dB
quieter (the engine's level).

```sh
python3 design.py <repo> <dir>          # the specs (00.spec ...)
render --makekit <dir>/00.spec 00.snd    # build one (Linux, test/engine/build.sh)
render --kitfile *.snd > levels.csv      # measure
python3 balance.py <repo> <dir> -5 *.snd # set the volumes from levels.csv
```
