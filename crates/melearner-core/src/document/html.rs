use html5ever::{parse_document, tendril::TendrilSink};
use markup5ever_rcdom::{Handle, NodeData, RcDom};

use super::{
    BlockKind, DocumentBuilder, DocumentError, DocumentFormat, DocumentWarningCode,
    MAX_NESTING_DEPTH, NormalizedDocument,
};

pub(super) fn normalize(source: &[u8]) -> Result<NormalizedDocument, DocumentError> {
    let source = std::str::from_utf8(source).map_err(|_| DocumentError::InvalidUtf8)?;
    let dom = parse_document(RcDom::default(), Default::default()).one(source);
    let mut builder = DocumentBuilder::new(DocumentFormat::Html);
    walk(&dom.document, &mut builder, 0)?;
    Ok(builder.finish())
}

fn walk(handle: &Handle, builder: &mut DocumentBuilder, depth: usize) -> Result<(), DocumentError> {
    if depth > MAX_NESTING_DEPTH {
        return Err(DocumentError::NestingLimitExceeded);
    }
    let NodeData::Element { name, attrs, .. } = &handle.data else {
        for child in handle.children.borrow().iter() {
            walk(child, builder, depth + 1)?;
        }
        return Ok(());
    };
    let tag = name.local.as_ref();
    inspect_attributes(attrs, builder);

    match tag {
        "script" | "iframe" | "object" | "embed" | "form" | "input" | "button" | "audio"
        | "video" | "source" | "canvas" | "svg" => {
            builder.warn(DocumentWarningCode::HtmlActiveContentOmitted);
        }
        "style" => builder.warn(DocumentWarningCode::HtmlCssOmitted),
        "img" | "link" | "meta" => {
            builder.warn(DocumentWarningCode::HtmlUnsupportedConstructOmitted);
        }
        "h1" | "h2" | "h3" | "h4" | "h5" | "h6" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Heading, tag[1..].parse().unwrap_or(0), source)?;
        }
        "p" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Paragraph, 0, source)?;
        }
        "li" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::ListItem, 0, source)?;
        }
        "blockquote" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Quote, 0, source)?;
        }
        "pre" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Code, 0, source)?;
        }
        "table" => {
            let source = table_source(handle, builder, depth + 1)?;
            builder.push(BlockKind::Table, 0, source)?;
        }
        "hr" => builder.push(BlockKind::Rule, 0, "---".to_owned())?,
        "html" | "head" | "body" | "main" | "article" | "section" | "div" | "header" | "footer"
        | "nav" | "ul" | "ol" => {
            for child in handle.children.borrow().iter() {
                walk(child, builder, depth + 1)?;
            }
        }
        "title" | "base" | "noscript" | "template" => {}
        "a" | "abbr" | "b" | "br" | "cite" | "code" | "del" | "em" | "i" | "kbd" | "mark" | "q"
        | "s" | "samp" | "small" | "span" | "strong" | "sub" | "sup" | "time" | "u" | "var" => {
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Paragraph, 0, source)?;
        }
        _ => {
            builder.warn(DocumentWarningCode::HtmlUnsupportedConstructOmitted);
            let source = node_text(handle, builder, depth + 1)?;
            builder.push(BlockKind::Paragraph, 0, source)?;
        }
    }
    Ok(())
}

fn node_text(
    handle: &Handle,
    builder: &mut DocumentBuilder,
    depth: usize,
) -> Result<String, DocumentError> {
    let mut source = String::new();
    for child in handle.children.borrow().iter() {
        collect_text(child, builder, depth, &mut source)?;
    }
    Ok(collapse_whitespace(&source))
}

fn collect_text(
    handle: &Handle,
    builder: &mut DocumentBuilder,
    depth: usize,
    source: &mut String,
) -> Result<(), DocumentError> {
    if depth > MAX_NESTING_DEPTH {
        return Err(DocumentError::NestingLimitExceeded);
    }
    match &handle.data {
        NodeData::Text { contents } => source.push_str(&contents.borrow()),
        NodeData::Element { name, attrs, .. } => {
            let tag = name.local.as_ref();
            inspect_attributes(attrs, builder);
            match tag {
                "script" | "iframe" | "object" | "embed" | "form" | "input" | "button"
                | "audio" | "video" | "source" | "canvas" | "svg" => {
                    builder.warn(DocumentWarningCode::HtmlActiveContentOmitted);
                    return Ok(());
                }
                "style" => {
                    builder.warn(DocumentWarningCode::HtmlCssOmitted);
                    return Ok(());
                }
                "img" | "link" | "meta" => {
                    builder.warn(DocumentWarningCode::HtmlUnsupportedConstructOmitted);
                    return Ok(());
                }
                "br" => source.push('\n'),
                _ => {}
            }
            for child in handle.children.borrow().iter() {
                collect_text(child, builder, depth + 1, source)?;
            }
        }
        _ => {
            for child in handle.children.borrow().iter() {
                collect_text(child, builder, depth + 1, source)?;
            }
        }
    }
    Ok(())
}

fn inspect_attributes(
    attrs: &std::cell::RefCell<Vec<html5ever::Attribute>>,
    builder: &mut DocumentBuilder,
) {
    for attribute in attrs.borrow().iter() {
        let name = attribute.name.local.as_ref();
        let value = attribute.value.as_ref();
        if name.starts_with("on") || has_unsafe_scheme(value) {
            builder.warn(DocumentWarningCode::HtmlActiveContentOmitted);
        } else if name == "style" {
            builder.warn(DocumentWarningCode::HtmlCssOmitted);
        } else if matches!(name, "src" | "href") && is_remote(value) {
            builder.warn(DocumentWarningCode::HtmlRemoteResourceOmitted);
        }
    }
}

fn table_source(
    handle: &Handle,
    builder: &mut DocumentBuilder,
    depth: usize,
) -> Result<String, DocumentError> {
    let mut rows = Vec::new();
    collect_rows(handle, builder, depth, &mut rows)?;
    if rows.is_empty() {
        return Ok(String::new());
    }
    let columns = rows.iter().map(Vec::len).max().unwrap_or(0);
    let mut source = String::new();
    append_row(&mut source, &rows[0], columns);
    append_row(&mut source, &vec!["---".to_owned(); columns], columns);
    for row in &rows[1..] {
        append_row(&mut source, row, columns);
    }
    Ok(source.trim_end().to_owned())
}

fn collect_rows(
    handle: &Handle,
    builder: &mut DocumentBuilder,
    depth: usize,
    rows: &mut Vec<Vec<String>>,
) -> Result<(), DocumentError> {
    if depth > MAX_NESTING_DEPTH {
        return Err(DocumentError::NestingLimitExceeded);
    }
    if let NodeData::Element { name, .. } = &handle.data
        && name.local.as_ref() == "tr"
    {
        let mut row = Vec::new();
        for child in handle.children.borrow().iter() {
            if let NodeData::Element { name, .. } = &child.data
                && matches!(name.local.as_ref(), "th" | "td")
            {
                row.push(node_text(child, builder, depth + 1)?);
            }
        }
        rows.push(row);
        return Ok(());
    }
    for child in handle.children.borrow().iter() {
        collect_rows(child, builder, depth + 1, rows)?;
    }
    Ok(())
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

fn collapse_whitespace(source: &str) -> String {
    source.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn is_remote(value: &str) -> bool {
    let value = value.trim_start().to_ascii_lowercase();
    value.starts_with("http://") || value.starts_with("https://") || value.starts_with("//")
}

fn has_unsafe_scheme(value: &str) -> bool {
    let value = value.trim_start().to_ascii_lowercase();
    value.starts_with("javascript:") || value.starts_with("data:") || value.starts_with("vbscript:")
}
