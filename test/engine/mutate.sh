#!/bin/bash
# Mutation check of the harness: for each engine, apply one tiny change to a
# fresh copy of the working tree and verify that the rendered output differs.
# Usage: mutate.sh <repo> <reference render.raw> <work dir (must not exist)>
set -e
REPO=$1; REF=$2; WORK=$3; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir "$WORK"
# the voices of the Modal and Analog engines are not in the kit renders: their own renders' hashes
voiceHashes() { (cd "$1" && for e in modal analog; do ./render --$e 2>/dev/null | sed -n "s/^$e voice renders: .*hash //p"; done); }
bash "$HERE/build.sh" "$REPO" "$WORK/ref" >/dev/null
REF_VOICES=$(voiceHashes "$WORK/ref")
n=0
while IFS='|' read -r file from to; do
	[ -n "$file" ] || continue
	n=$((n+1)); tree="$WORK/tree$n"; mkdir "$tree"
	(cd "$REPO" && git ls-files -co --exclude-standard -z | xargs -0 tar -c) | tar -x -C "$tree"
	python3 -I -c "
import sys
p, a, b = sys.argv[1:4]
s = open(p, newline='').read()
if a not in s: sys.exit('pattern missing: ' + a)
open(p, 'w', newline='').write(s.replace(a, b, 1))" "$tree/$file" "$from" "$to"
	bash "$HERE/build.sh" "$tree" "$WORK/out$n" >/dev/null
	(cd "$WORK/out$n" && ./render m.raw >/dev/null)
	if ! cmp -s "$WORK/out$n/m.raw" "$REF"; then echo "detected:     $file"
	elif [ -n "$REF_VOICES" ] && [ "$(voiceHashes "$WORK/out$n")" != "$REF_VOICES" ]; then echo "detected:     $file (modal/analog voice)"
	else echo "NOT DETECTED: $file: $from"; fi
done <<'LIST'
Core/Src/Soundengine/DSPAudio/DrumVoice.c|v->velo = vol/127.f;|v->velo = vol/126.f;
Core/Src/Soundengine/DSPAudio/Snare.c|v->velo = vel/127.f;|v->velo = vel/126.f;
Core/Src/Soundengine/DSPAudio/CymbalVoice.c|v->velo = vel/127.f;|v->velo = vel/126.f;
Core/Src/Soundengine/DSPAudio/HiHat.c|v->velo = vel/127.f;|v->velo = vel/126.f;
Core/Src/Soundengine/DSPAudio/ModalVoice.c|v->velo = vel/127.f;|v->velo = vel/126.f;
Core/Src/Soundengine/DSPAudio/AnalogVoice.c|v->velo = vel/127.f;|v->velo = vel/126.f;
Core/Src/Soundengine/DSPAudio/AnalogModels.c|const float fBase = 52.f * c->tf;|const float fBase = 52.01f * c->tf;
Core/Src/menu/analogSetup.c|{ 65,  0 },	// BD8|{ 66,  0 },	// BD8
LIST
