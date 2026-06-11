use jwalk::WalkDir;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

const VIDEO_EXTENSIONS: &[&str] = &["mp4", "mkv", "webm", "mov", "avi", "m4v"];
const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "aac", "m4a", "flac", "ogg"];
const DOCUMENT_EXTENSIONS: &[&str] = &["pdf", "txt", "md", "html", "docx"];
const SUBTITLE_EXTENSIONS: &[&str] = &["srt", "vtt"];
const IGNORED_FOLDERS: &[&str] = &[".git", "node_modules", "__MACOSX", ".DS_Store", "Thumbs.db"];
const RESOURCE_FOLDERS: &[&str] = &["resources", "assets", "downloads", "extras", "materials"];
const PARTIAL_EXTENSIONS: &[&str] = &["part", "crdownload", "download"];

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileEntry {
    pub id: String,
    pub path: String,
    pub name: String,
    pub file_type: FileType,
    pub size: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
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
    pub id: String,
    pub name: String,
    pub files: Vec<FileEntry>,
    pub order: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CourseData {
    pub id: String,
    pub name: String,
    pub path: String,
    pub sections: Vec<SectionData>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ScanType {
    Library,
    SingleCourse,
    Bundle,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScanResult {
    pub scan_type: ScanType,
    pub courses: Vec<CourseData>,
    pub warnings: Vec<String>,
}

fn get_file_type(path: &Path) -> FileType {
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| e.to_lowercase())
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
        .map(|n| n.to_lowercase())
        .unwrap_or_default();

    if name.contains("quiz") || name.contains("test") || name.contains("exam") {
        return FileType::Quiz;
    }

    FileType::Unknown
}

fn is_ignored(path: &Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(|name| {
            name.starts_with('.') || IGNORED_FOLDERS.contains(&name)
        })
        .unwrap_or(false)
}

fn is_resource_folder(path: &Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(|name| RESOURCE_FOLDERS.contains(&name.to_lowercase().as_str()))
        .unwrap_or(false)
}

fn is_media_file(file_type: &FileType) -> bool {
    matches!(file_type, FileType::Video | FileType::Audio | FileType::Document | FileType::Quiz)
}

fn generate_id(path: &Path) -> String {
    use std::collections::hash_map::DefaultHasher;
    use std::hash::{Hash, Hasher};
    let mut hasher = DefaultHasher::new();
    path.to_string_lossy().hash(&mut hasher);
    format!("{:x}", hasher.finish())
}

fn generate_id_from_parts(parts: &[&str]) -> String {
    use std::collections::hash_map::DefaultHasher;
    use std::hash::{Hash, Hasher};
    let mut hasher = DefaultHasher::new();
    for part in parts {
        part.hash(&mut hasher);
    }
    format!("{:x}", hasher.finish())
}

fn scan_directory(dir: &Path) -> Vec<FileEntry> {
    WalkDir::new(dir)
        .skip_hidden(true)
        .max_depth(1)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .filter(|e| !is_ignored(&e.path()))
        .map(|e| {
            let path = e.path();
            let file_type = get_file_type(&path);
            let size = e.metadata().map(|m| m.len()).unwrap_or(0);
            FileEntry {
                id: generate_id(&path),
                path: path.to_string_lossy().to_string(),
                name: path.file_name().unwrap_or_default().to_string_lossy().to_string(),
                file_type,
                size,
            }
        })
        .filter(|e| is_media_file(&e.file_type) || e.file_type == FileType::Subtitle)
        .collect()
}

fn scan_course(course_path: &Path) -> CourseData {
    let mut sections: Vec<SectionData> = Vec::new();
    let mut root_files: Vec<FileEntry> = Vec::new();

    let entries: Vec<_> = std::fs::read_dir(course_path)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .collect();

    let mut entries = entries;
    entries.sort_by(|a, b| a.file_name().cmp(&b.file_name()));

    for (index, entry) in entries.iter().enumerate() {
        let path = entry.path();
        
        if is_ignored(&path) || is_resource_folder(&path) {
            continue;
        }

        if path.is_dir() {
            let files = scan_directory(&path);
            if !files.is_empty() {
                sections.push(SectionData {
                    id: generate_id(&path),
                    name: path.file_name().unwrap_or_default().to_string_lossy().to_string(),
                    files,
                    order: index,
                });
            }
        } else if path.is_file() {
            let file_type = get_file_type(&path);
            if is_media_file(&file_type) || file_type == FileType::Subtitle {
                let size = entry.metadata().map(|m| m.len()).unwrap_or(0);
                root_files.push(FileEntry {
                    id: generate_id(&path),
                    path: path.to_string_lossy().to_string(),
                    name: path.file_name().unwrap_or_default().to_string_lossy().to_string(),
                    file_type,
                    size,
                });
            }
        }
    }

    if !root_files.is_empty() {
        sections.insert(0, SectionData {
            id: generate_id_from_parts(&[
                &course_path.to_string_lossy(),
                "_introduction",
            ]),
            name: "introduction".to_string(),
            files: root_files,
            order: 0,
        });
        for (i, section) in sections.iter_mut().skip(1).enumerate() {
            section.order = i + 1;
        }
    }

    sections.sort_by(|a, b| a.order.cmp(&b.order).then_with(|| a.name.cmp(&b.name)));

    CourseData {
        id: generate_id(course_path),
        name: course_path.file_name().unwrap_or_default().to_string_lossy().to_string(),
        path: course_path.to_string_lossy().to_string(),
        sections,
    }
}

pub fn scan_library(root_path: &str) -> ScanResult {
    let root = PathBuf::from(root_path);
    let mut warnings: Vec<String> = Vec::new();

    if !root.exists() {
        return ScanResult {
            scan_type: ScanType::Library,
            courses: vec![],
            warnings: vec!["path does not exist".to_string()],
        };
    }

    let root_files: Vec<_> = std::fs::read_dir(&root)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_file())
        .filter(|e| {
            let ft = get_file_type(&e.path());
            is_media_file(&ft)
        })
        .collect();

    let subdirs: Vec<PathBuf> = std::fs::read_dir(&root)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .filter(|e| !is_ignored(&e.path()))
        .map(|e| e.path())
        .collect();

    if !root_files.is_empty() && subdirs.is_empty() {
        let course = scan_course(&root);
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: vec![course],
            warnings,
        };
    }

    if !root_files.is_empty() && !subdirs.is_empty() {
        warnings.push("mixed content at root level".to_string());
        let course = scan_course(&root);
        return ScanResult {
            scan_type: ScanType::SingleCourse,
            courses: vec![course],
            warnings,
        };
    }

    let courses: Vec<CourseData> = subdirs
        .par_iter()
        .map(|dir| scan_course(dir))
        .filter(|c| !c.sections.is_empty())
        .collect();

    ScanResult {
        scan_type: ScanType::Library,
        courses,
        warnings,
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
    let size = std::fs::metadata(&p).map(|m| m.len()).unwrap_or(0);
    
    Ok(FileEntry {
        id: generate_id(&p),
        path,
        name: p.file_name().unwrap_or_default().to_string_lossy().to_string(),
        file_type,
        size,
    })
}
