mod audio;
mod sirius;

use tokio::io::{AsyncBufReadExt};

use unicode_segmentation::UnicodeSegmentation;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    sirius::start()
}
