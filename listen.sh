#!/usr/bin/env bash
# Listen, transcribe, display:   audio source -> gigaam -s -> epdtext -l
#
#   ./listen.sh                        live, from the microphone (MIC=plughw:N,0 picks the sound card)
#   ./listen.sh --file recording.wav   run a recording through the same chain (no microphone needed)
#
# Environment: INTERVAL=SECONDS  minimum time between screen refreshes (default 180)
#              GATE=LEVEL        speech level for gigaam -s (default 0.005; lower it for a quiet microphone)
set -euo pipefail
cd "$(dirname "$0")"

GIGAAM="${GIGAAM:-./gigaam}"
EPDTEXT="${EPDTEXT:-./epdtext}"

if [ "${1:-}" = "--file" ]; then
    [ -n "${2:-}" ] || { echo "usage: $0 [--file recording]" >&2; exit 2; }
    source_cmd=(ffmpeg -loglevel error -i "$2" -f s16le -ar 16000 -ac 1 -)
else
    source_cmd=(arecord -q -D "${MIC:-plughw:1,0}" -f S16_LE -r 16000 -c 1 -t raw)
fi

"${source_cmd[@]}" | "$GIGAAM" -s -v -g "${GATE:-0.005}" | "$EPDTEXT" -l -i "${INTERVAL:-180}"
