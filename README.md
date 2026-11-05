# gigaam-c — Russian speech-to-text in plain C

GigaAM-v3 (RNN-T, int8) running through ONNX Runtime's C API. No Python at runtime.

## Build and run (x86_64 or 64-bit Raspberry Pi OS)

```bash
sudo apt install build-essential curl unzip ffmpeg   # Debian / Pi OS
./fetch.sh        # ONNX Runtime (headers + .so) and the int8 models, ~400 MB
make
ffmpeg -i recording.m4a -ar 16000 -ac 1 -c:a pcm_s16le recording.wav
./gigaam -v recording.wav
```

Run `./gigaam` with no arguments to list the options. Input must be 16-bit PCM, mono, 16 kHz WAV.

## Show text on a Waveshare 2.13" e-Paper HAT (V4, 250x122, black/white)

`epdtext` word-wraps UTF-8 text (Latin and Cyrillic) onto the panel through Waveshare's C driver. SPI must be on
(`sudo raspi-config nonint do_spi 0`).

```bash
sudo apt install liblgpio-dev    # GPIO library used by the Waveshare C driver (else build lgpio from source)
git clone --depth 1 --filter=blob:none --sparse https://github.com/waveshare/e-Paper.git ~/e-Paper
(cd ~/e-Paper && git sparse-checkout set RaspberryPi_JetsonNano)
make epdtext                     # uses ~/e-Paper/RaspberryPi_JetsonNano/c; override with EPD_DIR=...
./epdtext "Привет, Sirius!"
./gigaam recording.wav | ./epdtext
```

If the text is upside down, add `-r`. `make preview` builds the same renderer without the driver; `-p out.pbm`
writes the picture to a file instead of the display. Waveshare asks for at least 180 s between refreshes and at
least one refresh every 24 hours; `epdtext` puts the panel to sleep after each update.

`epdtext -l` (loop mode) reads lines from stdin and adds each one to the text on the screen. It refreshes when
new text has arrived, at most once every `-i` seconds (default 180), so lines that arrive in between are shown
together.

## Listening: microphone to screen

`gigaam -s` is the stream mode: it reads raw 16 kHz mono 16-bit PCM from stdin (for example from `arecord`),
loads the model once, finds utterances with a simple energy gate and prints one line of text per utterance.
`-g LEVEL` sets the speech level (default 0.005 of full scale); the gate rises by itself in a noisy room.
Pauses longer than 0.7 s end an utterance.

```bash
arecord -q -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw | ./gigaam -s -v    # see `arecord -l` for the card number
./listen.sh                        # microphone -> gigaam -s -> epdtext -l
./listen.sh --file recording.wav   # the same chain with a recording (no microphone needed)
```

`listen.sh` reads `MIC` (sound card, default `plughw:1,0`), `INTERVAL` (screen refresh interval, default 180)
and `GATE` from the environment. The microphone should be a USB one: the e-Paper HAT switches its power on
GPIO 18, which is also the I2S clock pin.

## Files

| File | What it is |
|---|---|
| `gigaam.c` | The whole program: WAV reader, ONNX Runtime calls, greedy RNN-T decoder, text cleanup, chunking, stream mode |
| `epdtext.c` | Text-to-panel program: word wrap, 8x16 glyph drawing, Waveshare driver calls, loop mode |
| `listen.sh` | Chains `arecord` (or `ffmpeg` for a file), `gigaam -s` and `epdtext -l` |
| `font8x16.h` | Glyph table generated from [Terminus Font](https://terminus-font.sourceforge.net/) 4.48 (SIL OFL 1.1) |
| `fetch.sh` | Downloads ONNX Runtime from NuGet (picks x64 or arm64) and the models from Hugging Face |
| `models/gigaam_v3.onnx` | Log-mel feature extractor, taken from the [onnx-asr](https://github.com/istupakov/onnx-asr) package (MIT) |
| `models/v3_e2e_rnnt_*` | GigaAM-v3 int8 encoder / decoder / joint / vocab from `istupakov/gigaam-v3-onnx` |

## Notes

- 32-bit Raspberry Pi OS is not supported: the ONNX Runtime package has no 32-bit ARM library.
- Long audio is cut at the quietest point near each chunk limit. Chunking changes the transcript.
- On glibc the program pins `M_MMAP_THRESHOLD`; without it peak memory is ~100 MB higher and varies with the input.
- On x86_64 the whole-file output is byte-identical to the Python (`onnx-asr`) int8 RNN-T output.
- Measured on a Raspberry Pi Zero 2 W (512 MB, 64-bit Pi OS Lite), 59 s clip: model load 32.5 s, decode 69.6 s
  (1.17x real time, 4 threads), peak memory 269 MB.
