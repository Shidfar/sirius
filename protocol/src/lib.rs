//! Shared protocol definitions for Sirius TTS client-server communication.
//!
//! The protocol is simple:
//! - Client sends: JSON text message with the text to synthesize
//! - Server returns: Binary WAV audio data
//!
//! For control messages:
//! - Client can send commands like "flush" to clear server-side buffers (if any)

use serde::{Deserialize, Serialize};

/// Request from client to server
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "data")]
pub enum Request {
    /// Synthesize text to speech and return audio
    Synthesize(SynthesizeRequest),
    /// Ping to keep connection alive
    Ping,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SynthesizeRequest {
    /// The text to synthesize
    pub text: String,
    /// Voice to use (e.g., "am_onyx.4+bm_lewis.6")
    #[serde(default = "default_voice")]
    pub voice: String,
    /// Language code (e.g., "en-us")
    #[serde(default = "default_lang")]
    pub lang: String,
    /// Speech speed (0.0 to 2.0, default 0.99)
    #[serde(default = "default_speed")]
    pub speed: f32,
}

fn default_voice() -> String {
    "am_onyx.4+bm_lewis.6".to_string()
}

fn default_lang() -> String {
    "en-us".to_string()
}

fn default_speed() -> f32 {
    0.99
}

impl SynthesizeRequest {
    pub fn new(text: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            voice: default_voice(),
            lang: default_lang(),
            speed: default_speed(),
        }
    }

    pub fn with_voice(mut self, voice: impl Into<String>) -> Self {
        self.voice = voice.into();
        self
    }

    pub fn with_lang(mut self, lang: impl Into<String>) -> Self {
        self.lang = lang.into();
        self
    }

    pub fn with_speed(mut self, speed: f32) -> Self {
        self.speed = speed;
        self
    }
}

/// Response metadata (sent as JSON before binary audio)
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum Response {
    /// Audio is coming next as a binary message
    AudioReady {
        /// Duration in seconds
        duration_secs: f32,
        /// Sample rate
        sample_rate: u32,
        /// Number of channels
        channels: u16,
        /// Size of the WAV data in bytes
        size_bytes: usize,
    },
    /// Pong response to ping
    Pong,
    /// Error occurred
    Error { message: String },
}

/// Audio format constants
pub const SAMPLE_RATE: u32 = 24000;
pub const CHANNELS: u16 = 1;
pub const BITS_PER_SAMPLE: u16 = 16;
