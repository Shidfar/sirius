# Sirius TTS - Client/Server Architecture

A WebSocket-based TTS service using Kokoro for speech synthesis.

## Architecture

```
┌─────────────────┐         WebSocket          ┌─────────────────┐
│                 │  ───── text (JSON) ─────>  │                 │
│     Client      │                            │     Server      │
│  (sends text,   │  <──── WAV audio ───────   │  (loads Kokoro, │
│   plays audio)  │                            │   synthesizes)  │
└─────────────────┘                            └─────────────────┘
```

## Protocol

The protocol is simple and uses two message types:

### Client → Server (JSON)
```json
{
  "type": "Synthesize",
  "data": {
    "text": "Hello, world!",
    "voice": "am_onyx.4+bm_lewis.6",
    "lang": "en-us",
    "speed": 0.99
  }
}
```

### Server → Client
1. **Metadata** (JSON):
```json
{
  "type": "AudioReady",
  "duration_secs": 1.5,
  "sample_rate": 24000,
  "channels": 1,
  "size_bytes": 72044
}
```

2. **Audio** (Binary): Raw WAV file bytes

## Project Structure

```
sirius/
├── Cargo.toml          # Workspace root
├── protocol/           # Shared types (Request, Response)
│   └── src/lib.rs
├── server/             # TTS server
│   └── src/
│       ├── main.rs     # WebSocket server
│       └── tts.rs      # Kokoro wrapper
├── client/             # TTS client
│   └── src/
│       ├── main.rs     # CLI client
│       └── audio.rs    # Audio playback
└── legacy/             # Old standalone code
```

## Usage

### Start the Server

```bash
# Default: listens on ws://127.0.0.1:9876
cargo run --release -p sirius-server

# Custom address
SIRIUS_ADDR=0.0.0.0:9876 cargo run --release -p sirius-server

# Custom model paths
SIRIUS_MODEL=path/to/model.onnx SIRIUS_VOICES=path/to/voices.bin cargo run --release -p sirius-server
```

### Use the Client

```bash
# Interactive mode
cargo run --release -p sirius-client

# Single text
cargo run --release -p sirius-client -- --text "Hello, world!"

# Save to file instead of playing
cargo run --release -p sirius-client -- --text "Hello" --output hello.wav

# Custom server/voice
cargo run --release -p sirius-client -- --server ws://192.168.1.100:9876 --voice "bm_lewis" --text "Test"
```

### Interactive Mode Commands

```
> Hello, world!           # Type text and press Enter to synthesize
> :v bm_daniel           # Change voice
> :s 1.2                 # Change speed (0.5-2.0)
> :q                     # Quit
```

## Configuration

### Server Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `SIRIUS_ADDR` | `127.0.0.1:9876` | Listen address |
| `SIRIUS_MODEL` | `checkpoints/kokoro-v1.0.onnx` | Path to ONNX model |
| `SIRIUS_VOICES` | `data/voices-v1.0.bin` | Path to voices file |

### Client CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `-s, --server` | `ws://127.0.0.1:9876` | Server WebSocket URL |
| `-t, --text` | (none) | Text to synthesize (interactive if omitted) |
| `-o, --output` | (none) | Output WAV file (plays if omitted) |
| `-v, --voice` | `am_onyx.4+bm_lewis.6` | Voice to use |
| `-l, --lang` | `en-us` | Language code |
| `--speed` | `0.99` | Speech speed (0.5-2.0) |

## Building

```bash
# Build everything
cargo build --release

# Build only server
cargo build --release -p sirius-server

# Build only client  
cargo build --release -p sirius-client
```

## Dependencies Note

The server depends on `kokoro` - adjust the path in `server/Cargo.toml` to point to your local kokoro-rs installation:

```toml
kokoro = { path = "../../kokoro-rs" }  # Adjust this path
```


### Kudos to:

https://github.com/lucasjinreal/Kokoros

https://huggingface.co/spaces/hexgrad/Kokoro-TTS
