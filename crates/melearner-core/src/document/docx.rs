use std::collections::HashSet;
use std::io::{Cursor, Read};

use quick_xml::Reader;
use quick_xml::XmlVersion;
use quick_xml::events::{BytesStart, Event};
use zip::ZipArchive;

use super::{
    BlockKind, DocumentBuilder, DocumentError, DocumentFormat, DocumentWarningCode,
    MAX_NESTING_DEPTH, NormalizedDocument,
};

const MAX_ARCHIVE_ENTRIES: usize = 256;
const MAX_ENTRY_BYTES: u64 = 16 * 1024 * 1024;
const MAX_ARCHIVE_BYTES: u64 = 32 * 1024 * 1024;
const MAX_XML_BYTES: u64 = 4 * 1024 * 1024;

pub(super) fn normalize(source: &[u8]) -> Result<NormalizedDocument, DocumentError> {
    let mut archive =
        ZipArchive::new(Cursor::new(source)).map_err(|_| DocumentError::InvalidDocx)?;
    inspect_archive(&mut archive)?;

    let mut builder = DocumentBuilder::new(DocumentFormat::Docx);
    let relationship_name = "word/_rels/document.xml.rels";
    if archive.file_names().any(|name| name == relationship_name) {
        let relationships = read_part(&mut archive, relationship_name, MAX_XML_BYTES)?;
        parse_relationships(&relationships, &mut builder)?;
    }
    let document = read_part(&mut archive, "word/document.xml", MAX_XML_BYTES)?;
    parse_document(&document, &mut builder)?;
    Ok(builder.finish())
}

fn inspect_archive(archive: &mut ZipArchive<Cursor<&[u8]>>) -> Result<(), DocumentError> {
    if archive.len() > MAX_ARCHIVE_ENTRIES {
        return Err(DocumentError::ArchiveLimitExceeded);
    }
    let mut total = 0_u64;
    let mut names = HashSet::with_capacity(archive.len());
    for index in 0..archive.len() {
        let file = archive
            .by_index(index)
            .map_err(|_| DocumentError::InvalidDocx)?;
        let Some(path) = file.enclosed_name() else {
            return Err(DocumentError::InvalidDocx);
        };
        if !names.insert(path.to_owned()) {
            return Err(DocumentError::InvalidDocx);
        }
        if matches!(
            path.extension().and_then(|extension| extension.to_str()),
            Some("xml" | "rels")
        ) && file.size() > MAX_XML_BYTES
        {
            return Err(DocumentError::XmlLimitExceeded);
        }
        if file.size() > MAX_ENTRY_BYTES {
            return Err(DocumentError::ArchiveLimitExceeded);
        }
        total = total
            .checked_add(file.size())
            .ok_or(DocumentError::ArchiveLimitExceeded)?;
        if total > MAX_ARCHIVE_BYTES {
            return Err(DocumentError::ArchiveLimitExceeded);
        }
    }
    Ok(())
}

fn read_part(
    archive: &mut ZipArchive<Cursor<&[u8]>>,
    name: &str,
    limit: u64,
) -> Result<Vec<u8>, DocumentError> {
    let file = archive
        .by_name(name)
        .map_err(|_| DocumentError::InvalidDocx)?;
    if file.size() > limit {
        return Err(DocumentError::XmlLimitExceeded);
    }
    let mut contents = Vec::with_capacity(file.size() as usize);
    file.take(limit + 1)
        .read_to_end(&mut contents)
        .map_err(|_| DocumentError::InvalidDocx)?;
    if contents.len() as u64 > limit {
        return Err(DocumentError::XmlLimitExceeded);
    }
    Ok(contents)
}

fn parse_relationships(source: &[u8], builder: &mut DocumentBuilder) -> Result<(), DocumentError> {
    let mut reader = Reader::from_reader(source);
    let mut depth = 0_usize;
    loop {
        match reader
            .read_event()
            .map_err(|_| DocumentError::InvalidDocx)?
        {
            Event::Start(element) => {
                depth += 1;
                check_depth(depth)?;
                inspect_relationship(&element, &reader, builder)?;
            }
            Event::Empty(element) => inspect_relationship(&element, &reader, builder)?,
            Event::End(_) => {
                depth = depth.checked_sub(1).ok_or(DocumentError::InvalidDocx)?;
            }
            Event::DocType(_) => return Err(DocumentError::InvalidDocx),
            Event::Eof => break,
            _ => {}
        }
    }
    if depth != 0 {
        return Err(DocumentError::InvalidDocx);
    }
    Ok(())
}

fn inspect_relationship(
    element: &BytesStart<'_>,
    reader: &Reader<&[u8]>,
    builder: &mut DocumentBuilder,
) -> Result<(), DocumentError> {
    if local_name(element.name().as_ref()) != b"Relationship" {
        return Ok(());
    }
    for attribute in element.attributes() {
        let attribute = attribute.map_err(|_| DocumentError::InvalidDocx)?;
        if local_name(attribute.key.as_ref()) == b"TargetMode"
            && attribute
                .decoded_and_normalized_value(XmlVersion::Implicit1_0, reader.decoder())
                .map_err(|_| DocumentError::InvalidDocx)?
                .eq_ignore_ascii_case("external")
        {
            builder.warn(DocumentWarningCode::DocxExternalRelationshipOmitted);
        }
    }
    Ok(())
}

#[derive(Default)]
struct Paragraph {
    text: String,
    style: Option<String>,
    numbered: bool,
}

#[derive(Default)]
struct Table {
    rows: Vec<Vec<String>>,
    row: Option<Vec<String>>,
    cell: Option<String>,
}

fn parse_document(source: &[u8], builder: &mut DocumentBuilder) -> Result<(), DocumentError> {
    let mut reader = Reader::from_reader(source);
    let mut depth = 0_usize;
    let mut paragraph = None;
    let mut in_text = false;
    let mut table = None;
    let mut skipped_depth = 0_usize;

    loop {
        match reader
            .read_event()
            .map_err(|_| DocumentError::InvalidDocx)?
        {
            Event::Start(element) => {
                depth += 1;
                check_depth(depth)?;
                if skipped_depth != 0 {
                    skipped_depth += 1;
                    continue;
                }
                if start_element(
                    &element,
                    &reader,
                    builder,
                    &mut paragraph,
                    &mut in_text,
                    &mut table,
                )? {
                    skipped_depth = 1;
                }
            }
            Event::Empty(element) => {
                if skipped_depth == 0 {
                    empty_element(&element, &reader, builder, &mut paragraph, &mut table)?;
                }
            }
            Event::Text(text) if skipped_depth == 0 && in_text => {
                let decoded = text.decode().map_err(|_| DocumentError::InvalidDocx)?;
                let unescaped = quick_xml::escape::unescape(&decoded)
                    .map_err(|_| DocumentError::InvalidDocx)?;
                if let Some(paragraph) = paragraph.as_mut() {
                    paragraph.text.push_str(&unescaped);
                }
            }
            Event::CData(text) if skipped_depth == 0 && in_text => {
                if let Some(paragraph) = paragraph.as_mut() {
                    paragraph
                        .text
                        .push_str(&text.decode().map_err(|_| DocumentError::InvalidDocx)?);
                }
            }
            Event::End(element) => {
                if skipped_depth != 0 {
                    skipped_depth -= 1;
                } else {
                    end_element(
                        local_name(element.name().as_ref()),
                        builder,
                        &mut paragraph,
                        &mut in_text,
                        &mut table,
                    )?;
                }
                depth = depth.checked_sub(1).ok_or(DocumentError::InvalidDocx)?;
            }
            Event::DocType(_) => return Err(DocumentError::InvalidDocx),
            Event::Eof => break,
            _ => {}
        }
    }
    if depth != 0 || skipped_depth != 0 || paragraph.is_some() || table.is_some() {
        return Err(DocumentError::InvalidDocx);
    }
    Ok(())
}

fn start_element(
    element: &BytesStart<'_>,
    reader: &Reader<&[u8]>,
    builder: &mut DocumentBuilder,
    paragraph: &mut Option<Paragraph>,
    in_text: &mut bool,
    table: &mut Option<Table>,
) -> Result<bool, DocumentError> {
    match local_name(element.name().as_ref()) {
        b"p" => {
            if paragraph.is_some() {
                return Err(DocumentError::InvalidDocx);
            }
            *paragraph = Some(Paragraph::default());
        }
        b"pStyle" => set_style(element, reader, paragraph)?,
        b"numPr" => {
            if let Some(paragraph) = paragraph.as_mut() {
                paragraph.numbered = true;
            }
        }
        b"t" => *in_text = true,
        b"tab" => append_control(paragraph, '\t'),
        b"br" | b"cr" => append_control(paragraph, '\n'),
        b"tbl" => {
            if table.is_some() {
                return Err(DocumentError::InvalidDocx);
            }
            *table = Some(Table::default());
        }
        b"tr" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            if table.row.is_some() {
                return Err(DocumentError::InvalidDocx);
            }
            table.row = Some(Vec::new());
        }
        b"tc" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            if table.cell.is_some() {
                return Err(DocumentError::InvalidDocx);
            }
            table.cell = Some(String::new());
        }
        b"drawing" | b"pict" => {
            builder.warn(DocumentWarningCode::DocxEmbeddedImageOmitted);
            return Ok(true);
        }
        b"altChunk" | b"object" | b"oleObject" | b"fldSimple" | b"instrText" => {
            builder.warn(DocumentWarningCode::DocxUnsupportedConstructOmitted);
            return Ok(true);
        }
        _ => {}
    }
    Ok(false)
}

fn empty_element(
    element: &BytesStart<'_>,
    reader: &Reader<&[u8]>,
    builder: &mut DocumentBuilder,
    paragraph: &mut Option<Paragraph>,
    table: &mut Option<Table>,
) -> Result<(), DocumentError> {
    match local_name(element.name().as_ref()) {
        b"pStyle" => set_style(element, reader, paragraph)?,
        b"tab" => append_control(paragraph, '\t'),
        b"br" | b"cr" => append_control(paragraph, '\n'),
        b"drawing" | b"pict" => {
            builder.warn(DocumentWarningCode::DocxEmbeddedImageOmitted);
        }
        b"altChunk" | b"object" | b"oleObject" | b"fldSimple" | b"instrText" => {
            builder.warn(DocumentWarningCode::DocxUnsupportedConstructOmitted);
        }
        b"tr" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            table.rows.push(Vec::new());
        }
        b"tc" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            let row = table.row.as_mut().ok_or(DocumentError::InvalidDocx)?;
            row.push(String::new());
        }
        _ => {}
    }
    Ok(())
}

fn end_element(
    name: &[u8],
    builder: &mut DocumentBuilder,
    paragraph: &mut Option<Paragraph>,
    in_text: &mut bool,
    table: &mut Option<Table>,
) -> Result<(), DocumentError> {
    match name {
        b"t" => *in_text = false,
        b"p" => {
            let paragraph = paragraph.take().ok_or(DocumentError::InvalidDocx)?;
            let text = paragraph.text.trim().to_owned();
            if let Some(cell) = table.as_mut().and_then(|table| table.cell.as_mut()) {
                if !cell.is_empty() && !text.is_empty() {
                    cell.push('\n');
                }
                cell.push_str(&text);
            } else {
                let (kind, level) = paragraph_kind(&paragraph);
                builder.push(kind, level, text)?;
            }
        }
        b"tc" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            let cell = table.cell.take().ok_or(DocumentError::InvalidDocx)?;
            table
                .row
                .as_mut()
                .ok_or(DocumentError::InvalidDocx)?
                .push(cell);
        }
        b"tr" => {
            let table = table.as_mut().ok_or(DocumentError::InvalidDocx)?;
            let row = table.row.take().ok_or(DocumentError::InvalidDocx)?;
            table.rows.push(row);
        }
        b"tbl" => {
            let table = table.take().ok_or(DocumentError::InvalidDocx)?;
            builder.push(BlockKind::Table, 0, render_table(&table.rows))?;
        }
        _ => {}
    }
    Ok(())
}

fn set_style(
    element: &BytesStart<'_>,
    reader: &Reader<&[u8]>,
    paragraph: &mut Option<Paragraph>,
) -> Result<(), DocumentError> {
    let Some(paragraph) = paragraph.as_mut() else {
        return Ok(());
    };
    for attribute in element.attributes() {
        let attribute = attribute.map_err(|_| DocumentError::InvalidDocx)?;
        if local_name(attribute.key.as_ref()) == b"val" {
            paragraph.style = Some(
                attribute
                    .decoded_and_normalized_value(XmlVersion::Implicit1_0, reader.decoder())
                    .map_err(|_| DocumentError::InvalidDocx)?
                    .into_owned(),
            );
        }
    }
    Ok(())
}

fn append_control(paragraph: &mut Option<Paragraph>, value: char) {
    if let Some(paragraph) = paragraph.as_mut() {
        paragraph.text.push(value);
    }
}

fn paragraph_kind(paragraph: &Paragraph) -> (BlockKind, u8) {
    if let Some(level) = paragraph
        .style
        .as_deref()
        .and_then(|style| style.strip_prefix("Heading"))
        .and_then(|level| level.parse::<u8>().ok())
        .filter(|level| (1..=6).contains(level))
    {
        (BlockKind::Heading, level)
    } else if paragraph.numbered {
        (BlockKind::ListItem, 0)
    } else {
        (BlockKind::Paragraph, 0)
    }
}

fn render_table(rows: &[Vec<String>]) -> String {
    if rows.is_empty() {
        return String::new();
    }
    let columns = rows.iter().map(Vec::len).max().unwrap_or(0);
    let mut source = String::new();
    append_row(&mut source, &rows[0], columns);
    append_row(&mut source, &vec!["---".to_owned(); columns], columns);
    for row in &rows[1..] {
        append_row(&mut source, row, columns);
    }
    source.trim_end().to_owned()
}

fn append_row(source: &mut String, row: &[String], columns: usize) {
    source.push('|');
    for column in 0..columns {
        source.push(' ');
        source.push_str(row.get(column).map(String::as_str).unwrap_or(""));
        source.push_str(" |");
    }
    source.push('\n');
}

fn check_depth(depth: usize) -> Result<(), DocumentError> {
    if depth > MAX_NESTING_DEPTH {
        Err(DocumentError::NestingLimitExceeded)
    } else {
        Ok(())
    }
}

fn local_name(name: &[u8]) -> &[u8] {
    name.rsplit(|byte| *byte == b':').next().unwrap_or(name)
}
