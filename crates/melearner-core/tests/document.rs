#[path = "../src/document.rs"]
mod document;

use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use document::{
    BlockKind, DocumentError, DocumentFormat, DocumentWarning, DocumentWarningCode,
    MAX_BLOCK_SOURCE_BYTES, MAX_SOURCE_BYTES, normalize_document,
};
use zip::CompressionMethod;
use zip::write::{SimpleFileOptions, ZipWriter};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

struct RepoFixtureDir(PathBuf);

impl RepoFixtureDir {
    fn new(test_name: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../.tmp/document-tests")
            .join(format!("{}-{}-{sequence}", std::process::id(), test_name));
        fs::create_dir_all(&path).expect("create fixture directory");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for RepoFixtureDir {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).expect("remove fixture directory");
        if let Some(parent) = self.0.parent() {
            let _ = fs::remove_dir(parent);
        }
    }
}

fn write_docx(path: &Path, entries: &[(&str, &[u8])]) {
    let file = File::create(path).expect("create DOCX fixture");
    let mut zip = ZipWriter::new(file);
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
    for (name, contents) in entries {
        zip.start_file(*name, options).expect("start DOCX entry");
        zip.write_all(contents).expect("write DOCX entry");
    }
    zip.finish().expect("finish DOCX fixture");
}

#[test]
fn text_is_normalized_into_selectable_bounded_blocks() {
    let document = normalize_document(
        DocumentFormat::Text,
        b"\xef\xbb\xbfFirst paragraph.\r\n \r\nSecond paragraph.",
    )
    .expect("normalize text");

    assert_eq!(document.format, DocumentFormat::Text);
    assert_eq!(document.warnings, []);
    assert_eq!(document.blocks.len(), 2);
    assert_eq!(document.blocks[0].id, 0);
    assert_eq!(document.blocks[0].kind, BlockKind::Text);
    assert_eq!(document.blocks[0].source, "First paragraph.");
    assert_eq!(document.blocks[1].id, 1);
    assert_eq!(document.blocks[1].source, "Second paragraph.");
}

#[test]
fn document_and_block_bounds_are_enforced() {
    assert_eq!(
        normalize_document(DocumentFormat::Text, &vec![b'a'; MAX_SOURCE_BYTES + 1]),
        Err(DocumentError::SourceTooLarge)
    );
    assert_eq!(
        normalize_document(DocumentFormat::Text, &[0xff]),
        Err(DocumentError::InvalidUtf8)
    );

    let long_paragraph = "é".repeat(MAX_BLOCK_SOURCE_BYTES / 2 + 1);
    let document = normalize_document(DocumentFormat::Text, long_paragraph.as_bytes())
        .expect("split long text block");
    assert_eq!(document.blocks.len(), 2);
    assert!(
        document
            .blocks
            .iter()
            .all(|block| block.source.len() <= MAX_BLOCK_SOURCE_BYTES)
    );

    let too_many_blocks = "a\n\n".repeat(8_193);
    assert_eq!(
        normalize_document(DocumentFormat::Text, too_many_blocks.as_bytes()),
        Err(DocumentError::TooManyBlocks)
    );
}

#[test]
fn markup_nesting_is_bounded() {
    let markdown = format!("{}deep", "> ".repeat(129));
    assert_eq!(
        normalize_document(DocumentFormat::Markdown, markdown.as_bytes()),
        Err(DocumentError::NestingLimitExceeded)
    );

    let html = format!("{}deep{}", "<div>".repeat(129), "</div>".repeat(129));
    assert_eq!(
        normalize_document(DocumentFormat::Html, html.as_bytes()),
        Err(DocumentError::NestingLimitExceeded)
    );
}

#[test]
fn markdown_is_normalized_and_unsafe_embeds_are_omitted() {
    let document = normalize_document(
        DocumentFormat::Markdown,
        br#"# Heading

Paragraph with **emphasis** and [a link](https://example.com).

- first
- second

> quoted

```rust
let answer = 42;
```

| Key | Value |
| --- | --- |
| A | B |

![remote image](https://example.com/image.png)

<script>alert("no")</script>
"#,
    )
    .expect("normalize markdown");

    assert_eq!(document.format, DocumentFormat::Markdown);
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::Heading)
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::ListItem)
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::Quote)
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::Code)
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::Table)
    );
    assert!(
        document
            .blocks
            .iter()
            .all(|block| !block.source.contains("<script>"))
    );
    assert!(
        document
            .blocks
            .iter()
            .all(|block| !block.source.contains("image.png"))
    );
    assert_eq!(
        document.warnings,
        [
            DocumentWarning {
                code: DocumentWarningCode::MarkdownHtmlOmitted,
                count: 1,
            },
            DocumentWarning {
                code: DocumentWarningCode::MarkdownImageOmitted,
                count: 1,
            },
        ]
    );
}

#[test]
fn markdown_javascript_links_are_reduced_to_plain_text() {
    let document = normalize_document(
        DocumentFormat::Markdown,
        b"[safe label](javascript:alert('no'))",
    )
    .expect("normalize unsafe Markdown link");

    assert_eq!(document.blocks[0].source, "safe label");
    assert_eq!(
        document.warnings,
        [DocumentWarning {
            code: DocumentWarningCode::MarkdownUnsafeLinkOmitted,
            count: 1,
        }]
    );
}

#[test]
fn html_is_sanitized_into_supported_blocks_without_active_content() {
    let document = normalize_document(
        DocumentFormat::Html,
        br#"<!doctype html>
<html>
  <head>
    <style>body { display: none }</style>
    <script>alert("no")</script>
  </head>
  <body>
    <h2>Safe heading</h2>
    <p onclick="alert('no')">Text with <strong>emphasis</strong>.</p>
    <p><a href="javascript:alert('no')">unsafe link text</a></p>
    <ul><li>first</li><li>second</li></ul>
    <table><tr><th>Key</th><th>Value</th></tr><tr><td>A</td><td>B</td></tr></table>
    <img src="https://example.com/tracker.png">
    <marquee>unsupported but readable</marquee>
  </body>
</html>"#,
    )
    .expect("normalize html");

    assert_eq!(document.format, DocumentFormat::Html);
    assert!(document.blocks.iter().any(|block| {
        block.kind == BlockKind::Heading && block.level == 2 && block.source == "Safe heading"
    }));
    assert!(document.blocks.iter().any(|block| {
        block.kind == BlockKind::Paragraph && block.source.contains("Text with emphasis")
    }));
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::ListItem)
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| block.kind == BlockKind::Table)
    );
    let normalized = document
        .blocks
        .iter()
        .map(|block| block.source.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    assert!(normalized.contains("unsafe link text"));
    assert!(normalized.contains("unsupported but readable"));
    assert!(!normalized.contains("alert"));
    assert!(!normalized.contains("tracker.png"));
    assert_eq!(
        document
            .warnings
            .iter()
            .map(|warning| warning.code)
            .collect::<Vec<_>>(),
        [
            DocumentWarningCode::HtmlActiveContentOmitted,
            DocumentWarningCode::HtmlCssOmitted,
            DocumentWarningCode::HtmlRemoteResourceOmitted,
            DocumentWarningCode::HtmlUnsupportedConstructOmitted,
        ]
    );
}

#[test]
fn docx_is_normalized_from_bounded_zip_and_xml_parts() {
    let fixtures = RepoFixtureDir::new("docx-content");
    let path = fixtures.path().join("content.docx");
    let document_xml = br#"<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
 <w:body>
  <w:p><w:pPr><w:pStyle w:val="Heading2"/></w:pPr><w:r><w:t>Chapter</w:t></w:r></w:p>
  <w:p><w:r><w:t>Hello </w:t></w:r><w:r><w:t>world.</w:t></w:r></w:p>
  <w:p><w:pPr><w:numPr><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>First item</w:t></w:r></w:p>
  <w:tbl>
   <w:tr><w:tc><w:p><w:r><w:t>Key</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Value</w:t></w:r></w:p></w:tc></w:tr>
   <w:tr><w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>B</w:t></w:r></w:p></w:tc></w:tr>
  </w:tbl>
  <w:p><w:r><w:drawing><w:t>hidden image text</w:t></w:drawing></w:r></w:p>
  <w:altChunk r:id="external"><w:p><w:r><w:t>hidden imported text</w:t></w:r></w:p></w:altChunk>
 </w:body>
</w:document>"#;
    let relationships = br#"<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Id="external" TargetMode="External" Target="https://example.com/content"/>
</Relationships>"#;
    write_docx(
        &path,
        &[
            ("word/document.xml", document_xml),
            ("word/_rels/document.xml.rels", relationships),
        ],
    );

    let bytes = fs::read(&path).expect("read DOCX fixture");
    let document = normalize_document(DocumentFormat::Docx, &bytes).expect("normalize DOCX");

    assert!(document.blocks.iter().any(|block| {
        block.kind == BlockKind::Heading && block.level == 2 && block.source == "Chapter"
    }));
    assert!(
        document
            .blocks
            .iter()
            .any(|block| { block.kind == BlockKind::Paragraph && block.source == "Hello world." })
    );
    assert!(
        document
            .blocks
            .iter()
            .any(|block| { block.kind == BlockKind::ListItem && block.source == "First item" })
    );
    assert!(document.blocks.iter().any(|block| {
        block.kind == BlockKind::Table
            && block.source.contains("| Key | Value |")
            && block.source.contains("| A | B |")
    }));
    assert!(
        document
            .blocks
            .iter()
            .all(|block| !block.source.contains("hidden"))
    );
    assert_eq!(
        document
            .warnings
            .iter()
            .map(|warning| warning.code)
            .collect::<Vec<_>>(),
        [
            DocumentWarningCode::DocxEmbeddedImageOmitted,
            DocumentWarningCode::DocxExternalRelationshipOmitted,
            DocumentWarningCode::DocxUnsupportedConstructOmitted,
        ]
    );
}

#[test]
fn docx_rejects_archive_entry_limit_and_unsafe_paths() {
    let fixtures = RepoFixtureDir::new("docx-archive-limits");
    let entries_path = fixtures.path().join("entries.docx");
    let names = (0..257)
        .map(|index| format!("word/item-{index}.xml"))
        .collect::<Vec<_>>();
    let entries = names
        .iter()
        .map(|name| (name.as_str(), b"" as &[u8]))
        .collect::<Vec<_>>();
    write_docx(&entries_path, &entries);
    assert_eq!(
        normalize_document(
            DocumentFormat::Docx,
            &fs::read(entries_path).expect("read entry-limit fixture")
        ),
        Err(DocumentError::ArchiveLimitExceeded)
    );

    let unsafe_path = fixtures.path().join("unsafe-path.docx");
    write_docx(
        &unsafe_path,
        &[
            ("word/document.xml", b"<w:document/>"),
            ("../outside.xml", b"outside"),
        ],
    );
    assert_eq!(
        normalize_document(
            DocumentFormat::Docx,
            &fs::read(unsafe_path).expect("read unsafe-path fixture")
        ),
        Err(DocumentError::InvalidDocx)
    );
}

#[test]
fn docx_rejects_xml_size_and_nesting_limits() {
    let fixtures = RepoFixtureDir::new("docx-xml-limits");
    let oversized_path = fixtures.path().join("oversized-xml.docx");
    let oversized_xml = vec![b' '; 4 * 1024 * 1024 + 1];
    write_docx(&oversized_path, &[("word/document.xml", &oversized_xml)]);
    assert_eq!(
        normalize_document(
            DocumentFormat::Docx,
            &fs::read(oversized_path).expect("read XML-size fixture")
        ),
        Err(DocumentError::XmlLimitExceeded)
    );

    let nesting_path = fixtures.path().join("nested.docx");
    let nested_xml = format!(
        "<w:document>{}<w:t>deep</w:t>{}</w:document>",
        "<w:sdt>".repeat(129),
        "</w:sdt>".repeat(129)
    );
    write_docx(
        &nesting_path,
        &[("word/document.xml", nested_xml.as_bytes())],
    );
    assert_eq!(
        normalize_document(
            DocumentFormat::Docx,
            &fs::read(nesting_path).expect("read nesting fixture")
        ),
        Err(DocumentError::NestingLimitExceeded)
    );
}
