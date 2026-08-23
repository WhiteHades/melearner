use pulldown_cmark::{Event, HeadingLevel, Options, Parser, Tag, TagEnd};
use pulldown_cmark_to_cmark::cmark;

use super::{
    BlockKind, DocumentBuilder, DocumentError, DocumentFormat, DocumentWarningCode,
    MAX_NESTING_DEPTH, NormalizedDocument,
};

pub(super) fn normalize(source: &[u8]) -> Result<NormalizedDocument, DocumentError> {
    let source = std::str::from_utf8(source).map_err(|_| DocumentError::InvalidUtf8)?;
    let options = Options::ENABLE_GFM
        | Options::ENABLE_STRIKETHROUGH
        | Options::ENABLE_TABLES
        | Options::ENABLE_TASKLISTS;
    let mut builder = DocumentBuilder::new(DocumentFormat::Markdown);
    let mut events = Vec::new();
    let mut kind = BlockKind::Paragraph;
    let mut level = 0;
    let mut depth = 0_usize;
    let mut omitted_image_depth = 0_usize;
    let mut omitted_html_depth = 0_usize;
    let mut omitted_link_depth = 0_usize;

    for event in Parser::new_ext(source, options) {
        if omitted_html_depth != 0 {
            match event {
                Event::Start(_) => omitted_html_depth += 1,
                Event::End(TagEnd::HtmlBlock) if omitted_html_depth == 1 => {
                    omitted_html_depth = 0;
                }
                Event::End(_) => omitted_html_depth -= 1,
                _ => {}
            }
            continue;
        }
        if omitted_image_depth != 0 {
            match event {
                Event::Start(_) => omitted_image_depth += 1,
                Event::End(TagEnd::Image) if omitted_image_depth == 1 => {
                    omitted_image_depth = 0;
                }
                Event::End(_) => omitted_image_depth -= 1,
                _ => {}
            }
            continue;
        }
        if omitted_link_depth != 0 {
            match event {
                Event::Start(_) => omitted_link_depth += 1,
                Event::End(TagEnd::Link) if omitted_link_depth == 1 => {
                    omitted_link_depth = 0;
                }
                Event::End(_) => omitted_link_depth -= 1,
                Event::Text(_) | Event::Code(_) | Event::SoftBreak | Event::HardBreak => {
                    events.push(event.into_static());
                }
                _ => {}
            }
            continue;
        }

        match event {
            Event::Start(Tag::HtmlBlock) => {
                builder.warn(DocumentWarningCode::MarkdownHtmlOmitted);
                omitted_html_depth = 1;
            }
            Event::Start(Tag::Image { .. }) => {
                builder.warn(DocumentWarningCode::MarkdownImageOmitted);
                omitted_image_depth = 1;
            }
            Event::Start(Tag::Link { ref dest_url, .. }) if has_unsafe_scheme(dest_url) => {
                builder.warn(DocumentWarningCode::MarkdownUnsafeLinkOmitted);
                omitted_link_depth = 1;
            }
            Event::Html(_) | Event::InlineHtml(_) => {
                builder.warn(DocumentWarningCode::MarkdownHtmlOmitted);
            }
            Event::Start(tag) => {
                if depth == 0 {
                    (kind, level) = block_kind(&tag);
                }
                depth += 1;
                if depth > MAX_NESTING_DEPTH {
                    return Err(DocumentError::NestingLimitExceeded);
                }
                events.push(Event::Start(tag.into_static()));
            }
            Event::End(tag) => {
                events.push(Event::End(tag));
                depth = depth.checked_sub(1).ok_or(DocumentError::InvalidMarkdown)?;
                if depth == 0 {
                    flush(&mut builder, kind, level, &mut events)?;
                }
            }
            Event::Rule => {
                flush(&mut builder, kind, level, &mut events)?;
                builder.push(BlockKind::Rule, 0, "---".to_owned())?;
            }
            event => {
                events.push(event.into_static());
                if depth == 0 {
                    flush(&mut builder, BlockKind::Paragraph, 0, &mut events)?;
                }
            }
        }
    }

    if depth != 0 {
        return Err(DocumentError::InvalidMarkdown);
    }
    flush(&mut builder, kind, level, &mut events)?;
    Ok(builder.finish())
}

fn flush(
    builder: &mut DocumentBuilder,
    kind: BlockKind,
    level: u8,
    events: &mut Vec<Event<'static>>,
) -> Result<(), DocumentError> {
    if events.is_empty() {
        return Ok(());
    }
    let mut source = String::new();
    cmark(events.drain(..), &mut source).map_err(|_| DocumentError::InvalidMarkdown)?;
    builder.push(kind, level, source.trim().to_owned())
}

fn block_kind(tag: &Tag<'_>) -> (BlockKind, u8) {
    match tag {
        Tag::Heading { level, .. } => (BlockKind::Heading, heading_level(*level)),
        Tag::List(_) | Tag::Item => (BlockKind::ListItem, 0),
        Tag::BlockQuote(_) => (BlockKind::Quote, 0),
        Tag::CodeBlock(_) => (BlockKind::Code, 0),
        Tag::Table(_) => (BlockKind::Table, 0),
        _ => (BlockKind::Paragraph, 0),
    }
}

fn heading_level(level: HeadingLevel) -> u8 {
    match level {
        HeadingLevel::H1 => 1,
        HeadingLevel::H2 => 2,
        HeadingLevel::H3 => 3,
        HeadingLevel::H4 => 4,
        HeadingLevel::H5 => 5,
        HeadingLevel::H6 => 6,
    }
}

fn has_unsafe_scheme(url: &str) -> bool {
    let normalized = url.trim_start().to_ascii_lowercase();
    normalized.starts_with("javascript:")
        || normalized.starts_with("data:")
        || normalized.starts_with("vbscript:")
}
