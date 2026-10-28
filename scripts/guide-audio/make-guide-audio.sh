#!/bin/bash
# Regenerates docs/guides/audio/accessibility/*.ogg from docs/guides/ACCESSIBILITY.md, so the guide and its
# recordings never drift: each section with a "Listen to this section" link is read by Ember's Piper voice
# on the Steam Deck (no game needed, silent: Piper writes files), and chime.ogg / tick.ogg come from the
# repository's own sound definitions. Encodes Opus, mono, 24 kb/s with ember-build\tools\ffmpeg.
# Runs from WSL. Needs: a Steam Deck (or any Linux host) over ssh with Ember's Piper and the voice in
# ~/.cache/ember-speech (speech on once under Proton puts them there), a Windows copy of the sources with
# its VS environment (EMBER_SRC), and ffmpeg with libopus (FFMPEG).
# Usage: make-guide-audio.sh   (VOICE, DECK, EMBER_SRC, FFMPEG, WORK override the defaults below)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$HERE/../.." && pwd)
VOICE=${VOICE:-en_US-ljspeech-medium}
DECK=${DECK:-deck@192.168.20.58}
EMBER_SRC=${EMBER_SRC:-'C:\Users\FRaccie\source\repos\ember-build\src-a11y'}
FFMPEG=${FFMPEG:-/mnt/c/Users/FRaccie/source/repos/ember-build/tools/ffmpeg/bin/ffmpeg.exe}
WORK=${WORK:-/mnt/d/ember-build/guide-audio}; rm -rf $WORK; mkdir -p $WORK
OUT=$W/docs/guides/audio/accessibility; mkdir -p $OUT
python3 $HERE/sections.py $W/docs/guides/ACCESSIBILITY.md > $WORK/sections.jsonl
# Piper on the Deck: one JSON line per section, each to its own WAV.
python3 - $WORK/sections.jsonl > $WORK/piper.jsonl <<'PY'
import json, sys
for line in open(sys.argv[1]):
    s = json.loads(line)
    print(json.dumps({"text": s["text"], "output_file": "/tmp/ember-guide/%s.wav" % s["name"]}))
PY
# Piper takes the Deck's CPU for a while: never while the player is in the game.
ssh $DECK 'pgrep -f "^[A-Za-z]:.*(SSFIV|Launcher)\.exe" >/dev/null' && { echo "the game is running on the Deck; not generating"; exit 1; }
ssh $DECK "rm -rf /tmp/ember-guide; mkdir -p /tmp/ember-guide"
scp -q $WORK/piper.jsonl $DECK:/tmp/ember-guide/
ssh $DECK "cd ~/.cache/ember-speech/piper && timeout -k 5 300 ./piper -q --model ../voices/$VOICE.onnx --json-input --sentence_silence 0.35 < /tmp/ember-guide/piper.jsonl > /dev/null"
scp -q "$DECK:/tmp/ember-guide/*.wav" $WORK/
# The chime and the tick from AwayAlert.hxx, built with the VS environment of EMBER_SRC.
cp $HERE/tones.cxx $WORK/
powershell.exe -NoProfile -Command "& { . '$EMBER_SRC\scripts\BuildEnvironment.ps1'; Enter-EmberVcEnvironment (Get-EmberToolPaths '$EMBER_SRC' '' '$EMBER_SRC\..\vcpkg').VisualStudioPath; Set-Location '$(wslpath -w $WORK)'; cl /nologo /EHsc /std:c++17 /I '$EMBER_SRC\src' tones.cxx | Out-Null; .\tones.exe }"
for wav in $WORK/*.wav; do
    name=$(basename $wav .wav)
    $FFMPEG -hide_banner -loglevel error -y -i "$(wslpath -w $wav)" -ac 1 -c:a libopus -b:a 24k "$(wslpath -w $OUT/$name.ogg)"
done
ls -la $OUT
