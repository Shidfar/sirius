use crate::audio;
use tokio::io::{AsyncBufReadExt, BufReader};
use kokoro::tts::koko::TTSKoko;


fn start() -> Result<(), Box<dyn std::error::Error>> {
    let rt = tokio::runtime::Runtime::new()?;

    const PLAY: &str = "://play";
    const FLUSH: &str = "://flush";

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
            } else if stripped_line == PLAY {
                audio::play_f32_buffer(&full_audio, 1, 24000)?;
            } else if stripped_line == FLUSH {
                full_audio = Vec::new();
                println!("Audio buffer is cleared");
                continue;
            } else {
                audio::generate(&tts, stripped_line, &mut full_audio)?
            }
        }

        Ok(())
    })
}
