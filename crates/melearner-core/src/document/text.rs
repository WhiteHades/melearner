use super::{
    BlockKind, DocumentBuilder, DocumentError, DocumentFormat, MAX_BLOCK_SOURCE_BYTES,
    NormalizedDocument,
};

pub(super) fn normalize(source: &[u8]) -> Result<NormalizedDocument, DocumentError> {
    let text = std::str::from_utf8(source).map_err(|_| DocumentError::InvalidUtf8)?;
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut builder = DocumentBuilder::new(DocumentFormat::Text);
    let normalized = text.replace("\r\n", "\n");
    let mut paragraph = String::new();

    for line in normalized.split('\n') {
        if line.trim().is_empty() {
            push_chunks(&mut builder, paragraph.trim())?;
            paragraph.clear();
        } else {
            if !paragraph.is_empty() {
                paragraph.push('\n');
            }
            paragraph.push_str(line);
        }
    }
    push_chunks(&mut builder, paragraph.trim())?;

    Ok(builder.finish())
}

fn push_chunks(builder: &mut DocumentBuilder, mut source: &str) -> Result<(), DocumentError> {
    while !source.is_empty() {
        if source.len() <= MAX_BLOCK_SOURCE_BYTES {
            return builder.push(BlockKind::Text, 0, source.to_owned());
        }

        let mut boundary = MAX_BLOCK_SOURCE_BYTES;
        while !source.is_char_boundary(boundary) {
            boundary -= 1;
        }
        if let Some(split) = source[..boundary].rfind(char::is_whitespace)
            && split != 0
        {
            boundary = split;
        }
        builder.push(BlockKind::Text, 0, source[..boundary].trim_end().to_owned())?;
        source = source[boundary..].trim_start();
    }

    Ok(())
}
