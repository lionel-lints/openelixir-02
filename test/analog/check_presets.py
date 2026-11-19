#!/usr/bin/env python3
"""test/analog: the Analog engine's setup (Core/Src/menu/analogSetup.c) must use AnalogModels'
values (test/analog/original/modelPresets.c): the decay and open decay of every instrument, in
the same order, and the same setup constants."""
import re, sys
here, root = sys.argv[1], sys.argv[2]
def table(path, name):
    src = open(path, encoding='utf-8').read()
    body = src[src.index(name):]
    body = body[body.index('{') + 1:body.index('};')]
    return [tuple(int(v) for v in m) for m in re.findall(r'\{\s*(\d+),\s*(\d+)\s*\}', body)]
def consts(path):
    src = open(path, encoding='utf-8').read()
    return {k: int(v) for k, v in re.findall(r'#define\s+(TUNING_ORIGINAL|FINE_CENTER|SLOPE_GATE|FILTER_OFF)\s+(\d+)', src)}
orig = table(here + '/original/modelPresets.c', 'modelPresets[NUM_ANALOG_MODELS]')
port = table(root + '/Core/Src/menu/analogSetup.c', 'analogPresets[NUM_ANALOG_MODELS]')
co, cp = consts(here + '/original/modelPresets.c'), consts(root + '/Core/Src/menu/analogSetup.c')
ok = len(orig) == 38 and orig == port and co == cp and len(co) == 4
print("setup: %d instruments, decays %s, constants %s" % (len(port), "same" if orig == port else "DIFFERENT", co if co == cp else (co, cp)))
sys.exit(0 if ok else 1)
