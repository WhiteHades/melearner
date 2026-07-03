use jwalk::WalkDir;
use rayon::prelude::*;
use seahash::SeaHasher;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::hash::Hasher;
use std::path::{Path, PathBuf};

const VIDEO_EXTENSIONS: &[&str] = &["mp4", "mkv", "webm", "mov", "avi", "m4v"];
const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "aac", "m4a", "flac", "ogg"];
const DOCUMENT_EXTENSIONS: &[&str] = &["pdf", "txt", "md", "markdown", "html", "htm", "docx", "doc"];
const SUBTITLE_EXTENSIONS: &[&str] = &["srt", "vtt"];
const IGNORED_FOLDERS: &[&str] = &[".git", "node_modules", "__MACOSX", ".DS_Store", "Thumbs.db"];
const RESOURCE_FOLDERS: &[&str] = &["resources", "assets", "downloads", "extras", "materials"];
const PARTIAL_EXTENSIONS: &[&str] = &["part", "crdownload", "download"];

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
    Bundle,
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

fn scan_directory(dir: &Path) -> Box<[FileEntry]> {
    WalkDir::new(dir)
        .skip_hidden(true)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .map(|e| e.path())
        .filter(|p| !is_ignored_or_resource_path(p, dir))
        .filter_map(|p| {
            let ft = get_file_type(&p);
            (is_media_file(ft) || ft == FileType::Subtitle)
                .then(|| file_entry(&p, ft))
                .flatten()
        })
        .collect::<Vec<_>>()
        .into_boxed_slice()
}

fn scan_course(course_path: &Path) -> CourseData {
    let mut sections: Vec<SectionData> = Vec::new();
    let mut root_files: Vec<FileEntry> = Vec::new();

    for (index, entry) in read_dir_sorted(course_path).iter().enumerate() {
        let path = entry.path();

        if is_ignored(&path) || is_resource_folder(&path) {
            continue;
        }

        if path.is_dir() {
            let files = scan_directory(&path);
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
                id: hash_str_to_id(&format!(
                    "{}_introduction",
                    course_path.to_string_lossy()
                )),
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

    CourseData {
        id: hash_path_to_id(course_path),
        name: course_path
            .file_name()
            .and_then(|n| n.to_str())
            .map(Box::from)
            .unwrap_or_default(),
        path: course_path.to_string_lossy().into_owned().into_boxed_str(),
        sections: sections.into_boxed_slice(),
    }
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

    let root_files_exist = entries
        .iter()
        .filter(|e| e.path().is_file())
        .any(|e| is_media_file(get_file_type(&e.path())));

    let subdirs: Vec<PathBuf> = entries
        .into_iter()
        .filter(|e| e.path().is_dir())
        .map(|e| e.path())
        .filter(|p| !is_ignored(p))
        .collect();

    if root_files_exist && subdirs.is_empty() {
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([scan_course(&root)]),
            warnings: Box::new([]),
        };
    }

    if root_files_exist && !subdirs.is_empty() {
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: Box::new([scan_course(&root)]),
            warnings: Box::new(["mixed content at root level".to_string()]),
        };
    }

    let courses: Box<[CourseData]> = subdirs
        .par_iter()
        .filter_map(|dir| std::panic::catch_unwind(|| scan_course(dir)).ok())
        .filter(|c| !c.sections.is_empty())
        .collect::<Vec<_>>()
        .into_boxed_slice();

    ScanResult {
        scan_type: ScanType::Library,
        courses,
        warnings: Box::new([]),
    }
}

#[tauri::command]
pub async fn scan_folder(path: String) -> Result<ScanResult, String> {
    use std::io::Write;
    use std::fs::OpenOptions;

    let path_for_log = path.clone();
    let log_path = std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".melearner").join("scan.log"))
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
