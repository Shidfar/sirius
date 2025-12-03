//! Audio playback module using rodio
//!
//! We use rodio instead of kira here because it's simpler for basic playback
//! and handles WAV decoding automatically.

use std::io::Cursor;
use std::time::Duration;

use anyhow::Result;
use rodio::{Decoder, OutputStream, Sink};
use kira::{AudioManager, AudioManagerSettings};

/// Play WAV audio from bytes
pub fn play_wav_bytes(wav_data: &[u8]) -> Result<()> {
    // Create output stream
    let (_stream, stream_handle) = OutputStream::try_default()?;

    // Create a sink for playback
    let sink = Sink::try_new(&stream_handle)?;

    // Decode WAV data
    let cursor = Cursor::new(wav_data.to_vec());
    let source = Decoder::new(cursor)?;

    // Get duration estimate before playing
    let duration = estimate_wav_duration(wav_data);

    // Play the audio
    sink.append(source);

    // Wait for playback to complete
    // We use sleep instead of sink.sleep_until_end() for more control
    if let Some(dur) = duration {
        std::thread::sleep(dur + Duration::from_millis(100)); // Add small buffer
    } else {
        sink.sleep_until_end();
    }

    Ok(())
}

/// Estimate WAV duration from header
fn estimate_wav_duration(wav_data: &[u8]) -> Option<Duration> {
    // Simple WAV header parsing
    // WAV format: RIFF header (12 bytes) + fmt chunk + data chunk
    // We need: sample rate (bytes 24-27) and data size

    if wav_data.len() < 44 {
        return None;
    }

    // Check RIFF header
    if &wav_data[0..4] != b"RIFF" || &wav_data[8..12] != b"WAVE" {
        return None;
    }

    // Get sample rate (little-endian u32 at offset 24)
    let sample_rate = u32::from_le_bytes([
        wav_data[24],
        wav_data[25],
        wav_data[26],
        wav_data[27],
    ]);

    // Get channels (little-endian u16 at offset 22)
    let channels = u16::from_le_bytes([wav_data[22], wav_data[23]]);

    // Get bits per sample (little-endian u16 at offset 34)
    let bits_per_sample = u16::from_le_bytes([wav_data[34], wav_data[35]]);

    // Find data chunk and get its size
    let mut pos = 12; // Skip RIFF header
    while pos + 8 < wav_data.len() {
        let chunk_id = &wav_data[pos..pos + 4];
        let chunk_size = u32::from_le_bytes([
            wav_data[pos + 4],
            wav_data[pos + 5],
            wav_data[pos + 6],
            wav_data[pos + 7],
        ]);

        if chunk_id == b"data" {
            // Calculate duration
            let bytes_per_sample = (bits_per_sample / 8) as u32;
            let num_samples = chunk_size / (channels as u32 * bytes_per_sample);
            let duration_secs = num_samples as f64 / sample_rate as f64;
            return Some(Duration::from_secs_f64(duration_secs));
        }

        pos += 8 + chunk_size as usize;
        // Align to word boundary
        if pos % 2 != 0 {
            pos += 1;
        }
    }

    None
}

/// Alternative playback using kira (if rodio doesn't work well)
#[allow(dead_code)]
pub fn play_wav_bytes_kira(wav_data: &[u8]) -> Result<()> {
    use kira::backend::cpal::CpalBackend;
    use kira::sound::static_sound::StaticSoundData;

    let cursor = Cursor::new(wav_data.to_vec());

    let mut manager = AudioManager::<CpalBackend>::new(AudioManagerSettings::default())?;
    let sound_data = StaticSoundData::from_cursor(cursor)?;

    let _handle = manager.play(sound_data)?;

    // Wait for playback
    if let Some(duration) = estimate_wav_duration(wav_data) {
        std::thread::sleep(duration + Duration::from_millis(100));
    } else {
        std::thread::sleep(Duration::from_secs(10)); // Fallback
    }

    Ok(())
}
