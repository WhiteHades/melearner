use jwalk::WalkDir;
use rayon::prelude::*;
use seahash::SeaHasher;
use serde::{Deserialize, Serialize};
use std::hash::Hasher;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

const VIDEO_EXTENSIONS: &[&str] = &["mp4", "mkv", "webm", "mov", "avi", "m4v"];
const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "aac", "m4a", "flac", "ogg"];
const DOCUMENT_EXTENSIONS: &[&str] = &["pdf", "txt", "md", "html", "docx"];
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

static HASHER_SEED: OnceLock<[u64; 4]> = OnceLock::new();

fn new_hasher() -> SeaHasher {
    let seed = HASHER_SEED.get_or_init(|| {
        use std::time::{SystemTime, UNIX_EPOCH};
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        let mut h = SeaHasher::new();
        h.write_u64(now);
        h.write_u64(now.wrapping_mul(0x9E3779B97F4A7C15));
        h.write_u64(now.rotate_left(17));
        let a = h.finish();
        h.write_u64(a.wrapping_add(0xDEADBEEF));
        [h.finish(), a, a.rotate_left(31), a.rotate_left(47)]
    });
    let mut h = SeaHasher::with_seeds(seed[0], seed[1], seed[2], seed[3]);
    h.write_u64(0xC0FFEE_BEEF);
    h
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

fn is_media_file(file_type: FileType) -> bool {
    matches!(
        file_type,
        FileType::Video | FileType::Audio | FileType::Document | FileType::Quiz
    )
}

fn read_dir_sorted(path: &Path) -> Vec<std::fs::DirEntry> {
    let mut entries: Vec<_> = std::fs::read_dir(path)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .collect();
    entries.sort_by(|a, b| a.file_name().cmp(&b.file_name()));
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
        .max_depth(1)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .map(|e| e.path())
        .filter(|p| !is_ignored(p))
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

    sections.sort_by(|a, b| a.order.cmp(&b.order).then_with(|| a.name.cmp(&b.name)));

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
            warnings: Box::new(["path does not exist".to_string()]),
        };
    }

    let entries = read_dir_sorted(&root);

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
        .map(|dir| scan_course(dir))
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
    Ok(tokio::task::spawn_blocking(move || scan_library(&path))
        .await
        .map_err(|e| format!("scan task failed: {e}"))?)
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
