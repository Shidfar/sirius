#!/usr/bin/env bash
# Checks the parts of Sirius that do not need the Pi's hardware. Run it from the repository root after `make dev`
# (or `make`): needs age, ffmpeg and python3 on the PC. The speech test also needs ./fetch.sh to have run.
#
#   tests/run.sh                    uses build/<cpu>-dev/sirius, else build/<cpu>/sirius
set -u
cd "$(dirname "$0")/.."
ARCH="$(uname -m)"
SIR="$PWD/${SIRIUS:-build/$ARCH-dev/sirius}"
[ -x "$SIR" ] || SIR="$PWD/build/$ARCH/sirius"
[ -x "$SIR" ] || { echo "build it first: make dev"; exit 1; }
for tool in age age-keygen ffmpeg python3; do command -v $tool >/dev/null || { echo "needs $tool"; exit 1; }; done

T="$(mktemp -d)"
trap 'kill $MOCK 2>/dev/null; rm -rf "$T"' EXIT
fail=0
check() { if "${@:2}" >"$T/out" 2>&1; then echo "ok    $1"; else echo "FAIL  $1"; sed 's/^/        /' "$T/out" | head -n 8; fail=1; fi; }

age-keygen -o "$T/key.txt" 2>"$T/pub.txt"
PUB="$(grep -o 'age1[a-z0-9]*' "$T/pub.txt")"

# --- age: the real `age` must open what we write, at every chunk boundary
age_roundtrip() {
  for n in 0 1 16 65535 65536 65537 131072 300000; do
    head -c $n /dev/urandom >"$T/in"
    "$SIR" age "$PUB" <"$T/in" >"$T/enc" && age -d -i "$T/key.txt" "$T/enc" | cmp - "$T/in" || { echo "size $n"; return 1; }
  done
}
age_rejects_truncation() { head -c 70000 /dev/urandom | "$SIR" age "$PUB" >"$T/enc"; ! age -d -i "$T/key.txt" <(head -c 40000 "$T/enc") >/dev/null 2>&1; }
age_rejects_bad_key() { ! "$SIR" age age1qqqqq </dev/null >/dev/null 2>&1; }
check "age output opens with the real age (8 sizes)" age_roundtrip
check "age: a truncated file does not open" age_rejects_truncation
check "age: a bad recipient is refused" age_rejects_bad_key

# --- opus: every length decodes to exactly the input length
opus_lengths() {
  for n in 0 2 640 641 9999 64000 64002; do
    head -c $n /dev/zero | "$SIR" opus >"$T/t.opus" || return 1
    got=$(ffmpeg -v error -y -i "$T/t.opus" -f s16le -ar 48000 - | wc -c)
    [ "$got" = $(( n / 2 * 6 )) ] || { echo "n=$n decoded $got bytes"; return 1; }
  done
}
check "opus: decoded length equals input length" opus_lengths

# --- diary: encrypted local store
head -c 300000 /dev/urandom >"$T/pcm"
diary_roundtrip() { "$SIR" diary rec "$T/a.enc" <"$T/pcm" && "$SIR" diary cat "$T/a.enc" | cmp - "$T/pcm"; }
diary_cut() { head -c 100000 "$T/a.enc" >"$T/cut.enc"; "$SIR" diary cat "$T/cut.enc" >"$T/o" 2>/dev/null; [ $? = 3 ] && [ "$(stat -c %s "$T/o")" = 98304 ] && head -c 98304 "$T/pcm" | cmp - "$T/o"; }
diary_tamper() { cp "$T/a.enc" "$T/bad.enc"; printf 'X' | dd of="$T/bad.enc" bs=1 seek=70000 conv=notrunc 2>/dev/null; "$SIR" diary cat "$T/bad.enc" >/dev/null 2>&1; [ $? = 4 ]; }
diary_no_overwrite() { ! "$SIR" diary rec "$T/a.enc" </dev/null >/dev/null 2>&1; }
check "diary: encrypt and decrypt" diary_roundtrip
check "diary: a power cut keeps everything up to the last second" diary_cut
check "diary: a damaged file is detected" diary_tamper
check "diary: never overwrites a recording" diary_no_overwrite

# --- S3: signature checked by a separate implementation (tests/mock_s3.py)
mkdir -p "$T/store"
python3 tests/mock_s3.py 18099 "$T/store" 2>"$T/mock.log" &
MOCK=$!
sleep 1
export S3_BUCKET=diary-test S3_REGION=eu-north-1 S3_ACCESS_KEY=AKIATESTKEY S3_SECRET_KEY=testsecret S3_ENDPOINT=http://127.0.0.1:18099
head -c 300000 /dev/urandom >"$T/obj"
s3_put() { "$SIR" s3put "$T/obj" entries/t/x.age && cmp "$T/obj" "$T/store/entries/t/x.age"; }
s3_wrong_secret() { S3_SECRET_KEY=wrong "$SIR" s3put "$T/obj" entries/t/y.age >/dev/null 2>&1; [ $? = 2 ]; }
s3_server_error_then_ok() { "$SIR" s3put "$T/obj" fail500/1/z >/dev/null 2>&1; [ $? = 1 ] && "$SIR" s3put "$T/obj" fail500/1/z >/dev/null 2>&1; }
s3_offline() { S3_ENDPOINT=http://127.0.0.1:9 "$SIR" s3put "$T/obj" q >/dev/null 2>&1; [ $? = 1 ]; }
check "s3: signed upload arrives intact" s3_put
check "s3: wrong secret is refused (not retried)" s3_wrong_secret
check "s3: server error is retried later, then works" s3_server_error_then_ok
check "s3: no connection means retry later" s3_offline

# --- the whole chain: recording -> transcript + opus, both encrypted -> upload -> nothing left locally
if [ -f models/v3_e2e_rnnt_encoder.int8.onnx ]; then
  export SIRIUS_CONF="$T/sirius.conf" SIRIUS_ORT_LIB="$PWD/ort/lib-$ARCH/libonnxruntime.so"
  mkdir -p "$T/data/entries/20260101T000000Z"
  printf 'DATA_DIR=%s\nAGE_RECIPIENT=%s\nMODELS_DIR=%s\nS3_BUCKET=diary-test\nS3_REGION=eu-north-1\nS3_ACCESS_KEY=AKIATESTKEY\nS3_SECRET_KEY=testsecret\nS3_ENDPOINT=%s\n' \
    "$T/data" "$PUB" "$PWD/models" "$S3_ENDPOINT" >"$T/sirius.conf"
  ffmpeg -v error -f lavfi -i "sine=frequency=300:duration=3" -ar 16000 -ac 1 -f s16le "$T/tone.pcm"
  "$SIR" diary rec "$T/data/entries/20260101T000000Z/audio.enc" <"$T/tone.pcm"
  chain() {
    "$SIR" sync 2>"$T/sync.log" || return 1
    [ -z "$(find "$T/data" -type f)" ] || { echo "files left locally"; return 1; }
    age -d -i "$T/key.txt" "$T/store/entries/20260101T000000Z/transcript.txt.age" | grep -q "Sirius diary entry 20260101T000000Z" || return 1
    age -d -i "$T/key.txt" "$T/store/entries/20260101T000000Z/audio.opus.age" >"$T/a.opus" && ffprobe -v error -show_entries format=duration -of default=nw=1 "$T/a.opus" | grep -q 'duration=3'
  }
  check "chain: record, transcribe, encrypt, upload, clean up" chain
else
  echo "skip  chain (run ./fetch.sh for the speech models)"
fi

[ $fail = 0 ] && echo "all good" || echo "SOME CHECKS FAILED"
exit $fail
