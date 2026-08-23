use std::io::Read;
use std::path::{Path, PathBuf};

use cap_std::ambient_authority;
use cap_std::fs::{Dir as CapabilityDir, File as CapabilityFile};
use serde::Serialize;
use sqlx::Row;

use super::{LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE};
use crate::document::{
    DocumentBlock, DocumentFormat, DocumentWarning, MAX_SOURCE_BYTES, NormalizedDocument,
    normalize_document,
};

#[derive(Debug)]
pub(crate) struct DocumentOpenInput {
    pub(crate) expected_revision: u64,
    pub(crate) lesson_id: String,
}

#[derive(Debug)]
pub(crate) struct DocumentPageInput {
    pub(crate) expected_revision: u64,
    pub(crate) document_id: String,
    pub(crate) offset: u64,
    pub(crate) limit: u32,
}

#[derive(Debug)]
pub(crate) struct DocumentExternalOpenInput {
    pub(crate) expected_revision: u64,
    pub(crate) lesson_id: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DocumentOpened {
    pub(crate) revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) document_id: String,
    pub(crate) format: DocumentFormat,
    pub(crate) total_blocks: u64,
    pub(crate) warnings: Vec<DocumentWarning>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DocumentPage {
    pub(crate) revision: u64,
    pub(crate) document_id: String,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) blocks: Vec<DocumentBlock>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DocumentExternalOpenReady {
    pub(crate) revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) canonical_path: String,
}

pub(super) struct ActiveDocument {
    revision: u64,
    id: String,
    document: NormalizedDocument,
}

struct ResolvedDocument {
    canonical_path: PathBuf,
    file: CapabilityFile,
}

impl LibraryDatabase {
    pub(crate) async fn open_document(
        &mut self,
        input: DocumentOpenInput,
        max_payload_bytes: usize,
    ) -> Result<DocumentOpened, LibraryError> {
        let resolved = self
            .resolve_document(input.expected_revision, &input.lesson_id)
            .await?;
        let format = document_format(&resolved.canonical_path)?;
        let mut file = resolved.file;
        let file_size = file
            .metadata()
            .map_err(|_| LibraryError::DocumentUnavailable)?
            .len();
        if file_size > MAX_SOURCE_BYTES as u64 {
            return Err(LibraryError::DocumentDecodeFailed);
        }
        let mut source = Vec::with_capacity(file_size as usize);
        file.by_ref()
            .take(MAX_SOURCE_BYTES as u64 + 1)
            .read_to_end(&mut source)
            .map_err(|_| LibraryError::DocumentUnavailable)?;
        if source.len() > MAX_SOURCE_BYTES {
            return Err(LibraryError::DocumentDecodeFailed);
        }
        let document =
            normalize_document(format, &source).map_err(|_| LibraryError::DocumentDecodeFailed)?;
        let total_blocks =
            u64::try_from(document.blocks.len()).map_err(|_| LibraryError::DocumentDecodeFailed)?;
        let opened = DocumentOpened {
            revision: self.revision,
            lesson_id: input.lesson_id.clone(),
            document_id: input.lesson_id.clone(),
            format,
            total_blocks,
            warnings: document.warnings.clone(),
        };
        require_response_fits(&opened, max_payload_bytes)?;
        self.active_document = Some(ActiveDocument {
            revision: self.revision,
            id: input.lesson_id,
            document,
        });
        Ok(opened)
    }

    pub(crate) fn document_page(
        &self,
        input: DocumentPageInput,
        max_payload_bytes: usize,
    ) -> Result<DocumentPage, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.document_id.is_empty() || input.document_id.contains('\0') {
            return Err(LibraryError::InvalidDocument);
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&input.limit) {
            return Err(LibraryError::InvalidPageSize { limit: input.limit });
        }
        let offset = usize::try_from(input.offset).map_err(|_| LibraryError::InvalidOffset {
            offset: input.offset,
        })?;
        let active = self
            .active_document
            .as_ref()
            .filter(|active| active.revision == self.revision && active.id == input.document_id)
            .ok_or(LibraryError::DocumentNotOpen)?;
        let start = offset.min(active.document.blocks.len());
        let end = start
            .saturating_add(input.limit as usize)
            .min(active.document.blocks.len());
        let page = DocumentPage {
            revision: self.revision,
            document_id: input.document_id,
            offset: input.offset,
            total: active.document.blocks.len() as u64,
            blocks: active.document.blocks[start..end].to_vec(),
        };
        require_response_fits(&page, max_payload_bytes)?;
        Ok(page)
    }

    pub(crate) async fn document_external_open(
        &mut self,
        input: DocumentExternalOpenInput,
        max_payload_bytes: usize,
    ) -> Result<DocumentExternalOpenReady, LibraryError> {
        let resolved = self
            .resolve_document(input.expected_revision, &input.lesson_id)
            .await?;
        let canonical_path = resolved
            .canonical_path
            .to_str()
            .ok_or(LibraryError::DocumentUnavailable)?
            .to_owned();
        let ready = DocumentExternalOpenReady {
            revision: self.revision,
            lesson_id: input.lesson_id,
            canonical_path,
        };
        require_response_fits(&ready, max_payload_bytes)?;
        Ok(ready)
    }

    async fn resolve_document(
        &mut self,
        expected_revision: u64,
        lesson_id: &str,
    ) -> Result<ResolvedDocument, LibraryError> {
        self.require_revision(expected_revision)?;
        if lesson_id.is_empty() || lesson_id.contains('\0') {
            return Err(LibraryError::InvalidDocument);
        }
        let record = sqlx::query(
            "SELECT lessons.path, lessons.type, courses.missing_since
             FROM lessons
             INNER JOIN courses ON courses.id = lessons.course_id
             WHERE lessons.id = ?1",
        )
        .bind(lesson_id)
        .fetch_optional(&mut self.connection)
        .await?
        .ok_or(LibraryError::LessonNotFound)?;
        if record.try_get::<String, _>("type")? != "document" {
            return Err(LibraryError::LessonNotFound);
        }
        if record
            .try_get::<Option<String>, _>("missing_since")?
            .is_some()
        {
            return Err(LibraryError::DocumentUnavailable);
        }
        let approved_root = self
            .library_path
            .as_deref()
            .filter(|root| !root.is_empty())
            .ok_or(LibraryError::DocumentUnavailable)?;
        let lesson_path = record.try_get::<String, _>("path")?;
        open_approved_document(approved_root, &lesson_path)
    }
}

fn open_approved_document(
    approved_root: &str,
    lesson_path: &str,
) -> Result<ResolvedDocument, LibraryError> {
    let approved_root_path = Path::new(approved_root);
    let requested_relative = Path::new(lesson_path)
        .strip_prefix(approved_root_path)
        .map_err(|_| LibraryError::DocumentUnavailable)?;
    if requested_relative.as_os_str().is_empty() {
        return Err(LibraryError::DocumentUnavailable);
    }
    let canonical_root =
        std::fs::canonicalize(approved_root_path).map_err(|_| LibraryError::DocumentUnavailable)?;
    let root = CapabilityDir::open_ambient_dir(&canonical_root, ambient_authority())
        .map_err(|_| LibraryError::DocumentUnavailable)?;
    let canonical_relative = root
        .canonicalize(requested_relative)
        .map_err(|_| LibraryError::DocumentUnavailable)?;
    if canonical_relative.is_absolute() {
        return Err(LibraryError::DocumentUnavailable);
    }
    let file = root
        .open(&canonical_relative)
        .map_err(|_| LibraryError::DocumentUnavailable)?;
    if !file
        .metadata()
        .map_err(|_| LibraryError::DocumentUnavailable)?
        .is_file()
    {
        return Err(LibraryError::DocumentUnavailable);
    }
    Ok(ResolvedDocument {
        canonical_path: canonical_root.join(canonical_relative),
        file,
    })
}

fn document_format(path: &Path) -> Result<DocumentFormat, LibraryError> {
    match path
        .extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase)
        .as_deref()
    {
        Some("txt") => Ok(DocumentFormat::Text),
        Some("md" | "markdown") => Ok(DocumentFormat::Markdown),
        Some("html" | "htm") => Ok(DocumentFormat::Html),
        Some("docx") => Ok(DocumentFormat::Docx),
        _ => Err(LibraryError::DocumentUnsupported),
    }
}

fn require_response_fits(
    value: &impl Serialize,
    max_payload_bytes: usize,
) -> Result<(), LibraryError> {
    if serde_json::to_vec(value).is_ok_and(|payload| payload.len() <= max_payload_bytes) {
        Ok(())
    } else {
        Err(LibraryError::ResponseTooLarge {
            limit: max_payload_bytes,
        })
    }
}
