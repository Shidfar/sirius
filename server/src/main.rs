//! Sirius TTS Server
//!
//! A WebSocket server that accepts text and returns synthesized audio.
//!
//! Usage:
//!   cargo run --release -p sirius-server
//!
//! The server listens on ws://127.0.0.1:9876 by default.

mod tts;

use std::net::SocketAddr;
use std::sync::Arc;

use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Mutex;
use tokio_tungstenite::tungstenite::Message;
use tracing::{error, info, warn};

use sirius_protocol::{Request, Response};
use tts::TtsEngine;

const DEFAULT_ADDR: &str = "127.0.0.1:9876";

#[tokio::main]
async fn main() -> Result<()> {
    // Initialize logging
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive("sirius_server=info".parse()?)
                .add_directive("tokio_tungstenite=warn".parse()?),
        )
        .init();

    let addr = std::env::var("SIRIUS_ADDR").unwrap_or_else(|_| DEFAULT_ADDR.to_string());

    // Initialize TTS engine (this loads the model - may take a moment)
    info!("Loading TTS model...");
    let model_path = std::env::var("SIRIUS_MODEL")
        .unwrap_or_else(|_| "checkpoints/kokoro-v1.0.onnx".to_string());
    let voices_path = std::env::var("SIRIUS_VOICES")
        .unwrap_or_else(|_| "data/voices-v1.0.bin".to_string());

    let tts = TtsEngine::new(&model_path, &voices_path).await?;
    let tts = Arc::new(Mutex::new(tts));

    info!("TTS model loaded successfully");

    // Start WebSocket server
    let listener = TcpListener::bind(&addr).await?;
    info!("Sirius TTS server listening on ws://{}", addr);

    while let Ok((stream, peer_addr)) = listener.accept().await {
        let tts = Arc::clone(&tts);
        tokio::spawn(async move {
            if let Err(e) = handle_connection(stream, peer_addr, tts).await {
                error!("Connection error from {}: {}", peer_addr, e);
            }
        });
    }

    Ok(())
}

async fn handle_connection(
    stream: TcpStream,
    peer_addr: SocketAddr,
    tts: Arc<Mutex<TtsEngine>>,
) -> Result<()> {
    info!("New connection from: {}", peer_addr);

    let ws_stream = tokio_tungstenite::accept_async(stream).await?;
    let (mut ws_sender, mut ws_receiver) = ws_stream.split();

    while let Some(msg) = ws_receiver.next().await {
        let msg = match msg {
            Ok(m) => m,
            Err(e) => {
                warn!("WebSocket error from {}: {}", peer_addr, e);
                break;
            }
        };

        match msg {
            Message::Text(text) => {
                // Parse the request
                let request: Request = match serde_json::from_str(&text) {
                    Ok(r) => r,
                    Err(e) => {
                        let error_response = Response::Error {
                            message: format!("Invalid request: {}", e),
                        };
                        ws_sender
                            .send(Message::Text(serde_json::to_string(&error_response)?))
                            .await?;
                        continue;
                    }
                };

                match request {
                    Request::Ping => {
                        let response = Response::Pong;
                        ws_sender
                            .send(Message::Text(serde_json::to_string(&response)?))
                            .await?;
                    }
                    Request::Synthesize(req) => {
                        info!(
                            "Synthesizing {} chars for {} (voice: {})",
                            req.text.len(),
                            peer_addr,
                            req.voice
                        );

                        let start = std::time::Instant::now();

                        // Generate audio
                        let tts_guard = tts.lock().await;
                        match tts_guard.synthesize(&req.text, &req.lang, &req.voice, req.speed) {
                            Ok(wav_data) => {
                                drop(tts_guard); // Release lock before sending

                                let duration_secs = wav_data.len() as f32
                                    / (sirius_protocol::SAMPLE_RATE as f32
                                        * sirius_protocol::CHANNELS as f32
                                        * 2.0); // 2 bytes per sample (16-bit)

                                info!(
                                    "Generated {:.2}s audio ({} bytes) in {:?}",
                                    duration_secs,
                                    wav_data.len(),
                                    start.elapsed()
                                );

                                // Send metadata first
                                let response = Response::AudioReady {
                                    duration_secs,
                                    sample_rate: sirius_protocol::SAMPLE_RATE,
                                    channels: sirius_protocol::CHANNELS,
                                    size_bytes: wav_data.len(),
                                };
                                ws_sender
                                    .send(Message::Text(serde_json::to_string(&response)?))
                                    .await?;

                                // Then send binary audio data
                                ws_sender.send(Message::Binary(wav_data)).await?;
                            }
                            Err(e) => {
                                error!("TTS error: {}", e);
                                let response = Response::Error {
                                    message: format!("TTS error: {}", e),
                                };
                                ws_sender
                                    .send(Message::Text(serde_json::to_string(&response)?))
                                    .await?;
                            }
                        }
                    }
                }
            }
            Message::Binary(_) => {
                warn!("Received unexpected binary message from {}", peer_addr);
            }
            Message::Ping(data) => {
                ws_sender.send(Message::Pong(data)).await?;
            }
            Message::Pong(_) => {}
            Message::Close(_) => {
                info!("Client {} disconnected", peer_addr);
                break;
            }
            Message::Frame(_) => {}
        }
    }

    info!("Connection closed: {}", peer_addr);
    Ok(())
}
