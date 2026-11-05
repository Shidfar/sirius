# Sirius - a voice diary in one file

Press a button, talk, press it again. Sirius records what you said, turns it into Russian text on the device (no cloud
speech service, no Python), encrypts the text and the audio so that only **you** can open them, and uploads them to your
own S3 bucket whenever there is Wi-Fi. Without Wi-Fi it just keeps them and uploads later.

Everything is **one program, `sirius`**: the speech model ([GigaAM-v3](https://github.com/salute-developers/GigaAM) through
ONNX Runtime), its libraries, encryption, Opus audio compression, the S3 upload, the microphone and display code are
inside the file. A clean Raspberry Pi OS needs nothing installed except that file.

## Who can read what

| Where | What is there | Who can open it |
|---|---|---|
| Right after recording, on the SD card | the audio, encrypted with a key **built into the program** | anyone who has the SD card *and* takes the key out of the program - a speed bump, not a lock |
| As soon as the text is ready | transcript and compressed audio, encrypted to **your public key** (age) | only the owner of your private key, which never touches the device. The locally encrypted audio is deleted at that moment |
| In the S3 bucket | those same two encrypted files | only you |
| After a confirmed upload | the local copies are deleted | - |

The device holds your *public* key only, so a lost or stolen Sirius reveals nothing that was already transcribed. The S3
user it uses can only write (`PutObject`), so it cannot read or delete your entries either. Plain text and plain audio are
never written to disk; decrypted data only passes through memory (locked out of swap).

What the bucket does show: when entries were made and how long they are (file names and sizes).

## What you need

- Raspberry Pi Zero 2 W (or another 64-bit Pi with 512 MB or more), **64-bit Raspberry Pi OS Lite** (Debian 12 "bookworm" or newer)
- Waveshare 2.13" e-Paper HAT+ (V4) - shows what Sirius is doing
- A USB microphone (plus a micro-USB OTG adapter on the Zero)
- [PiSugar 3](https://github.com/PiSugar/PiSugar) battery with its button (tap = start/stop). Any USB button or small keyboard works too (`EVDEV=` in `/etc/sirius.conf`), or run `sirius ctl toggle` from anywhere
- An AWS account for the S3 bucket

## One-time preparation (on your computer)

1. **Your key pair** - the private key stays with you:
   ```bash
   age-keygen -o sirius-key.txt      # prints the public key: age1...
   ```
   (`brew install age` / `apt install age`.) Back up `sirius-key.txt` somewhere safe. Without it nobody - including you - can read the entries.
2. **The bucket** - a private S3 bucket (Block Public Access on, Versioning on). Create an IAM user for the device with only this policy, and an access key for it:
   ```json
   {"Version": "2012-10-17", "Statement": [{"Effect": "Allow", "Action": "s3:PutObject", "Resource": "arn:aws:s3:::YOUR-BUCKET/entries/*"}]}
   ```

## Get the program

The Pi build is made on another machine (it is 250 MB because the speech model is inside).

- **On GitHub, no Linux needed** (only for a *private* repository: the downloadable file contains the local key): Actions tab -> "Build the Pi binary" -> Run workflow, then download the artifact `sirius-aarch64`, unzip it. (Optional repository secret `SIRIUS_LOCAL_KEY`, 64 hex digits from `openssl rand -hex 32`, makes every build use the same local key.)
- **On a Linux PC** (also WSL):
  ```bash
  sudo apt install gcc-aarch64-linux-gnu cmake make curl unzip xz-utils bzip2
  ./deps.sh aarch64        # static libraries from pinned sources, built against Debian 12's glibc
  ./fetch.sh aarch64       # ONNX Runtime and the speech models
  make ARCH=aarch64        # -> build/aarch64/sirius
  ```
- **On the Pi itself:** `./deps.sh && ./fetch.sh && make` (slow; linking the 250 MB file may need some swap).

The local key is made by the first `make` into `localkey.h` and kept; recordings are readable only by builds with the same
key, so update the program when `sirius status` shows nothing waiting for transcription.

## Set up a Pi

Flash 64-bit Raspberry Pi OS Lite with Raspberry Pi Imager (set Wi-Fi, user, SSH in its options), then:

```bash
scp sirius sirius@sirius.local:                  # from your computer
ssh sirius@sirius.local
chmod +x sirius
sudo ./sirius install       # copies itself to /usr/local/bin, switches on SPI and I2C, adds the boot service
sudo sirius setup           # asks for the age1... public key and the bucket details, tests an upload
sudo reboot                 # once, because SPI/I2C were just switched on
```

Plug in the microphone. After the reboot the display says "Tap the button to record".

## Using it

- **Single tap**: start recording; tap again to stop. The display shows RECORDING, then "Transcribing", then what is waiting or "Last upload".
- **Double tap**: transcribe and upload now.
- A transcript looks like `[0:00:12] text...` with the time of each sentence; an entry is `transcript.txt.age` and `audio.opus.age` in the bucket under `entries/<date>T<time>Z/`.
- Transcribing takes about as long as the recording, or a little longer for many short sentences (measured on a Zero 2 W: 1.2x), and starts when you stop; you can record again meanwhile.
- If the picture is upside down set `FLIP=1` in `/etc/sirius.conf` and `sudo systemctl restart sirius`.
- Read an entry on your computer: `age -d -i sirius-key.txt transcript.txt.age`, and `age -d -i sirius-key.txt audio.opus.age > entry.opus`.

## Commands

```
sirius install            set up a fresh Pi (root, once)          sirius status      what is waiting
sirius setup              keys and bucket (root)                  sirius sync        transcribe + upload now
sirius run                the diary (the service runs this)       sirius ctl toggle|sync   same as tapping
sirius record FILE        test the microphone (Ctrl-C to stop)    sirius diary cat FILE    decrypt a local recording to raw audio
sirius transcribe a.wav   speech to text for a 16 kHz mono WAV    sirius show TEXT   put text on the display
sirius age age1...        encrypt stdin for a key                 sirius s3put FILE KEY    one upload (S3_* from the environment)
```

Settings live in `/etc/sirius.conf` (the file `sirius setup` writes); logs: `journalctl -u sirius -f`.

## How it is built

- `deps.sh` builds libsodium, libopus/libogg, libcurl + mbedTLS, ALSA and lgpio as static libraries from pinned, checksummed sources. For the Pi it uses Debian 12's C library, so the program starts on Debian 12 and anything newer (the build fails if it needs a newer glibc).
- `fetch.sh` downloads ONNX Runtime and the models. They are embedded with `.incbin`; at run time ONNX Runtime is copied to an anonymous memory file and loaded from there, the models are read straight from the program's own pages.
- `certs/amazon-roots.pem` (Amazon's root certificates) is embedded too, so the device needs no certificate store.
- `epd/` is Waveshare's MIT-licensed driver, unchanged.
- `make dev` builds a small version without the models for quick edits (use `MODELS_DIR=` in the settings and `SIRIUS_ORT_LIB=` for ONNX Runtime).

| File | What it is |
|---|---|
| `sirius.c` | the entry point: picks the command |
| `daemon.c` | button, recording, background work, display, retry timers |
| `rec.c` / `diary.c` | microphone to the locally encrypted file / that file format |
| `entry.c` | transcribe, encrypt with age, upload, delete: the store of entries |
| `gigaam.c` | speech recognition (greedy RNN-T decoder on ONNX Runtime) |
| `age.c`, `oggopus.c`, `s3.c` | age encryption, Opus in Ogg, S3 upload |
| `epdtext.c`, `font8x16.h` | drawing text and status screens (Terminus font, SIL OFL) |
| `install.c`, `config.c` | installer and settings |
| `tests/` | `tests/run.sh` checks everything that needs no hardware; `mock_s3.py` is a stand-in S3 |

## Not yet tried on the real device

Built and tested here: the whole chain (record, transcribe, encrypt, upload, delete) with a simulated microphone and a
stand-in S3, the Pi binary under emulation, the file formats against the real `age` and `ffmpeg`. Not yet seen on hardware:
the USB microphone, the PiSugar tap register, the new status screens on the panel, an upload to real S3, memory use on a
512 MB Pi, and the GitHub workflow.

## Notes

- The speech model and the voice activity gate are tuned for Russian speech in a quiet room. Pauses over 0.7 s end a sentence.
- The e-Paper panel is refreshed only when the state changes (at most every 15 s, and once every 6 h to avoid a burnt-in picture).
- 32-bit Raspberry Pi OS is not supported (ONNX Runtime has no 32-bit ARM build).
