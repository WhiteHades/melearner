#[path = "document/docx.rs"]
mod docx;
#[path = "document/html.rs"]
mod html;
#[path = "document/markdown.rs"]
mod markdown;
#[path = "document/text.rs"]
mod text;

use std::collections::BTreeMap;

use serde::Serialize;

pub(crate) const MAX_SOURCE_BYTES: usize = 16 * 1024 * 1024;
pub(crate) const MAX_BLOCK_SOURCE_BYTES: usize = 16 * 1024;
pub(crate) const MAX_BLOCKS: usize = 8_192;
pub(crate) const MAX_NESTING_DEPTH: usize = 128;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum DocumentFormat {
    Text,
    Markdown,
    Html,
    Docx,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum BlockKind {
    Text,
    Paragraph,
    Heading,
    ListItem,
    Quote,
    Code,
    Table,
    Rule,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DocumentBlock {
    pub(crate) id: u64,
    pub(crate) kind: BlockKind,
    pub(crate) level: u8,
    pub(crate) source: String,
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd, Serialize)]
#[allow(clippy::enum_variant_names)]
#[serde(rename_all = "camelCase")]
pub(crate) enum DocumentWarningCode {
    MarkdownHtmlOmitted,
    MarkdownImageOmitted,
    MarkdownUnsafeLinkOmitted,
    HtmlActiveContentOmitted,
    HtmlCssOmitted,
    HtmlRemoteResourceOmitted,
    HtmlUnsupportedConstructOmitted,
    DocxEmbeddedImageOmitted,
    DocxExternalRelationshipOmitted,
    DocxUnsupportedConstructOmitted,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DocumentWarning {
    pub(crate) code: DocumentWarningCode,
    pub(crate) count: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct NormalizedDocument {
    pub(crate) format: DocumentFormat,
    pub(crate) blocks: Vec<DocumentBlock>,
    pub(crate) warnings: Vec<DocumentWarning>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum DocumentError {
    InvalidUtf8,
    InvalidMarkdown,
    SourceTooLarge,
    BlockTooLarge,
    TooManyBlocks,
    InvalidDocx,
    ArchiveLimitExceeded,
    XmlLimitExceeded,
    NestingLimitExceeded,
}

pub(super) struct DocumentBuilder {
    format: DocumentFormat,
    blocks: Vec<DocumentBlock>,
    warnings: BTreeMap<DocumentWarningCode, u32>,
}

impl DocumentBuilder {
    pub(super) fn new(format: DocumentFormat) -> Self {
        Self {
            format,
            blocks: Vec::new(),
            warnings: BTreeMap::new(),
        }
    }

    pub(super) fn push(
        &mut self,
        kind: BlockKind,
        level: u8,
        source: String,
    ) -> Result<(), DocumentError> {
        if source.is_empty() {
            return Ok(());
        }
        if source.len() > MAX_BLOCK_SOURCE_BYTES {
            return Err(DocumentError::BlockTooLarge);
        }
        if self.blocks.len() == MAX_BLOCKS {
            return Err(DocumentError::TooManyBlocks);
        }
        self.blocks.push(DocumentBlock {
            id: self.blocks.len() as u64,
            kind,
            level,
            source,
        });
        Ok(())
    }

    pub(super) fn warn(&mut self, code: DocumentWarningCode) {
        let count = self.warnings.entry(code).or_default();
        *count = count.saturating_add(1);
    }

    pub(super) fn finish(self) -> NormalizedDocument {
        NormalizedDocument {
            format: self.format,
            blocks: self.blocks,
            warnings: self
                .warnings
                .into_iter()
                .map(|(code, count)| DocumentWarning { code, count })
                .collect(),
        }
    }
}

pub(crate) fn normalize_document(
    format: DocumentFormat,
    source: &[u8],
) -> Result<NormalizedDocument, DocumentError> {
    if source.len() > MAX_SOURCE_BYTES {
        return Err(DocumentError::SourceTooLarge);
    }
    match format {
        DocumentFormat::Text => text::normalize(source),
        DocumentFormat::Markdown => markdown::normalize(source),
        DocumentFormat::Html => html::normalize(source),
        DocumentFormat::Docx => docx::normalize(source),
    }
}
