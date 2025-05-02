use std::collections::BTreeMap;
use std::io::{Error, ErrorKind};
use std::path::Path;

use lopdf::{Document, Object};
use rayon::iter::{IntoParallelIterator, ParallelIterator};
use serde::{Deserialize, Serialize};
use serde_json;

static IGNORE: &[&[u8]] = &[
    b"Length",
    b"BBox",
    b"FormType",
    b"Matrix",
    b"Type",
    b"XObject",
    b"Subtype",
    b"Filter",
    b"ColorSpace",
    b"Width",
    b"Height",
    b"BitsPerComponent",
    b"Length1",
    b"Length2",
    b"Length3",
    b"PTEX.FileName",
    b"PTEX.PageNumber",
    b"PTEX.InfoDict",
    b"FontDescriptor",
    b"ExtGState",
    b"MediaBox",
    b"Annot",
];

#[derive(Debug, Deserialize, Serialize)]
struct PdfText {
    text: BTreeMap<u32, Vec<String>>,
    errors: Vec<String>,
}

fn extract_text(doc: &Document, page_nums: &[u32]) -> Result<Vec<String>, Error> {
    let text_fragments = doc.extract_text_chunks(page_nums);
    let mut text: Vec<String> = Vec::new();
    for maybe_text_fragment in text_fragments.into_iter() {
        match maybe_text_fragment {
            Ok(text_fragment) => {
                text.push(text_fragment);
                // text.push_str(&text_fragment)
            }
            Err(err) => {
                Error::new(ErrorKind::Other, format!("could not process fragment: {err:}"));
            }
        }
    }

    Ok(text)
}

fn get_pdf_text(doc: &Document) -> Result<PdfText, Error> {
    let mut pdf_text: PdfText = PdfText {
        text: BTreeMap::new(),
        errors: Vec::new(),
    };
    let pages: Vec<Result<(u32, Vec<String>), Error>> = doc
        .get_pages()
        .into_par_iter()
        .map(
            |(page_num, page_id): (u32, (u32, u16))| -> Result<(u32, Vec<String>), Error> {
                let text_vec = extract_text(doc, &[page_num]).map_err(|e| {
                    Error::new(
                        ErrorKind::Other,
                        format!("Failed to extract text from page {page_num} id={page_id:?}: {e:}"),
                    )
                })?;

                let text = text_vec.iter().fold(String::new(), |mut acc, s| {
                    acc.push_str(s);
                    acc
                });

                Ok((
                    page_num,
                    text.split('\n').map(|s| s.trim_end().to_string())
                        .collect::<Vec<String>>(),
                ))
            },
        )
        .collect();
    for page in pages {
        match page {
            Ok((page_num, lines)) => {
                pdf_text.text.insert(page_num, lines);
            }
            Err(e) => {
                pdf_text.errors.push(e.to_string());
            }
        }
    }
    Ok(pdf_text)
}

fn filter_func(object_id: (u32, u16), object: &mut Object) -> Option<((u32, u16), Object)> {
    if IGNORE.contains(&object.type_name().unwrap_or_default()) {
        return None;
    }
    if let Ok(d) = object.as_dict_mut() {
        d.remove(b"Producer");
        d.remove(b"ModDate");
        d.remove(b"Creator");
        d.remove(b"ProcSet");
        d.remove(b"Procset");
        d.remove(b"XObject");
        d.remove(b"MediaBox");
        d.remove(b"Annots");
        if d.is_empty() {
            return None;
        }
    }
    Some((object_id, object.to_owned()))
}

fn load_pdf<P: AsRef<Path>>(path: P) -> Result<Document, Error> {
    Document::load_filtered(path, filter_func).map_err(|e| Error::new(ErrorKind::Other, e.to_string()))
}

pub fn read(file_path: &str) -> Result<BTreeMap<u32, Vec<String>>, Box<dyn std::error::Error>> {
    let doc = load_pdf(file_path)?;
    let text = get_pdf_text(&doc)?;
    if !text.errors.is_empty() {
        eprintln!("{file_path:?} has {} errors:", text.errors.len());
        for error in &text.errors {
            eprintln!("{error:?}");
        }
    }

    let data = serde_json::to_string_pretty(&text).unwrap();
    let txt = data.as_str();
    println!("{txt}");
    Ok(text.text)
}

// examples:
// https://github.com/J-F-Liu/lopdf/blob/main/examples/extract_text.rs
