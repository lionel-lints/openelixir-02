# Analog engine port test (host)

`test/analog/run.sh` renders all 38 instruments of the Analog engine in 7 cases each (pitches from
33 Hz to 2 kHz, short to long decays, soft and hard hits, saturator off to full, retriggers while
ringing, the amp EG closing early) twice: with the original source of AnalogModels 0.5
(`original/`, github.com/G3nzalo/analogmodels-firmware commit 17b06e7, unmodified) and with the port
in `Core/Src/Soundengine/DSPAudio/AnalogModels.c`. The samples must be identical. `check.py` also
checks that every instrument sounds in every case and doesn't sit at full scale (the 808 claves
three octaves up ring at 20 kHz, above the models' limit, and are silent there as in AnalogModels).

`check_presets.py` compares the setup table of `Core/Src/menu/analogSetup.c` (each instrument's
decay and open decay, and the setup constants) with AnalogModels' `original/modelPresets.c`.

`original/Soundengine/DSPAudio/Oscillator.h` stands in for AnalogModels' `OscInfo`: only the fields
its instruments use, so the original builds without the rest of its tree.

Linux only, like the other host tests (on a Mac: in Docker). Needs gcc and Python 3.
