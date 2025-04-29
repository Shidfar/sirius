use kokoro::tts::koko::TTSKoko;
use tokio::io::{AsyncBufReadExt, BufReader};

use kira::{
    backend::cpal::CpalBackend, sound::static_sound::StaticSoundData,
    AudioManager,
    AudioManagerSettings,
};

use hound::{WavSpec, WavWriter};
use std::io::Cursor;

fn save_f32_buffer(save_path: &str, audio: &Vec<f32>, channels: u16, sample_rate: u32) -> Result<(), Box<dyn std::error::Error>> {
    let spec = WavSpec {
        channels,
        sample_rate,
        bits_per_sample: 32,
        sample_format: hound::SampleFormat::Float,
    };

    let mut writer = hound::WavWriter::create(save_path, spec)?;
    for &sample in audio {
        writer.write_sample(sample)?;
    }
    writer.finalize()?;

    Ok(())
}

fn play_f32_buffer(
    samples: &[f32], // Use a slice for flexibility
    channels: u16,
    sample_rate: u32,
) -> Result<(), Box<dyn std::error::Error>> {
    // Convert f32 samples to i16 (kira expects audio in i16 format)
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

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async {

        let tts = TTSKoko::new("checkpoints/kokoro-v1.0.onnx", "data/voices-v1.0.bin").await;

        let stdin = tokio::io::stdin();
        let reader = BufReader::new(stdin);
        let mut lines = reader.lines();

        eprintln!("Entering streaming mode. Type text and press Enter. Use Ctrl+D to exit.");

        let mut full_audio: Vec<f32> = Vec::new();

        while let Some(line) = lines.next_line().await? {
            let stripped_line = line.trim();
            if stripped_line.is_empty() {
                continue;
            }

            if stripped_line == "::command://play" {
                play_f32_buffer(&full_audio, 1, 24000)?;
                continue;
            }

            if stripped_line == "::command://flush" {
                full_audio = Vec::new();
                println!("Audio buffer is cleared");
                continue
            }

            if stripped_line.starts_with("::command://save") {
                let command_parts: Vec<&str> = stripped_line.split_whitespace().collect();
                if command_parts.len() != 2 {
                    eprintln!("Output file is not specified. Ignoring command.");
                    continue;
                }
                let filename = command_parts[1];
                save_f32_buffer(filename, &full_audio, 1, 24000)?;
                println!("Saving to {filename} is done");
                continue;
            }

            let s = std::time::Instant::now();

            match tts.tts_raw_audio(&stripped_line, "en-us", "af_heart.4+af_bella.6", 1.0, None) {
                Ok(raw_audio) => {
                    full_audio.extend_from_slice(&raw_audio);

                    eprintln!("Audio buffered up. Ready for another line of text.");
                }
                Err(e) => eprintln!("Error processing line: {}", e),
            }

            println!("Time taken: {:?}", s.elapsed());
            let words_per_second =
                stripped_line.split_whitespace().count() as f32 / s.elapsed().as_secs_f32();
            println!("Words per second: {:.2}", words_per_second);
        }

        Ok(())
    })
}
