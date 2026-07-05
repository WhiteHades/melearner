use jwalk::WalkDir;
use rayon::prelude::*;
use seahash::SeaHasher;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::hash::Hasher;
use std::path::{Path, PathBuf};

const VIDEO_EXTENSIONS: &[&str] = &["mp4", "mkv", "webm", "mov", "avi", "m4v"];
const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "aac", "m4a", "flac", "ogg"];
const DOCUMENT_EXTENSIONS: &[&str] =
    &["pdf", "txt", "md", "markdown", "html", "htm", "docx", "doc"];
const SUBTITLE_EXTENSIONS: &[&str] = &["srt", "vtt"];
const IGNORED_FOLDERS: &[&str] = &[".git", "node_modules", "__MACOSX", ".DS_Store", "Thumbs.db"];
const RESOURCE_FOLDERS: &[&str] = &["resources", "assets", "downloads", "extras", "materials"];
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
const DOWNLOAD_SIDECAR_SUFFIXES: &[&str] = &["aria2", "part", "partial", "crdownload", "download", "!qB"];
const WARNING_LIMIT: usize = 24;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileEntry {
    pub id: Box<str>,
    pub path: Box<str>,
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
    pub name: Box<str>,
    pub path: Box<str>,
    pub sections: Box<[SectionData]>,
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

fn has_download_sidecar(path: &Path) -> bool {
    let path_text = path.to_string_lossy();
    DOWNLOAD_SIDECAR_SUFFIXES
        .iter()
        .any(|suffix| PathBuf::from(format!("{path_text}.{suffix}")).exists())
}

fn push_warning(warnings: &mut Vec<String>, message: String) {
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

fn skip_file_reason(path: &Path, file_type: FileType) -> Option<String> {
    if is_partial_file(path) {
        return Some(format!("skipped incomplete download: {}", path.display()));
    }

    if (is_media_file(file_type) || file_type == FileType::Subtitle) && has_download_sidecar(path) {
        return Some(format!("skipped file with active download sidecar: {}", path.display()));
    }

    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if size == 0 && (is_media_file(file_type) || file_type == FileType::Subtitle) {
        return Some(format!("skipped empty learning item: {}", path.display()));
    }

    None
}

fn is_resource_folder(path: &Path) -> bool {
    let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    RESOURCE_FOLDERS.contains(&name.to_ascii_lowercase().as_str())
}

fn is_ignored_or_resource_path(path: &Path, base: &Path) -> bool {
    let relative = path.strip_prefix(base).unwrap_or(path);
    relative.components().any(|component| {
        let part = component.as_os_str().to_string_lossy();
        let lower = part.to_ascii_lowercase();
        part.starts_with('.')
            || IGNORED_FOLDERS.contains(&part.as_ref())
            || RESOURCE_FOLDERS.contains(&lower.as_str())
            || PARTIAL_FOLDER_NAMES.contains(&lower.as_str())
    })
}

fn is_media_file(file_type: FileType) -> bool {
    matches!(
        file_type,
        FileType::Video | FileType::Audio | FileType::Document | FileType::Quiz
    )
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

    ab.len().cmp(&bb.len())
}

fn path_name(path: &Path) -> String {
    path.file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default()
        .to_string()
}

fn read_dir_sorted(path: &Path) -> Vec<std::fs::DirEntry> {
    let mut entries: Vec<_> = std::fs::read_dir(path)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .collect();
    entries.sort_by(|a, b| natural_cmp(&path_name(&a.path()), &path_name(&b.path())));
    entries
}

fn file_entry(path: &Path, file_type: FileType) -> Option<FileEntry> {
    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .map(Box::from)
        .unwrap_or_default();
    Some(FileEntry {
        id: hash_path_to_id(path),
        path: path.to_string_lossy().into_owned().into_boxed_str(),
        name,
        file_type,
        size,
    })
}

fn scan_directory(dir: &Path) -> (Box<[FileEntry]>, Vec<String>) {
    let mut warnings = Vec::new();
    let files = WalkDir::new(dir)
        .skip_hidden(true)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .map(|e| e.path())
        .filter(|p| !is_ignored_or_resource_path(p, dir))
        .filter_map(|p| {
            let ft = get_file_type(&p);
            if let Some(reason) = skip_file_reason(&p, ft) {
                push_warning(&mut warnings, reason);
                return None;
            }
            (is_media_file(ft) || ft == FileType::Subtitle)
                .then(|| file_entry(&p, ft))
                .flatten()
        })
        .collect::<Vec<_>>();

    (files.into_boxed_slice(), warnings)
}

fn scan_course(course_path: &Path) -> (CourseData, Vec<String>) {
    let mut sections: Vec<SectionData> = Vec::new();
    let mut root_files: Vec<FileEntry> = Vec::new();
    let mut warnings = Vec::new();

    for (index, entry) in read_dir_sorted(course_path).iter().enumerate() {
        let path = entry.path();

        if path.is_dir() && is_partial_folder(&path) {
            push_warning(&mut warnings, format!("skipped incomplete folder: {}", path.display()));
            continue;
        }

        if is_ignored(&path) || is_resource_folder(&path) {
            continue;
        }

        if path.is_dir() {
            let (files, section_warnings) = scan_directory(&path);
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
        } else if path.is_file() {
            let file_type = get_file_type(&path);
            if let Some(reason) = skip_file_reason(&path, file_type) {
                push_warning(&mut warnings, reason);
                continue;
            }
            if is_media_file(file_type) || file_type == FileType::Subtitle {
                if let Some(entry) = file_entry(&path, file_type) {
                    root_files.push(entry);
                }
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

    (
        CourseData {
            id: hash_path_to_id(course_path),
            name: course_path
                .file_name()
                .and_then(|n| n.to_str())
                .map(Box::from)
                .unwrap_or_default(),
            path: course_path.to_string_lossy().into_owned().into_boxed_str(),
            sections: sections.into_boxed_slice(),
        },
        warnings,
    )
}

pub fn scan_library(root_path: &str) -> ScanResult {
    let root = PathBuf::from(root_path);

    if !root.exists() {
        return ScanResult {
            scan_type: ScanType::Library,
            courses: Box::new([]),
            warnings: Box::new([format!("path does not exist: {root_path}")]),
        };
    }

    if !root.is_dir() {
        return ScanResult {
            scan_type: ScanType::Library,
            courses: Box::new([]),
            warnings: Box::new([format!("not a directory: {root_path}")]),
        };
    }

    let entries = match std::fs::read_dir(&root) {
        Ok(entries) => entries.filter_map(|e| e.ok()).collect::<Vec<_>>(),
        Err(e) => {
            return ScanResult {
                scan_type: ScanType::Library,
                courses: Box::new([]),
                warnings: Box::new([format!("cannot read directory {}: {e}", root.display())]),
            };
        }
    };

    let mut warnings = Vec::new();
    let mut root_files_exist = false;
    let mut subdirs: Vec<PathBuf> = Vec::new();

    for entry in entries {
        let path = entry.path();
        if path.is_file() {
            let file_type = get_file_type(&path);
            if let Some(reason) = skip_file_reason(&path, file_type) {
                push_warning(&mut warnings, reason);
                continue;
            }
            if is_media_file(file_type) {
                root_files_exist = true;
            }
        } else if path.is_dir() {
            if is_partial_folder(&path) {
                push_warning(&mut warnings, format!("skipped incomplete folder: {}", path.display()));
                continue;
            }
            if is_ignored(&path) || is_resource_folder(&path) {
                continue;
            }
            subdirs.push(path);
        }
    }

    if root_files_exist && subdirs.is_empty() {
        let (course, course_warnings) = scan_course(&root);
        extend_warnings(&mut warnings, course_warnings);
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([course]),
            warnings: warnings.into_boxed_slice(),
        };
    }

    if root_files_exist && !subdirs.is_empty() {
        let (course, course_warnings) = scan_course(&root);
        extend_warnings(&mut warnings, course_warnings);
        push_warning(&mut warnings, "mixed content at root level".to_string());
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([course]),
            warnings: warnings.into_boxed_slice(),
        };
    }

    let scanned = subdirs
        .par_iter()
        .map(|dir| {
            std::panic::catch_unwind(|| scan_course(dir))
                .map_err(|_| format!("skipped course after scanner panic: {}", dir.display()))
        })
        .collect::<Vec<_>>();

    let mut courses = Vec::new();
    for result in scanned {
        match result {
            Ok((course, course_warnings)) => {
                extend_warnings(&mut warnings, course_warnings);
                if !course.sections.is_empty() {
                    courses.push(course);
                }
            }
            Err(message) => push_warning(&mut warnings, message),
        }
    }

    ScanResult {
        scan_type: ScanType::Library,
        courses: courses.into_boxed_slice(),
        warnings: warnings.into_boxed_slice(),
    }
}

#[tauri::command]
pub async fn scan_folder(path: String) -> Result<ScanResult, String> {
    use std::fs::OpenOptions;
    use std::io::Write;

    let path_for_log = path.clone();
    let log_path = std::env::var("HOME")
        .map(|h| {
            std::path::PathBuf::from(h)
                .join(".melearner")
                .join("scan.log")
        })
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearner-scan.log"));

    let _ = std::fs::create_dir_all(log_path.parent().unwrap());

    let log = move |msg: &str| {
        if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(&log_path) {
            let ts = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs_f64())
                .unwrap_or(0.0);
            let _ = writeln!(f, "[{ts}] {msg}");
        }
    };

    log(&format!("scan_folder start: path={path_for_log}"));

    let log_for_thread = log.clone();
    let result = tokio::task::spawn_blocking(move || {
        log_for_thread(&format!("scan_library called: path={path_for_log}"));
        let r = scan_library(&path_for_log);
        log_for_thread(&format!(
            "scan_library returned: scan_type={:?} courses={} warnings={}",
            r.scan_type,
            r.courses.len(),
            r.warnings.join(" | ")
        ));
        r
    })
    .await;

    match result {
        Ok(r) => {
            log(&format!("scan_folder done: {} courses", r.courses.len()));
            Ok(r)
        }
        Err(e) => {
            let msg = format!("scan task panicked: {e}");
            log(&msg);
            Err(msg)
        }
    }
}

#[tauri::command]
pub async fn get_file_info(path: String) -> Result<FileEntry, String> {
    let p = PathBuf::from(&path);
    if !p.exists() {
        return Err("file does not exist".to_string());
    }

    let file_type = get_file_type(&p);
    file_entry(&p, file_type).ok_or_else(|| "failed to read file metadata".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
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

    #[test]
    fn scans_course_subdirectories_and_filters_non_learning_folders() {
        let root = temp_root("library");
        touch(&root.join("Rust Basics/01 Intro/01 welcome.mp4"));
        touch(&root.join("Rust Basics/01 Intro/01 welcome.en.srt"));
        touch(&root.join("Rust Basics/resources/ignored.pdf"));
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
        assert!(all_files
            .iter()
            .any(|file| file.name.as_ref() == "01 welcome.en.srt"
                && file.file_type == FileType::Subtitle));
        assert!(all_files
            .iter()
            .any(|file| file.name.as_ref() == "guide.markdown"
                && file.file_type == FileType::Document));
        assert!(
            all_files
                .iter()
                .any(|file| file.name.as_ref() == "legacy.doc"
                    && file.file_type == FileType::Document)
        );
        assert!(!all_files
            .iter()
            .any(|file| file.name.as_ref() == "ignored.pdf"));
        assert!(!all_files
            .iter()
            .any(|file| file.name.as_ref() == "ignored.mp4"));

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
        assert!(course.sections[0]
            .files
            .iter()
            .any(
                |file| file.name.as_ref() == "00 overview.mp4" && file.file_type == FileType::Video
            ));
        assert!(course.sections[0]
            .files
            .iter()
            .any(|file| file.name.as_ref() == "00 overview.srt"
                && file.file_type == FileType::Subtitle));

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
        assert!(files.iter().any(|file| file.name.as_ref() == "01 ready.mp4"));
        assert!(!files.iter().any(|file| file.name.as_ref() == "02 active.mp4"));
        assert!(!files.iter().any(|file| file.name.as_ref() == "03 browser.mp4.crdownload"));
        assert!(result
            .warnings
            .iter()
            .any(|warning| warning.contains("active download sidecar")));
        assert!(result
            .warnings
            .iter()
            .any(|warning| warning.contains("incomplete download")));
        assert!(result
            .warnings
            .iter()
            .any(|warning| warning.contains("incomplete folder")));

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
