//! Sirius TTS Client
//!
//! A WebSocket client that sends text to the Sirius server and plays the returned audio.
//!
//! Usage:
//!   # Interactive mode (type text, press Enter to synthesize and play)
//!   cargo run --release -p sirius-client
//!
//!   # Single text mode
//!   cargo run --release -p sirius-client -- --text "Hello world"
//!
//!   # Save to file instead of playing
//!   cargo run --release -p sirius-client -- --text "Hello world" --output hello.wav

mod audio;

use std::io::{self, BufRead, Write};

use anyhow::Result;
use clap::Parser;
use futures_util::{SinkExt, StreamExt};
use tokio_tungstenite::tungstenite::Message;
use tracing::{error, info};

use sirius_protocol::{Request, Response, SynthesizeRequest};

#[derive(Parser, Debug)]
#[command(author, version, about = "Sirius TTS Client")]
struct Args {
    /// Server address
    #[arg(short, long, default_value = "ws://127.0.0.1:9876")]
    server: String,

    /// Text to synthesize (if not provided, runs in interactive mode)
    #[arg(short, long)]
    text: Option<String>,

    /// Output file (if not provided, plays audio directly)
    #[arg(short, long)]
    output: Option<String>,

    /// Voice to use
    #[arg(short, long, default_value = "am_onyx.4+bm_lewis.6")]
    voice: String,

    /// Language code
    #[arg(short, long, default_value = "en-us")]
    lang: String,

    /// Speech speed (0.5-2.0)
    #[arg(long, default_value = "0.99")]
    speed: f32,
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive("sirius_client=info".parse()?)
                .add_directive("tokio_tungstenite=warn".parse()?),
        )
        .init();

    let args = Args::parse();

    info!("Connecting to {}", args.server);
    let (ws_stream, _) = tokio_tungstenite::connect_async(&args.server).await?;
    let (mut ws_sender, mut ws_receiver) = ws_stream.split();
    info!("Connected!");

    if let Some(text) = args.text {
        // Single text mode
        synthesize_and_play(
            &mut ws_sender,
            &mut ws_receiver,
            &text,
            &args.voice,
            &args.lang,
            args.speed,
            args.output.as_deref(),
        )
        .await?;
    } else {
        // Interactive mode
        println!("Sirius TTS Client - Interactive Mode");
        println!("=====================================");
        println!("Type text and press Enter to synthesize and play.");
        println!("Commands:");
        println!("  :q or :quit - Exit");
        println!("  :v <voice>  - Change voice");
        println!("  :s <speed>  - Change speed (0.5-2.0)");
        println!();

        let mut voice = args.voice;
        let mut speed = args.speed;
        let lang = args.lang;

        let stdin = io::stdin();
        let mut stdout = io::stdout();

        loop {
            print!("> ");
            stdout.flush()?;

            let mut line = String::new();
            if stdin.lock().read_line(&mut line)? == 0 {
                // EOF
                break;
            }

            let line = line.trim();
            if line.is_empty() {
                continue;
            }

            // Handle commands
            if line == ":q" || line == ":quit" {
                println!("Goodbye!");
                break;
            }

            if let Some(new_voice) = line.strip_prefix(":v ") {
                voice = new_voice.trim().to_string();
                println!("Voice changed to: {}", voice);
                continue;
            }

            if let Some(new_speed) = line.strip_prefix(":s ") {
                match new_speed.trim().parse::<f32>() {
                    Ok(s) if (0.5..=2.0).contains(&s) => {
                        speed = s;
                        println!("Speed changed to: {}", speed);
                    }
                    _ => {
                        println!("Invalid speed. Use a value between 0.5 and 2.0");
                    }
                }
                continue;
            }

            // Synthesize and play
            if let Err(e) = synthesize_and_play(
                &mut ws_sender,
                &mut ws_receiver,
                line,
                &voice,
                &lang,
                speed,
                None,
            )
            .await
            {
                error!("Error: {}", e);
            }
        }
    }

    // Close the connection gracefully
    ws_sender.send(Message::Close(None)).await?;

    Ok(())
}

async fn synthesize_and_play<S, R>(
    sender: &mut S,
    receiver: &mut R,
    text: &str,
    voice: &str,
    lang: &str,
    speed: f32,
    output: Option<&str>,
) -> Result<()>
where
    S: SinkExt<Message> + Unpin,
    S::Error: std::error::Error + Send + Sync + 'static,
    R: StreamExt<Item = Result<Message, tokio_tungstenite::tungstenite::Error>> + Unpin,
{
    // Build and send request
    let request = Request::Synthesize(
        SynthesizeRequest::new(text)
            .with_voice(voice)
            .with_lang(lang)
            .with_speed(speed),
    );

    let request_json = serde_json::to_string(&request)?;
    sender.send(Message::Text(request_json)).await.map_err(|e| anyhow::anyhow!("{}", e))?;

    // Wait for response
    let mut audio_metadata: Option<Response> = None;

    while let Some(msg) = receiver.next().await {
        let msg = msg?;

        match msg {
            Message::Text(text) => {
                let response: Response = serde_json::from_str(&text)?;
                match &response {
                    Response::AudioReady { duration_secs, size_bytes, .. } => {
                        info!(
                            "Receiving audio: {:.2}s, {} bytes",
                            duration_secs, size_bytes
                        );
                        audio_metadata = Some(response);
                    }
                    Response::Error { message } => {
                        return Err(anyhow::anyhow!("Server error: {}", message));
                    }
                    Response::Pong => {}
                }
            }
            Message::Binary(data) => {
                if audio_metadata.is_some() {
                    info!("Received {} bytes of audio data", data.len());

                    if let Some(output_path) = output {
                        // Save to file
                        std::fs::write(output_path, &data)?;
                        println!("Audio saved to: {}", output_path);
                    } else {
                        // Play audio
                        println!("Playing audio...");
                        audio::play_wav_bytes(&data)?;
                    }

                    return Ok(());
                }
            }
            Message::Close(_) => {
                return Err(anyhow::anyhow!("Connection closed by server"));
            }
            _ => {}
        }
    }

    Err(anyhow::anyhow!("No audio received"))
}
