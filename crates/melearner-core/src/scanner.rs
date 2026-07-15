use cap_std::fs::{Dir as CapabilityDir, OpenOptions as CapabilityOpenOptions};
use jwalk::WalkDir;
use seahash::SeaHasher;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::collections::HashSet;
use std::hash::Hasher;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::MutationControl;

const VIDEO_EXTENSIONS: &[&str] = &["mp4", "mkv", "webm", "mov", "avi", "m4v"];
const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "aac", "m4a", "flac", "ogg"];
const DOCUMENT_EXTENSIONS: &[&str] =
    &["pdf", "txt", "md", "markdown", "html", "htm", "docx", "doc"];
const SUBTITLE_EXTENSIONS: &[&str] = &["srt", "vtt"];
const IGNORED_FOLDERS: &[&str] = &[".git", "node_modules", "__MACOSX", ".DS_Store", "Thumbs.db"];
const PARTIAL_EXTENSIONS: &[&str] = &[
    "part",
    "partial",
    "crdownload",
    "download",
    "tmp",
    "temp",
    "aria2",
    "torrent",
    "!qb",
    "qb",
    "ut",
    "utpart",
    "bc!",
];
const PARTIAL_FILE_NAMES: &[&str] = &["desktop.ini", ".directory"];
const PARTIAL_FOLDER_NAMES: &[&str] = &[
    ".incomplete",
    ".parts",
    ".sync",
    ".stfolder",
    ".stversions",
    "incomplete",
    "temp",
    "tmp",
];
const DOWNLOAD_SIDECAR_SUFFIXES: &[&str] =
    &["aria2", "part", "partial", "crdownload", "download", "!qB"];
const WARNING_LIMIT: usize = 24;
const COURSE_MARKER_FILE_NAME: &str = ".melearner-course.json";
const COURSE_MARKER_VERSION: u8 = 1;
const COURSE_MARKER_MAX_BYTES: u64 = 4 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileEntry {
    pub id: Box<str>,
    pub path: Box<str>,
    pub relative_path: Box<str>,
    pub name: Box<str>,
    pub file_type: FileType,
    pub size: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum FileType {
    Video,
    Audio,
    Document,
    Subtitle,
    Quiz,
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SectionData {
    pub id: Box<str>,
    pub name: Box<str>,
    pub files: Box<[FileEntry]>,
    pub order: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CourseData {
    pub id: Box<str>,
    pub marker_identity_id: Option<Box<str>>,
    pub name: Box<str>,
    pub path: Box<str>,
    pub fingerprint: Box<str>,
    pub sections: Box<[SectionData]>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CourseMarker {
    version: u8,
    identity_id: Option<String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ScanType {
    Library,
    SingleCourse,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScanResult {
    pub scan_type: ScanType,
    pub courses: Box<[CourseData]>,
    pub warnings: Box<[String]>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ScanError {
    Cancelled,
    Invalid(String),
}

fn require_active(control: &MutationControl) -> Result<(), ScanError> {
    if control.is_cancelled() {
        Err(ScanError::Cancelled)
    } else {
        Ok(())
    }
}

fn new_hasher() -> SeaHasher {
    SeaHasher::with_seeds(
        0xD6E8FEB86659FD93,
        0xA5A3564E27F8862E,
        0x510E527FADE682D1,
        0x9B05688C2B3E6C1F,
    )
}

fn hash_str_to_id(s: &str) -> Box<str> {
    let mut h = new_hasher();
    h.write(s.as_bytes());
    format!("{:016x}", h.finish()).into()
}

fn hash_path_to_id(path: &Path) -> Box<str> {
    let mut h = new_hasher();
    h.write(path.to_string_lossy().as_bytes());
    format!("{:016x}", h.finish()).into()
}

fn normalized_relative_path(path: &Path, base: &Path) -> Box<str> {
    let relative = path.strip_prefix(base).unwrap_or(path);
    relative
        .components()
        .map(|component| component.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
        .into_boxed_str()
}

fn get_file_type(path: &Path) -> FileType {
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();

    if PARTIAL_EXTENSIONS.contains(&ext.as_str()) {
        return FileType::Unknown;
    }

    if VIDEO_EXTENSIONS.contains(&ext.as_str()) {
        return FileType::Video;
    }
    if AUDIO_EXTENSIONS.contains(&ext.as_str()) {
        return FileType::Audio;
    }
    if DOCUMENT_EXTENSIONS.contains(&ext.as_str()) {
        return FileType::Document;
    }
    if SUBTITLE_EXTENSIONS.contains(&ext.as_str()) {
        return FileType::Subtitle;
    }

    let name = path
        .file_stem()
        .and_then(|n| n.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();

    if name.contains("quiz") || name.contains("test") || name.contains("exam") {
        return FileType::Quiz;
    }

    FileType::Unknown
}

fn is_ignored(path: &Path) -> bool {
    let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    name.starts_with('.') || IGNORED_FOLDERS.contains(&name)
}

fn lower_name(path: &Path) -> String {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default()
}

fn is_partial_folder(path: &Path) -> bool {
    let name = lower_name(path);
    PARTIAL_FOLDER_NAMES.contains(&name.as_str())
}

fn is_partial_file(path: &Path) -> bool {
    let name = lower_name(path);
    if PARTIAL_FILE_NAMES.contains(&name.as_str()) {
        return true;
    }

    path.extension()
        .and_then(|e| e.to_str())
        .map(str::to_ascii_lowercase)
        .is_some_and(|ext| PARTIAL_EXTENSIONS.contains(&ext.as_str()))
}

fn download_sidecar_target(path: &Path) -> Option<PathBuf> {
    let path_text = path.to_string_lossy();
    for suffix in DOWNLOAD_SIDECAR_SUFFIXES {
        let marker = format!(".{suffix}");
        if path_text.ends_with(&marker) {
            let target = &path_text[..path_text.len() - marker.len()];
            return Some(PathBuf::from(target));
        }
    }
    None
}

fn download_sidecar_targets(
    paths: &[PathBuf],
    control: &MutationControl,
) -> Result<HashSet<PathBuf>, ScanError> {
    let mut targets = HashSet::new();
    for path in paths {
        require_active(control)?;
        if let Some(target) = download_sidecar_target(path) {
            targets.insert(target);
        }
    }
    Ok(targets)
}

fn push_warning(warnings: &mut Vec<String>, message: String) {
    if warnings.iter().any(|warning| warning == &message) {
        return;
    }
    if warnings.len() < WARNING_LIMIT {
        warnings.push(message);
    } else if warnings.len() == WARNING_LIMIT {
        warnings.push("more scan warnings omitted".to_string());
    }
}

fn extend_warnings(warnings: &mut Vec<String>, messages: Vec<String>) {
    for message in messages {
        push_warning(warnings, message);
    }
}

fn skip_file_reason(
    path: &Path,
    file_type: FileType,
    size: u64,
    download_sidecars: &HashSet<PathBuf>,
) -> Option<String> {
    if is_partial_file(path) {
        return Some(format!("skipped incomplete download: {}", path.display()));
    }

    if is_learning_file(file_type) && download_sidecars.contains(path) {
        return Some(format!(
            "skipped file with active download sidecar: {}",
            path.display()
        ));
    }

    if size == 0 && is_learning_file(file_type) {
        return Some(format!("skipped empty learning item: {}", path.display()));
    }

    None
}

fn is_ignored_or_partial_path(path: &Path, base: &Path) -> bool {
    let relative = path.strip_prefix(base).unwrap_or(path);
    relative.components().any(|component| {
        let part = component.as_os_str().to_string_lossy();
        let lower = part.to_ascii_lowercase();
        part.starts_with('.')
            || IGNORED_FOLDERS.contains(&part.as_ref())
            || PARTIAL_FOLDER_NAMES.contains(&lower.as_str())
    })
}

fn is_media_file(file_type: FileType) -> bool {
    matches!(
        file_type,
        FileType::Video | FileType::Audio | FileType::Document | FileType::Quiz
    )
}

fn is_learning_file(file_type: FileType) -> bool {
    is_media_file(file_type) || file_type == FileType::Subtitle
}

fn natural_cmp(a: &str, b: &str) -> Ordering {
    let mut ai = 0;
    let mut bi = 0;
    let ab = a.as_bytes();
    let bb = b.as_bytes();

    while ai < ab.len() && bi < bb.len() {
        if ab[ai].is_ascii_digit() && bb[bi].is_ascii_digit() {
            let a_start = ai;
            let b_start = bi;
            while ai < ab.len() && ab[ai].is_ascii_digit() {
                ai += 1;
            }
            while bi < bb.len() && bb[bi].is_ascii_digit() {
                bi += 1;
            }
            let an = a[a_start..ai].trim_start_matches('0');
            let bn = b[b_start..bi].trim_start_matches('0');
            let an = if an.is_empty() { "0" } else { an };
            let bn = if bn.is_empty() { "0" } else { bn };
            let number_order = an.len().cmp(&bn.len()).then_with(|| an.cmp(bn));
            if number_order != Ordering::Equal {
                return number_order;
            }
            continue;
        }

        let ac = ab[ai].to_ascii_lowercase();
        let bc = bb[bi].to_ascii_lowercase();
        if ac != bc {
            return ac.cmp(&bc);
        }
        ai += 1;
        bi += 1;
    }

    ab.len().cmp(&bb.len()).then_with(|| a.cmp(b))
}

fn require_utf8_path(path: &Path) -> Result<(), ScanError> {
    if path.to_str().is_some() {
        Ok(())
    } else {
        Err(ScanError::Invalid(format!(
            "path is not valid UTF-8: {}",
            path.display()
        )))
    }
}

fn path_name(path: &Path) -> String {
    path.file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default()
        .to_string()
}

fn read_dir_sorted(
    path: &Path,
    control: &MutationControl,
) -> Result<Vec<std::fs::DirEntry>, ScanError> {
    let directory = std::fs::read_dir(path).map_err(|error| {
        ScanError::Invalid(format!("cannot read directory {}: {error}", path.display()))
    })?;
    let mut entries = Vec::new();
    for entry in directory {
        require_active(control)?;
        let entry = entry.map_err(|error| {
            ScanError::Invalid(format!(
                "cannot read directory entry in {}: {error}",
                path.display()
            ))
        })?;
        require_utf8_path(&entry.path())?;
        entries.push(entry);
    }
    entries.sort_by(|a, b| natural_cmp(&path_name(&a.path()), &path_name(&b.path())));
    Ok(entries)
}

fn visible_child_dirs(path: &Path, control: &MutationControl) -> Result<Vec<PathBuf>, ScanError> {
    let mut directories = Vec::new();
    for entry in read_dir_sorted(path, control)? {
        require_active(control)?;
        let entry_path = entry.path();
        let file_type = entry.file_type().map_err(|error| {
            ScanError::Invalid(format!(
                "cannot inspect path {}: {error}",
                entry_path.display()
            ))
        })?;
        if file_type.is_dir() && !is_ignored(&entry_path) && !is_partial_folder(&entry_path) {
            directories.push(entry_path);
        }
    }
    Ok(directories)
}

fn warn_for_symlink_escape(path: &Path, root: &Path, warnings: &mut Vec<String>) {
    let message = match std::fs::canonicalize(path) {
        Ok(target) if target.starts_with(root) => return,
        Ok(_) => format!(
            "skipped symbolic link outside scan root: {}",
            path.display()
        ),
        Err(error) => format!(
            "skipped unreadable symbolic link {}: {error}",
            path.display()
        ),
    };
    push_warning(warnings, message);
}

fn safe_entry_type(
    entry: &std::fs::DirEntry,
    root: &Path,
    warnings: &mut Vec<String>,
) -> Result<Option<std::fs::FileType>, ScanError> {
    let path = entry.path();
    let file_type = entry.file_type().map_err(|error| {
        ScanError::Invalid(format!("cannot inspect path {}: {error}", path.display()))
    })?;
    if file_type.is_symlink() {
        warn_for_symlink_escape(&path, root, warnings);
        return Ok(None);
    }
    Ok(Some(file_type))
}

fn looks_like_section_folder(path: &Path) -> bool {
    let name = lower_name(path);
    let trimmed = name.trim();
    trimmed.chars().next().is_some_and(|c| c.is_ascii_digit())
        || trimmed.contains("section")
        || trimmed.contains("module")
        || trimmed.contains("chapter")
        || trimmed.contains("lecture")
}

fn should_scan_root_as_single_course(
    subdirs: &[PathBuf],
    control: &MutationControl,
) -> Result<bool, ScanError> {
    if subdirs.len() < 2 {
        return Ok(false);
    }

    for path in subdirs {
        require_active(control)?;
        if !looks_like_section_folder(path)
            || should_scan_root_as_single_course(&visible_child_dirs(path, control)?, control)?
        {
            return Ok(false);
        }
    }
    Ok(true)
}

fn file_type_key(file_type: FileType) -> &'static [u8] {
    match file_type {
        FileType::Video => b"video",
        FileType::Audio => b"audio",
        FileType::Document => b"document",
        FileType::Subtitle => b"subtitle",
        FileType::Quiz => b"quiz",
        FileType::Unknown => b"unknown",
    }
}

fn course_fingerprint(
    sections: &[SectionData],
    control: &MutationControl,
) -> Result<Box<str>, ScanError> {
    let mut h = new_hasher();
    h.write(b"course-fingerprint-v1");

    for section in sections {
        require_active(control)?;
        h.write(b"\x1fsection\x1e");
        h.write(section.name.as_bytes());

        for file in section
            .files
            .iter()
            .filter(|file| file.file_type != FileType::Subtitle)
        {
            require_active(control)?;
            h.write(b"\x1ffile\x1e");
            h.write(file.relative_path.as_bytes());
            h.write(b"\x1e");
            h.write(file_type_key(file.file_type));
            h.write(b"\x1e");
            h.write(&file.size.to_le_bytes());
        }
    }

    Ok(format!("{:016x}", h.finish()).into())
}

fn read_course_marker(course_path: &Path) -> Result<Option<Box<str>>, String> {
    let marker_path = course_path.join(COURSE_MARKER_FILE_NAME);
    match std::fs::symlink_metadata(&marker_path) {
        Ok(metadata) if metadata.file_type().is_symlink() => {
            return Err(format!(
                "course marker must not be a symbolic link: {}",
                marker_path.display()
            ));
        }
        Ok(metadata) if !metadata.file_type().is_file() => {
            return Err(format!(
                "course marker is not a regular file: {}",
                marker_path.display()
            ));
        }
        Ok(metadata) if metadata.len() > COURSE_MARKER_MAX_BYTES => {
            return Err(format!(
                "course marker exceeds {COURSE_MARKER_MAX_BYTES} bytes: {}",
                marker_path.display()
            ));
        }
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => {
            return Err(format!(
                "cannot inspect course marker {}: {error}",
                marker_path.display()
            ));
        }
    }

    let raw = std::fs::read_to_string(&marker_path)
        .map_err(|e| format!("cannot read course marker {}: {e}", marker_path.display()))?;
    parse_course_marker(&raw, &marker_path).map(Some)
}

fn parse_course_marker(raw: &str, marker_path: &Path) -> Result<Box<str>, String> {
    let marker: CourseMarker = serde_json::from_str(raw)
        .map_err(|e| format!("invalid course marker {}: {e}", marker_path.display()))?;
    if marker.version != COURSE_MARKER_VERSION {
        return Err(format!(
            "unsupported course marker version {} in {}",
            marker.version,
            marker_path.display()
        ));
    }
    let identity_id = marker.identity_id.as_deref().unwrap_or_default().trim();
    if identity_id.is_empty() {
        return Err(format!(
            "invalid course marker {}: identityId is empty",
            marker_path.display()
        ));
    }

    Ok(identity_id.to_string().into_boxed_str())
}

pub(crate) fn ensure_course_marker(
    root: &CapabilityDir,
    course_relative_path: &Path,
    course_path: &Path,
    identity_id: &str,
) -> Result<(), String> {
    let identity_id = identity_id.trim();
    if identity_id.is_empty() {
        return Err("course marker identity is empty".to_string());
    }
    if course_relative_path.is_absolute()
        || course_relative_path.components().any(|component| {
            matches!(
                component,
                std::path::Component::ParentDir
                    | std::path::Component::RootDir
                    | std::path::Component::Prefix(_)
            )
        })
    {
        return Err(format!(
            "course folder escaped library root: {}",
            course_path.display()
        ));
    }
    let relative_course = if course_relative_path.as_os_str().is_empty() {
        Path::new(".")
    } else {
        course_relative_path
    };
    let course_metadata = root.symlink_metadata(relative_course).map_err(|error| {
        format!(
            "cannot inspect course folder {}: {error}",
            course_path.display()
        )
    })?;
    if course_metadata.file_type().is_symlink() || !course_metadata.is_dir() {
        return Err(format!(
            "course folder is not available: {}",
            course_path.display()
        ));
    }
    let marker_relative_path = course_relative_path.join(COURSE_MARKER_FILE_NAME);
    let marker_path = course_path.join(COURSE_MARKER_FILE_NAME);

    match root.symlink_metadata(&marker_relative_path) {
        Ok(metadata) => {
            if metadata.file_type().is_symlink() {
                return Err(format!(
                    "course marker must not be a symbolic link: {}",
                    marker_path.display()
                ));
            }
            if !metadata.file_type().is_file() {
                return Err(format!(
                    "course marker is not a regular file: {}",
                    marker_path.display()
                ));
            }
            if metadata.len() > COURSE_MARKER_MAX_BYTES {
                return Err(format!(
                    "course marker exceeds {COURSE_MARKER_MAX_BYTES} bytes: {}",
                    marker_path.display()
                ));
            }
            let raw = root
                .read_to_string(&marker_relative_path)
                .map_err(|error| {
                    format!(
                        "cannot read course marker {}: {error}",
                        marker_path.display()
                    )
                })?;
            let existing = parse_course_marker(&raw, &marker_path)?;
            if existing.as_ref() == identity_id {
                return Ok(());
            }
            return Err(format!(
                "course marker already has a different identity: {}",
                marker_path.display()
            ));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(format!(
                "cannot inspect course marker {}: {error}",
                marker_path.display()
            ));
        }
    }

    let marker = CourseMarker {
        version: COURSE_MARKER_VERSION,
        identity_id: Some(identity_id.to_string()),
    };
    let json = serde_json::to_string_pretty(&marker)
        .map_err(|error| format!("cannot serialize course marker: {error}"))?;
    let mut options = CapabilityOpenOptions::new();
    options.write(true).create_new(true);
    let mut file = root
        .open_with(&marker_relative_path, &options)
        .map_err(|error| {
            format!(
                "cannot create course marker {}: {error}",
                marker_path.display()
            )
        })?;
    if let Err(error) = file.write_all(format!("{json}\n").as_bytes()) {
        drop(file);
        let cleanup = root.remove_file(&marker_relative_path);
        return Err(match cleanup {
            Ok(()) => format!(
                "cannot write course marker {}: {error}",
                marker_path.display()
            ),
            Err(cleanup_error) => format!(
                "cannot write course marker {}: {error}; cannot remove partial marker: {cleanup_error}",
                marker_path.display()
            ),
        });
    }
    Ok(())
}

fn file_size(path: &Path) -> Result<u64, ScanError> {
    std::fs::metadata(path)
        .map(|metadata| metadata.len())
        .map_err(|error| {
            ScanError::Invalid(format!(
                "cannot inspect learning item {}: {error}",
                path.display()
            ))
        })
}

fn file_entry(
    path: &Path,
    file_type: FileType,
    course_root: &Path,
    size: u64,
) -> Option<FileEntry> {
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .map(Box::from)
        .unwrap_or_default();
    Some(FileEntry {
        id: hash_path_to_id(path),
        path: path.to_string_lossy().into_owned().into_boxed_str(),
        relative_path: normalized_relative_path(path, course_root),
        name,
        file_type,
        size,
    })
}

fn scan_directory(
    dir: &Path,
    course_root: &Path,
    control: &MutationControl,
) -> Result<(Box<[FileEntry]>, Vec<String>), ScanError> {
    let mut warnings = Vec::new();
    let mut paths = Vec::new();
    let mut symlinks = Vec::new();
    for entry in WalkDir::new(dir).skip_hidden(true).follow_links(false) {
        require_active(control)?;
        let entry = entry.map_err(|error| {
            ScanError::Invalid(format!("cannot walk directory {}: {error}", dir.display()))
        })?;
        let path = entry.path();
        require_utf8_path(&path)?;
        if entry.file_type().is_symlink() {
            symlinks.push(path);
            continue;
        }
        if entry.file_type().is_file() && !is_ignored_or_partial_path(&path, dir) {
            paths.push(path);
        }
    }
    symlinks.sort_by(|left, right| natural_cmp(&left.to_string_lossy(), &right.to_string_lossy()));
    for path in symlinks {
        require_active(control)?;
        warn_for_symlink_escape(&path, course_root, &mut warnings);
    }
    paths.sort_by(|left, right| natural_cmp(&left.to_string_lossy(), &right.to_string_lossy()));
    let download_sidecars = download_sidecar_targets(&paths, control)?;

    let mut files = Vec::new();
    for path in paths {
        require_active(control)?;
        let file_type = get_file_type(&path);
        if !is_learning_file(file_type) {
            if let Some(reason) = skip_file_reason(&path, file_type, 0, &download_sidecars) {
                push_warning(&mut warnings, reason);
            }
            continue;
        }
        let size = file_size(&path)?;
        if let Some(reason) = skip_file_reason(&path, file_type, size, &download_sidecars) {
            push_warning(&mut warnings, reason);
            continue;
        }
        if let Some(entry) = file_entry(&path, file_type, course_root, size) {
            files.push(entry);
        }
    }

    files.sort_by(|a, b| natural_cmp(&a.relative_path, &b.relative_path));

    Ok((files.into_boxed_slice(), warnings))
}

fn scan_course_with_control(
    course_path: &Path,
    control: &MutationControl,
) -> Result<(CourseData, Vec<String>), ScanError> {
    let mut sections: Vec<SectionData> = Vec::new();
    let mut root_files: Vec<FileEntry> = Vec::new();
    let mut warnings = Vec::new();
    let entries = read_dir_sorted(course_path, control)?;
    let root_paths = entries.iter().map(|entry| entry.path()).collect::<Vec<_>>();
    let root_download_sidecars = download_sidecar_targets(&root_paths, control)?;
    let marker_identity_id = match read_course_marker(course_path) {
        Ok(identity_id) => identity_id,
        Err(message) => {
            push_warning(&mut warnings, message);
            None
        }
    };

    for (index, entry) in entries.iter().enumerate() {
        require_active(control)?;
        let path = entry.path();
        let Some(entry_type) = safe_entry_type(entry, course_path, &mut warnings)? else {
            continue;
        };

        if entry_type.is_dir() && is_partial_folder(&path) {
            push_warning(
                &mut warnings,
                format!("skipped incomplete folder: {}", path.display()),
            );
            continue;
        }

        if is_ignored(&path) {
            continue;
        }

        if entry_type.is_dir() {
            let (files, section_warnings) = scan_directory(&path, course_path, control)?;
            extend_warnings(&mut warnings, section_warnings);
            if !files.is_empty() {
                sections.push(SectionData {
                    id: hash_path_to_id(&path),
                    name: path
                        .file_name()
                        .and_then(|n| n.to_str())
                        .map(Box::from)
                        .unwrap_or_default(),
                    files,
                    order: index,
                });
            }
        } else if entry_type.is_file() {
            let file_type = get_file_type(&path);
            if !is_learning_file(file_type) {
                if let Some(reason) = skip_file_reason(&path, file_type, 0, &root_download_sidecars)
                {
                    push_warning(&mut warnings, reason);
                }
                continue;
            }
            let size = file_size(&path)?;
            if let Some(reason) = skip_file_reason(&path, file_type, size, &root_download_sidecars)
            {
                push_warning(&mut warnings, reason);
                continue;
            }
            if let Some(entry) = file_entry(&path, file_type, course_path, size) {
                root_files.push(entry);
            }
        }
    }

    if !root_files.is_empty() {
        sections.insert(
            0,
            SectionData {
                id: hash_str_to_id(&format!("{}_introduction", course_path.to_string_lossy())),
                name: "introduction".into(),
                files: root_files.into_boxed_slice(),
                order: 0,
            },
        );
        for (i, section) in sections.iter_mut().skip(1).enumerate() {
            section.order = i + 1;
        }
    }

    sections.sort_by(|a, b| {
        a.order
            .cmp(&b.order)
            .then_with(|| natural_cmp(&a.name, &b.name))
    });
    let fingerprint = course_fingerprint(&sections, control)?;

    Ok((
        CourseData {
            id: hash_path_to_id(course_path),
            marker_identity_id,
            name: course_path
                .file_name()
                .and_then(|n| n.to_str())
                .map(Box::from)
                .unwrap_or_default(),
            path: course_path.to_string_lossy().into_owned().into_boxed_str(),
            fingerprint,
            sections: sections.into_boxed_slice(),
        },
        warnings,
    ))
}

#[cfg(test)]
fn scan_course(course_path: &Path) -> (CourseData, Vec<String>) {
    scan_course_with_control(course_path, &MutationControl::new()).expect("scan test course")
}

pub(crate) fn scan_library_checked_with_control(
    root_path: &Path,
    control: &MutationControl,
) -> Result<ScanResult, ScanError> {
    require_active(control)?;
    let exists = root_path.try_exists().map_err(|error| {
        ScanError::Invalid(format!(
            "cannot access path {}: {error}",
            root_path.display()
        ))
    })?;
    if !exists {
        return Err(ScanError::Invalid(format!(
            "path does not exist: {}",
            root_path.display()
        )));
    }

    let metadata = std::fs::metadata(root_path).map_err(|error| {
        ScanError::Invalid(format!(
            "cannot inspect path {}: {error}",
            root_path.display()
        ))
    })?;
    if !metadata.is_dir() {
        return Err(ScanError::Invalid(format!(
            "not a directory: {}",
            root_path.display()
        )));
    }

    let root = std::fs::canonicalize(root_path).map_err(|error| {
        ScanError::Invalid(format!(
            "cannot resolve directory {}: {error}",
            root_path.display()
        ))
    })?;
    require_utf8_path(&root)?;
    let entries = read_dir_sorted(&root, control)?;

    scan_valid_root(root, entries, control)
}

pub(crate) fn scan_library_checked(root_path: &Path) -> Result<ScanResult, String> {
    let control = MutationControl::new();
    scan_library_checked_with_control(root_path, &control).map_err(|error| match error {
        ScanError::Cancelled => "scan cancelled".to_string(),
        ScanError::Invalid(message) => message,
    })
}

pub fn scan_library(root_path: &str) -> ScanResult {
    scan_library_checked(Path::new(root_path)).unwrap_or_else(|warning| ScanResult {
        scan_type: ScanType::Library,
        courses: Box::new([]),
        warnings: Box::new([warning]),
    })
}

fn scan_valid_root(
    root: PathBuf,
    entries: Vec<std::fs::DirEntry>,
    control: &MutationControl,
) -> Result<ScanResult, ScanError> {
    require_active(control)?;
    let mut warnings = Vec::new();
    match read_course_marker(&root) {
        Ok(Some(_)) => {
            let (course, course_warnings) = scan_course_with_control(&root, control)?;
            extend_warnings(&mut warnings, course_warnings);
            return Ok(ScanResult {
                scan_type: ScanType::SingleCourse,
                courses: Box::new([course]),
                warnings: warnings.into_boxed_slice(),
            });
        }
        Ok(None) => {}
        Err(message) => push_warning(&mut warnings, message),
    }

    let mut root_files_exist = false;
    let mut subdirs: Vec<PathBuf> = Vec::new();
    let root_paths = entries.iter().map(|entry| entry.path()).collect::<Vec<_>>();
    let root_download_sidecars = download_sidecar_targets(&root_paths, control)?;

    for entry in entries {
        require_active(control)?;
        let path = entry.path();
        let Some(entry_type) = safe_entry_type(&entry, &root, &mut warnings)? else {
            continue;
        };

        if entry_type.is_file() {
            let file_type = get_file_type(&path);
            if !is_learning_file(file_type) {
                if let Some(reason) = skip_file_reason(&path, file_type, 0, &root_download_sidecars)
                {
                    push_warning(&mut warnings, reason);
                }
                continue;
            }
            let size = file_size(&path)?;
            if let Some(reason) = skip_file_reason(&path, file_type, size, &root_download_sidecars)
            {
                push_warning(&mut warnings, reason);
                continue;
            }
            if is_media_file(file_type) {
                root_files_exist = true;
            }
        } else if entry_type.is_dir() {
            if is_partial_folder(&path) {
                push_warning(
                    &mut warnings,
                    format!("skipped incomplete folder: {}", path.display()),
                );
                continue;
            }
            if is_ignored(&path) {
                continue;
            }
            subdirs.push(path);
        }
    }

    if root_files_exist && subdirs.is_empty() {
        let (course, course_warnings) = scan_course_with_control(&root, control)?;
        extend_warnings(&mut warnings, course_warnings);
        return Ok(ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([course]),
            warnings: warnings.into_boxed_slice(),
        });
    }

    if root_files_exist && !subdirs.is_empty() {
        let (course, course_warnings) = scan_course_with_control(&root, control)?;
        extend_warnings(&mut warnings, course_warnings);
        push_warning(&mut warnings, "mixed content at root level".to_string());
        return Ok(ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([course]),
            warnings: warnings.into_boxed_slice(),
        });
    }

    if should_scan_root_as_single_course(&subdirs, control)? {
        let (course, course_warnings) = scan_course_with_control(&root, control)?;
        if !course.sections.is_empty() {
            extend_warnings(&mut warnings, course_warnings);
            return Ok(ScanResult {
                scan_type: ScanType::SingleCourse,
                courses: Box::new([course]),
                warnings: warnings.into_boxed_slice(),
            });
        }
        extend_warnings(&mut warnings, course_warnings);
    }

    let scanned = subdirs
        .iter()
        .map(|dir| {
            require_active(control)?;
            std::panic::catch_unwind(|| scan_course_with_control(dir, control))
                .map_err(|_| format!("skipped course after scanner panic: {}", dir.display()))
                .map_err(ScanError::Invalid)
        })
        .collect::<Result<Vec<_>, ScanError>>()?;

    let mut courses = Vec::new();
    for result in scanned {
        match result {
            Ok((course, course_warnings)) => {
                extend_warnings(&mut warnings, course_warnings);
                if !course.sections.is_empty() {
                    courses.push(course);
                }
            }
            Err(ScanError::Cancelled) => return Err(ScanError::Cancelled),
            Err(ScanError::Invalid(message)) => push_warning(&mut warnings, message),
        }
    }

    Ok(ScanResult {
        scan_type: ScanType::Library,
        courses: courses.into_boxed_slice(),
        warnings: warnings.into_boxed_slice(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::Deserialize;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root(name: &str) -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("melearner-{name}-{suffix}"));
        fs::create_dir_all(&root).expect("create temp root");
        root
    }

    fn touch(path: &Path) {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("create parent");
        }
        fs::write(path, b"fixture").expect("write fixture");
    }

    fn cleanup(path: &Path) {
        let _ = fs::remove_dir_all(path);
    }

    fn ensure_test_course_marker(
        root: &Path,
        course: &Path,
        identity_id: &str,
    ) -> Result<(), String> {
        let capability = CapabilityDir::open_ambient_dir(root, cap_std::ambient_authority())
            .map_err(|error| format!("open test root capability: {error}"))?;
        let relative = course
            .strip_prefix(root)
            .map_err(|error| format!("resolve test course relative path: {error}"))?;
        ensure_course_marker(&capability, relative, course, identity_id)
    }

    #[test]
    fn checked_scan_rejects_invalid_roots_without_writing_markers() {
        let root = temp_root("checked-root");
        let missing = root.join("missing");
        let file = root.join("not-a-directory");
        fs::write(&file, b"fixture").expect("write non-directory fixture");

        assert!(
            scan_library_checked(&missing)
                .expect_err("missing root should fail")
                .starts_with("path does not exist:")
        );
        assert!(
            scan_library_checked(&file)
                .expect_err("file root should fail")
                .starts_with("not a directory:")
        );

        let course = root.join("Course");
        touch(&course.join("01 Intro/01 welcome.mp4"));
        let result = scan_library_checked(&root).expect("scan readable root");

        assert_eq!(result.courses.len(), 1);
        assert!(!course.join(COURSE_MARKER_FILE_NAME).exists());

        cleanup(&root);
    }

    #[test]
    fn checked_scan_stops_before_traversal_when_cancelled() {
        let root = temp_root("cancelled-scan");
        touch(&root.join("Course/01 Intro/01 welcome.mp4"));
        let control = MutationControl::new();
        assert!(control.cancel());

        assert!(matches!(
            scan_library_checked_with_control(&root, &control),
            Err(ScanError::Cancelled)
        ));
        assert!(!root.join("Course").join(COURSE_MARKER_FILE_NAME).exists());

        cleanup(&root);
    }

    #[test]
    fn invalid_root_marker_does_not_collapse_a_library_scan() {
        let root = temp_root("invalid-root-marker");
        touch(&root.join("Course A/01 Intro/01 a.mp4"));
        touch(&root.join("Course B/01 Intro/01 b.mp4"));
        fs::write(root.join(COURSE_MARKER_FILE_NAME), "not json")
            .expect("write invalid root marker");

        let result = scan_library_checked(&root).expect("scan library with invalid root marker");

        assert_eq!(result.scan_type, ScanType::Library);
        assert_eq!(result.courses.len(), 2);
        assert!(result.warnings.iter().any(|warning| {
            warning.contains("invalid course marker") && warning.contains(COURSE_MARKER_FILE_NAME)
        }));

        cleanup(&root);
    }

    #[test]
    fn marker_owner_creates_v1_and_preserves_an_existing_match() {
        let root = temp_root("marker-owner");
        let course = root.join("Course");
        fs::create_dir_all(&course).expect("create course");
        let marker_path = course.join(COURSE_MARKER_FILE_NAME);

        ensure_test_course_marker(&root, &course, " course-identity-1 ").expect("create marker");
        assert_eq!(
            fs::read_to_string(&marker_path).expect("read created marker"),
            "{\n  \"version\": 1,\n  \"identityId\": \"course-identity-1\"\n}\n"
        );

        let existing = "{\"identityId\":\"course-identity-1\",\"version\":1}\n";
        fs::write(&marker_path, existing).expect("replace marker fixture");
        ensure_test_course_marker(&root, &course, "course-identity-1")
            .expect("accept matching marker");
        assert_eq!(
            fs::read_to_string(&marker_path).expect("read preserved marker"),
            existing
        );

        cleanup(&root);
    }

    #[test]
    fn marker_owner_never_overwrites_unowned_marker_bytes() {
        let root = temp_root("marker-conflicts");
        let course = root.join("Course");
        fs::create_dir_all(&course).expect("create course");
        let marker_path = course.join(COURSE_MARKER_FILE_NAME);
        let fixtures = [
            r#"{"version":1,"identityId":"different"}"#,
            r#"{"version":1,"identityId":""}"#,
            r#"{"version":2,"identityId":"course-identity-1"}"#,
            "not json",
        ];

        for existing in fixtures {
            fs::write(&marker_path, existing).expect("write conflicting marker fixture");
            ensure_test_course_marker(&root, &course, "course-identity-1")
                .expect_err("unowned marker should be rejected");
            assert_eq!(
                fs::read_to_string(&marker_path).expect("read rejected marker"),
                existing
            );
        }

        cleanup(&root);
    }

    #[cfg(unix)]
    #[test]
    fn checked_scan_rejects_direct_and_nested_symlink_escapes() {
        use std::os::unix::fs::symlink;

        let root = temp_root("symlink-root");
        let outside = temp_root("symlink-outside");
        let safe_course = root.join("Safe Course");
        touch(&safe_course.join("01 Intro/01 safe.mp4"));
        touch(&outside.join("Outside Course/01 Intro/01 outside.mp4"));
        touch(&outside.join("outside-direct.mp4"));

        symlink(outside.join("Outside Course"), root.join("Escaped Course"))
            .expect("link escaped course");
        symlink(
            outside.join("outside-direct.mp4"),
            safe_course.join("02 escaped.mp4"),
        )
        .expect("link escaped lesson");
        symlink(
            outside.join("Outside Course"),
            safe_course.join("01 Intro/escaped"),
        )
        .expect("link escaped nested directory");

        let result = scan_library_checked(&root).expect("scan contained root");
        let canonical_root = fs::canonicalize(&root).expect("canonical root");
        let files = result
            .courses
            .iter()
            .flat_map(|course| course.sections.iter())
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert_eq!(result.courses.len(), 1);
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].name.as_ref(), "01 safe.mp4");
        assert!(result.courses.iter().all(|course| {
            fs::canonicalize(Path::new(course.path.as_ref()))
                .is_ok_and(|path| path.starts_with(&canonical_root))
        }));
        assert!(files.iter().all(|file| {
            fs::canonicalize(Path::new(file.path.as_ref()))
                .is_ok_and(|path| path.starts_with(&canonical_root))
        }));
        assert!(
            result
                .warnings
                .iter()
                .filter(|warning| warning.contains("symbolic link outside scan root"))
                .count()
                >= 3
        );

        cleanup(&root);
        cleanup(&outside);
    }

    #[cfg(unix)]
    #[test]
    fn marker_owner_never_follows_a_symbolic_link() {
        use std::os::unix::fs::symlink;

        let root = temp_root("marker-link-root");
        let outside = temp_root("marker-link-outside");
        let course = root.join("Course");
        fs::create_dir_all(&course).expect("create course");
        let outside_marker = outside.join("marker.json");
        let existing = r#"{"version":1,"identityId":"outside-identity"}"#;
        fs::write(&outside_marker, existing).expect("write outside marker");
        symlink(&outside_marker, course.join(COURSE_MARKER_FILE_NAME)).expect("link course marker");

        ensure_test_course_marker(&root, &course, "course-identity-1")
            .expect_err("symbolic marker should be rejected");
        assert_eq!(
            fs::read_to_string(&outside_marker).expect("read outside marker"),
            existing
        );

        touch(&course.join("01 Intro/01 safe.mp4"));
        let result = scan_library_checked(&root).expect("scan root");
        assert_eq!(result.courses[0].marker_identity_id, None);
        assert!(
            result
                .warnings
                .iter()
                .any(|warning| warning.contains("course marker must not be a symbolic link"))
        );

        cleanup(&root);
        cleanup(&outside);
    }

    #[cfg(unix)]
    #[test]
    fn marker_owner_rejects_a_replaced_course_directory() {
        use std::os::unix::fs::symlink;

        let root = temp_root("marker-course-swap-root");
        let outside = temp_root("marker-course-swap-outside");
        let course = root.join("Course");
        fs::create_dir_all(&course).expect("create course");
        let capability = CapabilityDir::open_ambient_dir(&root, cap_std::ambient_authority())
            .expect("open root capability before directory replacement");
        fs::remove_dir(&course).expect("remove scanned course");
        symlink(&outside, &course).expect("replace course with symlink");

        ensure_course_marker(
            &capability,
            Path::new("Course"),
            &course,
            "course-identity-1",
        )
        .expect_err("replaced course directory should be rejected");
        assert!(!outside.join(COURSE_MARKER_FILE_NAME).exists());

        cleanup(&root);
        cleanup(&outside);
    }

    #[derive(Deserialize)]
    #[serde(rename_all = "camelCase")]
    struct ScannerParityFixture {
        files: Vec<ScannerParityFile>,
        expected: ScannerParityExpected,
        error_cases: Vec<ScannerParityError>,
    }

    #[derive(Deserialize)]
    struct ScannerParityFile {
        path: String,
        contents: String,
    }

    #[derive(Deserialize)]
    #[serde(rename_all = "camelCase")]
    struct ScannerParityExpected {
        scan_type: String,
        course_names: Vec<String>,
        learning_paths: Vec<String>,
        duplicate_fingerprint_courses: Vec<String>,
        duplicate_marker_courses: Vec<String>,
        warning_fragments: Vec<String>,
        excluded_names: Vec<String>,
    }

    #[derive(Deserialize)]
    #[serde(rename_all = "camelCase")]
    struct ScannerParityError {
        kind: String,
        warning_prefix: String,
    }

    #[test]
    fn scanner_matches_current_parity_fixture() {
        let fixture: ScannerParityFixture =
            serde_json::from_str(include_str!("../../../fixtures/parity/scanner-v1.json"))
                .expect("parse scanner parity fixture");
        let root = temp_root("parity-fixture");

        for file in &fixture.files {
            let path = root.join(&file.path);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).expect("create parity fixture parent");
            }
            fs::write(path, file.contents.as_bytes()).expect("write parity fixture file");
        }

        let result = scan_library(&root.to_string_lossy());
        assert_eq!(
            format!("{:?}", result.scan_type).to_ascii_lowercase(),
            fixture.expected.scan_type
        );

        let mut course_names = result
            .courses
            .iter()
            .map(|course| course.name.to_string())
            .collect::<Vec<_>>();
        course_names.sort();
        assert_eq!(course_names, fixture.expected.course_names);

        let files = result
            .courses
            .iter()
            .flat_map(|course| course.sections.iter())
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();
        let relative_paths = files
            .iter()
            .map(|file| file.relative_path.to_string())
            .collect::<Vec<_>>();
        for expected in &fixture.expected.learning_paths {
            assert!(
                relative_paths.contains(expected),
                "missing fixture path: {expected}"
            );
        }
        assert!(
            fixture
                .expected
                .learning_paths
                .iter()
                .any(|path| path.len() > 260),
            "scanner fixture must retain a long path"
        );
        for excluded in &fixture.expected.excluded_names {
            assert!(!files.iter().any(|file| file.name.as_ref() == excluded));
        }
        for fragment in &fixture.expected.warning_fragments {
            assert!(
                result
                    .warnings
                    .iter()
                    .any(|warning| warning.contains(fragment))
            );
        }

        let duplicate_fingerprints = fixture
            .expected
            .duplicate_fingerprint_courses
            .iter()
            .map(|name| {
                result
                    .courses
                    .iter()
                    .find(|course| course.name.as_ref() == name)
                    .expect("duplicate fingerprint course")
                    .fingerprint
                    .to_string()
            })
            .collect::<Vec<_>>();
        assert!(
            duplicate_fingerprints
                .windows(2)
                .all(|pair| pair[0] == pair[1])
        );

        let duplicate_markers = fixture
            .expected
            .duplicate_marker_courses
            .iter()
            .map(|name| {
                result
                    .courses
                    .iter()
                    .find(|course| course.name.as_ref() == name)
                    .expect("duplicate marker course")
                    .marker_identity_id
                    .as_deref()
            })
            .collect::<Vec<_>>();
        assert!(duplicate_markers.windows(2).all(|pair| pair[0] == pair[1]));

        for error in &fixture.error_cases {
            let result = match error.kind.as_str() {
                "missing" => scan_library(&root.join("missing").to_string_lossy()),
                "file" => {
                    let path = root.join("not-a-directory");
                    fs::write(&path, b"fixture").expect("write non-directory fixture");
                    scan_library(&path.to_string_lossy())
                }
                other => panic!("unknown scanner parity error kind: {other}"),
            };
            assert!(result.courses.is_empty());
            assert!(result.warnings[0].starts_with(&error.warning_prefix));
        }

        cleanup(&root);
    }

    #[test]
    fn scans_course_subdirectories_and_nested_learning_files() {
        let root = temp_root("library");
        touch(&root.join("Rust Basics/01 Intro/01 welcome.mp4"));
        touch(&root.join("Rust Basics/01 Intro/01 welcome.en.srt"));
        touch(&root.join("Rust Basics/resources/workbook.pdf"));
        touch(&root.join("Rust Basics/.hidden/ignored.mp4"));
        touch(&root.join("Docs Course/Reading/guide.markdown"));
        touch(&root.join("Docs Course/Reading/legacy.doc"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::Library);
        assert_eq!(result.courses.len(), 2);

        let all_files = result
            .courses
            .iter()
            .flat_map(|course| course.sections.iter())
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert!(all_files.iter().any(
            |file| file.name.as_ref() == "01 welcome.mp4" && file.file_type == FileType::Video
        ));
        assert!(
            all_files
                .iter()
                .any(|file| file.name.as_ref() == "01 welcome.en.srt"
                    && file.file_type == FileType::Subtitle)
        );
        assert!(
            all_files
                .iter()
                .any(|file| file.name.as_ref() == "guide.markdown"
                    && file.file_type == FileType::Document)
        );
        assert!(
            all_files
                .iter()
                .any(|file| file.name.as_ref() == "legacy.doc"
                    && file.file_type == FileType::Document)
        );
        assert!(all_files.iter().any(
            |file| file.name.as_ref() == "workbook.pdf" && file.file_type == FileType::Document
        ));
        assert!(
            !all_files
                .iter()
                .any(|file| file.name.as_ref() == "ignored.mp4")
        );

        cleanup(&root);
    }

    #[test]
    fn treats_mixed_root_content_as_single_course() {
        let root = temp_root("mixed");
        touch(&root.join("00 overview.mp4"));
        touch(&root.join("00 overview.srt"));
        touch(&root.join("Section 01/01 details.pdf"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::SingleCourse);
        assert_eq!(result.courses.len(), 1);
        assert_eq!(
            result.warnings.as_ref(),
            &["mixed content at root level".to_string()]
        );

        let course = &result.courses[0];
        assert_eq!(course.sections[0].name.as_ref(), "introduction");
        assert!(
            course.sections[0]
                .files
                .iter()
                .any(|file| file.name.as_ref() == "00 overview.mp4"
                    && file.file_type == FileType::Video)
        );
        assert!(
            course.sections[0]
                .files
                .iter()
                .any(|file| file.name.as_ref() == "00 overview.srt"
                    && file.file_type == FileType::Subtitle)
        );

        cleanup(&root);
    }

    #[test]
    fn treats_marked_course_root_without_root_media_as_single_course() {
        let root = temp_root("marked-course-root");
        fs::write(
            root.join(COURSE_MARKER_FILE_NAME),
            r#"{"version":1,"identityId":"course-identity-1"}"#,
        )
        .expect("write marker");
        touch(&root.join("01 - Intro/001 - Welcome.mp4"));
        touch(&root.join("02 - Data Structures/001 - Arrays.mp4"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::SingleCourse);
        assert_eq!(result.courses.len(), 1);
        assert_eq!(
            result.courses[0].marker_identity_id.as_deref(),
            Some("course-identity-1")
        );

        let files = result.courses[0]
            .sections
            .iter()
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert!(
            files
                .iter()
                .any(|file| file.name.as_ref() == "001 - Welcome.mp4"
                    && file.file_type == FileType::Video)
        );
        assert!(
            files
                .iter()
                .any(|file| file.name.as_ref() == "001 - Arrays.mp4"
                    && file.file_type == FileType::Video)
        );

        cleanup(&root);
    }

    #[test]
    fn treats_section_only_course_root_as_single_course() {
        let root = temp_root("section-only-course-root");
        touch(&root.join("01 - Intro/001 - Welcome.mp4"));
        touch(&root.join("02 - Data Structures/001 - Arrays.mp4"));
        touch(&root.join("03 - Trees/001 - Binary Trees.pdf"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::SingleCourse);
        assert_eq!(result.courses.len(), 1);
        let root_name = root.file_name().unwrap().to_string_lossy();
        assert_eq!(result.courses[0].name.as_ref(), root_name.as_ref());
        assert_eq!(result.courses[0].sections.len(), 3);

        let files = result.courses[0]
            .sections
            .iter()
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert_eq!(files.len(), 3);
        assert!(
            files
                .iter()
                .any(|file| file.relative_path.as_ref() == "01 - Intro/001 - Welcome.mp4")
        );
        assert!(
            files
                .iter()
                .any(|file| file.relative_path.as_ref() == "02 - Data Structures/001 - Arrays.mp4")
        );
        assert!(
            files
                .iter()
                .any(|file| file.relative_path.as_ref() == "03 - Trees/001 - Binary Trees.pdf")
        );

        cleanup(&root);
    }

    #[test]
    fn keeps_numbered_course_folders_as_library() {
        let root = temp_root("numbered-course-library");
        touch(&root.join("01 - Rust Basics/01 - Intro/001 - Welcome.mp4"));
        touch(&root.join("01 - Rust Basics/02 - Ownership/001 - Borrowing.mp4"));
        touch(&root.join("02 - Python Basics/01 - Intro/001 - Welcome.mp4"));
        touch(&root.join("02 - Python Basics/02 - Data/001 - Lists.mp4"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::Library);
        assert_eq!(result.courses.len(), 2);
        assert!(
            result
                .courses
                .iter()
                .any(|course| course.name.as_ref() == "01 - Rust Basics")
        );
        assert!(
            result
                .courses
                .iter()
                .any(|course| course.name.as_ref() == "02 - Python Basics")
        );

        cleanup(&root);
    }

    #[test]
    fn keeps_empty_section_named_folders_as_empty_library_scan() {
        let root = temp_root("empty-section-named-folders");
        fs::create_dir_all(root.join("01 - Intro")).expect("create empty section");
        fs::create_dir_all(root.join("02 - Data Structures")).expect("create empty section");

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::Library);
        assert!(result.courses.is_empty());

        cleanup(&root);
    }

    #[test]
    fn scans_marked_course_inside_library_with_nested_sections() {
        let root = temp_root("marked-course-library");
        let course = root.join("Rust Basics");
        fs::create_dir_all(&course).expect("create course");
        fs::write(
            course.join(COURSE_MARKER_FILE_NAME),
            r#"{"version":1,"identityId":"course-identity-1"}"#,
        )
        .expect("write marker");
        touch(&course.join("01 - Intro/001 - Welcome.mp4"));
        touch(&course.join("02 - Ownership/001 - Borrowing.mp4"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::Library);
        assert_eq!(result.courses.len(), 1);
        assert_eq!(
            result.courses[0].marker_identity_id.as_deref(),
            Some("course-identity-1")
        );

        let files = result.courses[0]
            .sections
            .iter()
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert_eq!(files.len(), 2);
        assert!(
            files
                .iter()
                .any(|file| file.name.as_ref() == "001 - Welcome.mp4")
        );
        assert!(
            files
                .iter()
                .any(|file| file.name.as_ref() == "001 - Borrowing.mp4")
        );

        cleanup(&root);
    }

    #[test]
    fn skips_partial_downloads_and_torrent_artifacts() {
        let root = temp_root("partial-downloads");
        touch(&root.join("Course/01 Intro/01 ready.mp4"));
        touch(&root.join("Course/01 Intro/02 active.mp4"));
        touch(&root.join("Course/01 Intro/02 active.mp4.aria2"));
        touch(&root.join("Course/01 Intro/03 browser.mp4.crdownload"));
        touch(&root.join("Course/01 Intro/course.torrent"));
        touch(&root.join("Course/.incomplete/04 later.mp4"));

        let result = scan_library(&root.to_string_lossy());
        let files = result
            .courses
            .iter()
            .flat_map(|course| course.sections.iter())
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert_eq!(result.courses.len(), 1);
        assert!(
            files
                .iter()
                .any(|file| file.name.as_ref() == "01 ready.mp4")
        );
        assert!(
            !files
                .iter()
                .any(|file| file.name.as_ref() == "02 active.mp4")
        );
        assert!(
            !files
                .iter()
                .any(|file| file.name.as_ref() == "03 browser.mp4.crdownload")
        );
        assert!(
            result
                .warnings
                .iter()
                .any(|warning| warning.contains("active download sidecar"))
        );
        assert!(
            result
                .warnings
                .iter()
                .any(|warning| warning.contains("incomplete download"))
        );
        assert!(
            result
                .warnings
                .iter()
                .any(|warning| warning.contains("incomplete folder"))
        );

        cleanup(&root);
    }

    #[test]
    fn skips_root_media_with_active_download_sidecar() {
        let root = temp_root("root-active-download");
        touch(&root.join("01 active.mp4"));
        touch(&root.join("01 active.mp4.aria2"));

        let result = scan_library(&root.to_string_lossy());

        assert!(result.courses.is_empty());
        assert!(
            result
                .warnings
                .iter()
                .any(|warning| warning.contains("active download sidecar"))
        );

        cleanup(&root);
    }

    #[test]
    fn course_fingerprint_survives_parent_move_and_folder_rename() {
        let root = temp_root("fingerprint-move");
        let original = root.join("Library A/Arm Assembly");
        let moved = root.join("Library B/Renamed Arm Assembly");
        touch(&original.join("01 Intro/01 welcome.mp4"));
        touch(&original.join("01 Intro/02 registers.pdf"));
        touch(&moved.join("01 Intro/01 welcome.mp4"));
        touch(&moved.join("01 Intro/02 registers.pdf"));

        let (original_course, _) = scan_course(&original);
        let (moved_course, _) = scan_course(&moved);

        assert_ne!(original_course.id, moved_course.id);
        assert_eq!(original_course.fingerprint, moved_course.fingerprint);
        assert_eq!(
            original_course.sections[0].files[0].relative_path.as_ref(),
            "01 Intro/01 welcome.mp4"
        );

        cleanup(&root);
    }

    #[test]
    fn course_fingerprint_changes_when_learning_items_change() {
        let root = temp_root("fingerprint-content");
        let first = root.join("Library A/Course");
        let second = root.join("Library B/Course");
        touch(&first.join("01 Intro/01 welcome.mp4"));
        touch(&second.join("01 Intro/01 welcome.mp4"));
        touch(&second.join("01 Intro/02 extra.mp4"));

        let (first_course, _) = scan_course(&first);
        let (second_course, _) = scan_course(&second);

        assert_ne!(first_course.fingerprint, second_course.fingerprint);

        cleanup(&root);
    }

    #[test]
    fn reads_course_marker_identity_without_treating_marker_as_content() {
        let root = temp_root("marker");
        let course = root.join("Course");
        touch(&course.join("01 Intro/01 welcome.mp4"));
        fs::write(
            course.join(COURSE_MARKER_FILE_NAME),
            r#"{"version":1,"identityId":"course-identity-1"}"#,
        )
        .expect("write marker");

        let (scanned, warnings) = scan_course(&course);
        let files = scanned
            .sections
            .iter()
            .flat_map(|section| section.files.iter())
            .collect::<Vec<_>>();

        assert_eq!(
            scanned.marker_identity_id.as_deref(),
            Some("course-identity-1")
        );
        assert!(warnings.is_empty());
        assert!(
            !files
                .iter()
                .any(|file| file.name.as_ref() == COURSE_MARKER_FILE_NAME)
        );

        cleanup(&root);
    }

    #[test]
    fn invalid_course_marker_produces_warning() {
        let root = temp_root("invalid-marker");
        let course = root.join("Course");
        touch(&course.join("01 Intro/01 welcome.mp4"));
        fs::write(course.join(COURSE_MARKER_FILE_NAME), r#"{"version":1}"#).expect("write marker");

        let (scanned, warnings) = scan_course(&course);

        assert_eq!(scanned.marker_identity_id, None);
        assert!(
            warnings
                .iter()
                .any(|warning| warning.contains("identityId is empty"))
        );

        cleanup(&root);
    }

    #[test]
    fn unsupported_course_marker_version_produces_warning() {
        let root = temp_root("unsupported-marker");
        let course = root.join("Course");
        touch(&course.join("01 Intro/01 welcome.mp4"));
        fs::write(
            course.join(COURSE_MARKER_FILE_NAME),
            r#"{"version":2,"identityId":"course-identity-1"}"#,
        )
        .expect("write marker");

        let (scanned, warnings) = scan_course(&course);

        assert_eq!(scanned.marker_identity_id, None);
        assert!(
            warnings
                .iter()
                .any(|warning| warning.contains("unsupported course marker version 2"))
        );

        cleanup(&root);
    }

    #[test]
    fn does_not_delete_existing_course_when_scan_finds_empty_root() {
        let root = temp_root("empty-root");
        touch(&root.join("download.part"));
        touch(&root.join("video.mp4.!qB"));

        let result = scan_library(&root.to_string_lossy());

        assert_eq!(result.scan_type, ScanType::Library);
        assert!(result.courses.is_empty());
        assert_eq!(result.warnings.len(), 2);

        cleanup(&root);
    }
}
