//! TTS engine wrapper around Kokoro

use std::io::Cursor;

use anyhow::Result;
use hound::{WavSpec, WavWriter};
use kokoro::tts::koko::TTSKoko;

use sirius_protocol::{BITS_PER_SAMPLE, CHANNELS, SAMPLE_RATE};

pub struct TtsEngine {
    tts: TTSKoko,
}

impl TtsEngine {
    pub async fn new(model_path: &str, voices_path: &str) -> Result<Self> {
        let tts = TTSKoko::new(model_path, voices_path).await;
        Ok(Self { tts })
    }

    /// Synthesize text to WAV audio bytes
    pub fn synthesize(
        &self,
        text: &str,
        lang: &str,
        voice: &str,
        speed: f32,
    ) -> Result<Vec<u8>> {
        let mut full_audio: Vec<f32> = Vec::new();

        // Process each sentence
        let sentences = text.split('.');
        for sentence in sentences {
            let trimmed = sentence.trim();
            if trimmed.is_empty() {
                continue;
            }

            match self.tts.tts_raw_audio(trimmed, lang, voice, speed, None) {
                Ok(raw_audio) => {
                    full_audio.extend_from_slice(&raw_audio);
                }
                Err(e) => {
                    return Err(anyhow::anyhow!("TTS generation error: {}", e));
                }
            }
        }

        // Convert f32 samples to WAV bytes
        let wav_data = encode_wav(&full_audio)?;
        Ok(wav_data)
    }
}

/// Encode f32 samples as WAV bytes
fn encode_wav(samples: &[f32]) -> Result<Vec<u8>> {
    // Convert f32 to i16
    let i16_samples: Vec<i16> = samples
        .iter()
        .map(|&s| (s.clamp(-1.0, 1.0) * 32767.0) as i16)
        .collect();

    // Create WAV in memory
    let mut wav_buffer = Cursor::new(Vec::new());
    {
        let spec = WavSpec {
            channels: CHANNELS,
            sample_rate: SAMPLE_RATE,
            bits_per_sample: BITS_PER_SAMPLE,
            sample_format: hound::SampleFormat::Int,
        };
        let mut writer = WavWriter::new(&mut wav_buffer, spec)?;
        for sample in i16_samples {
            writer.write_sample(sample)?;
        }
        writer.finalize()?;
    }

    Ok(wav_buffer.into_inner())
}
