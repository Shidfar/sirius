mod audio;
mod pdfreader;
mod commands;
mod sirius;

use kokoro::tts::koko::TTSKoko;
use tokio::io::{AsyncBufReadExt, BufReader};


use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use unicode_segmentation::UnicodeSegmentation;

// #[derive(Debug)]
// struct Done {
//     message: String,
// }

fn read_page(tts: &TTSKoko, path: &str, idx: u32, page: Vec<String>, remove_top: bool) -> Result<(), Box<dyn std::error::Error>> {
    let path_obj = Path::new(path);
    let stem = path_obj.file_stem()
        .and_then(|os_str| os_str.to_str())
        .ok_or_else(|| format!("Could not extract valid UTF-8 filename stem from '{}'", path))?;
    std::fs::create_dir_all(format!("data/voices/{stem}/"))?;
    let output_file = format!("data/voices/{stem}/{stem}-{idx}.wav");
    println!("Output file: {}", output_file);

    let lines_to_process = if remove_top && !page.is_empty() {
        &page[1..]
    } else {
        &page[..]
    };

    let sentences: Vec<String> = lines_to_process
        .iter()
        .flat_map(|txt| txt.split_sentence_bounds())
        .map(String::from)
        .collect();

    if sentences.is_empty() {
        println!("No sentences found to synthesize.");
        return Ok(());
    }

    println!("Found {} sentences.", sentences.len());

    let mut audio: Vec<f32> = Vec::new();
    for sentence in &sentences {
        println!("  Generating for: '{}'", sentence);
        match audio::generate(tts, sentence, &mut audio) {
            Ok(_) => println!("   -> done"),
            Err(e) => {
                eprintln!("Error generating audio for sentence: '{}'. Error: {}", sentence, e);
                return Err(e.into());
            }
        }
    }

    if !audio.is_empty() {
        const CHANNELS: u16 = 1;
        const SAMPLE_RATE: u32 = 24000;

        println!("Saving audio ({} samples) to {}...", audio.len(), output_file);
        audio::save_f32_buffer(&output_file, &audio, CHANNELS, SAMPLE_RATE)?;
        // audio::play_f32_buffer(&audio, CHANNELS, SAMPLE_RATE)?;
    } else {
        eprintln!("Warning: Audio buffer is empty after processing. Skipping save for {}.", output_file);
    }

    Ok(())
}

fn read_doc(tts: &TTSKoko, path: &str, remove_top: bool) -> Result<(), Box<dyn std::error::Error>> {
    println!("Input path: {}", path);

    let tree: BTreeMap<u32, Vec<String>> = pdfreader::read(path)?;
    for (page_n, parts) in tree {
        read_page(&tts, path, page_n, parts, remove_top)?
    }

    Ok(())
}

fn list_docs() -> Result<Vec<String>, Box<dyn std::error::Error>> {
    let folder_path = "data/docs";

    let pdf_files: Vec<_> = std::fs::read_dir(folder_path)?
        .filter_map(|entry| {
            let path = entry.ok()?.path();
            let is_pdf = path.extension()?
                .to_str()?
                .eq_ignore_ascii_case("pdf");

            if is_pdf {
                Some(path)
            } else {
                None
            }
        }).collect();

    let paths: Vec<String> = pdf_files.iter()
        .filter_map(|path_buf: &PathBuf| {
            path_buf.to_str()
        }).map(String::from)
        .collect();

    Ok(paths)
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    const LIST: &str = "://list";
    const READ: &str = "://read";
    let mut docs = list_docs()?;

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async {
        let tts = TTSKoko::new("checkpoints/kokoro-v1.0.onnx", "data/voices-v1.0.bin").await;

        let stdin = tokio::io::stdin();
        let reader = BufReader::new(stdin);
        let mut lines = reader.lines();

        while let Some(line) = lines.next_line().await? {
            let stripped_line = line.trim();
            if stripped_line.is_empty() {
                continue;
            } else if stripped_line == LIST {
                docs = list_docs()?;
                println!("Found {} PDF(s):", docs.len());
                for (i, pdf) in docs.iter().enumerate() {
                    println!("{}: {}", i+1, pdf);
                }
            } else if stripped_line.starts_with(READ) {
                let command_parts: Vec<&str> = stripped_line.split_whitespace().collect();
                if command_parts.len() > 3 || command_parts.len() < 2 {
                    eprintln!("Usage: {READ} <doc_index> [true|1|yes]");
                    continue;
                }

                let ignore_top = command_parts.len() == 3;

                let idx_str = command_parts[1];
                match idx_str.parse::<usize>() {
                    Ok(idx) => {
                        if idx - 1 >= docs.len() || idx == 0 {
                            eprintln!("Error: Document index {} is out of bounds (max index is {}).", idx, if docs.is_empty() { 0 } else { docs.len() });
                            continue;
                        }

                        let path_to_read = &docs[idx - 1];
                        println!("Executing: read_doc for index {}, path '{}', ignore_top: {}", idx, path_to_read, ignore_top);

                        if let Err(read_err) = read_doc(&tts, path_to_read, ignore_top) {
                            eprintln!("Error reading document '{}': {}", path_to_read, read_err);
                        }
                    }
                    Err(parse_err) => {
                        eprintln!("Error: Invalid document index '{}'. Index must be a number. Parser error: {}", idx_str, parse_err);
                    }
                };
            } else {
                println!("------- Commands ---------");
                println!("{LIST} - will list all pdf docs in data/docs dir");
                println!("{READ} <doc_index> [true|1|yes] - will read the doc at index optionally skipping the header");
                println!("------- PDF LIST ---------");
                for (i, pdf) in docs.iter().enumerate() {
                    println!("{}: {}", i+1, pdf);
                }
            }
        }

        Ok(())
    })
}
