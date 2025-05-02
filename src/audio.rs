use std::io::Cursor;
use hound::{WavSpec, WavWriter};
use kira::{AudioManager, AudioManagerSettings};
use kira::backend::cpal::CpalBackend;
use kira::sound::static_sound::StaticSoundData;
use kokoro::tts::koko::TTSKoko;

pub fn save_f32_buffer(save_path: &str, audio: &Vec<f32>, channels: u16, sample_rate: u32) -> Result<(), Box<dyn std::error::Error>> {
    let i16_samples: Vec<i16> = audio
        .iter()
        .map(|&s| (s.clamp(-1.0, 1.0) * 32767.0) as i16)
        .collect();

    let spec = WavSpec {
        channels,
        sample_rate,
        bits_per_sample: 16,
        sample_format: hound::SampleFormat::Int,
    };

    let mut writer = hound::WavWriter::create(save_path, spec)?;
    for sample in i16_samples {
        writer.write_sample(sample)?;
    }
    writer.finalize()?;

    Ok(())
}

pub fn play_f32_buffer(
    samples: &[f32], // Use a slice for flexibility
    channels: u16,
    sample_rate: u32,
) -> Result<(), Box<dyn std::error::Error>> {

    let i16_samples: Vec<i16> = samples
        .iter()
        .map(|&s| (s.clamp(-1.0, 1.0) * 32767.0) as i16)
        .collect();

    // Create a WAV buffer in memory using a Cursor
    let mut wav_buffer = Cursor::new(Vec::new());
    {
        let spec = WavSpec {
            channels,
            sample_rate,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };
        let mut writer = WavWriter::new(&mut wav_buffer, spec)?;
        for sample in i16_samples {
            writer.write_sample(sample)?;
        }
        writer.finalize()?; // Finalize the WAV file
    }

    // Reset the cursor to the beginning of the buffer
    wav_buffer.set_position(0);

    // Specify the backend type for the AudioManager
    let mut manager = AudioManager::<CpalBackend>::new(AudioManagerSettings::default())?;
    let sound_data = StaticSoundData::from_cursor(
        wav_buffer,
        // StaticSoundSettings::default(),
    )?;

    // Play the sound
    let _handle = manager.play(sound_data)?;

    // Calculate approximate duration of audio
    let duration_secs = samples.len() as f32 / (sample_rate as f32 * channels as f32);
    std::thread::sleep(std::time::Duration::from_secs_f32(duration_secs));

    Ok(())
}

pub fn generate(tts: &TTSKoko, text: &str, full_audio: &mut Vec<f32>) -> Result<(), Box<dyn std::error::Error>> {
    let s = std::time::Instant::now();

    match tts.tts_raw_audio(&text, "en-us", "af_heart.4+af_bella.6", 1.0, None) {
        Ok(raw_audio) => {
            full_audio.extend_from_slice(&raw_audio);
            // eprintln!("Audio buffered up. Ready for another line of text.");
        }
        Err(e) => eprintln!("Error processing line: {}", e),
    }

    println!("Time taken: {:?}", s.elapsed());
    let words_per_second = text.split_whitespace().count() as f32 / s.elapsed().as_secs_f32();
    println!("Words per second: {:.2}", words_per_second);

    Ok(())
}
