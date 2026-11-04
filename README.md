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

## Files

| File | What it is |
|---|---|
| `gigaam.c` | The whole program: WAV reader, ONNX Runtime calls, greedy RNN-T decoder, text cleanup, chunking |
| `fetch.sh` | Downloads ONNX Runtime from NuGet (picks x64 or arm64) and the models from Hugging Face |
| `models/gigaam_v3.onnx` | Log-mel feature extractor, taken from the [onnx-asr](https://github.com/istupakov/onnx-asr) package (MIT) |
| `models/v3_e2e_rnnt_*` | GigaAM-v3 int8 encoder / decoder / joint / vocab from `istupakov/gigaam-v3-onnx` |

## Notes

- 32-bit Raspberry Pi OS is not supported: the ONNX Runtime package has no 32-bit ARM library.
- Long audio is cut at the quietest point near each chunk limit. Chunking changes the transcript.
- On glibc the program pins `M_MMAP_THRESHOLD`; without it peak memory is ~100 MB higher and varies with the input.
- Verified on x86_64 only: the whole-file output is byte-identical to the Python (`onnx-asr`) int8 RNN-T output.
  The aarch64 build was checked under QEMU emulation (same text on an 8 s clip), not on real hardware.
