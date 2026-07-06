use fff_search::{
    FFFMode, FilePicker, FilePickerOptions, FuzzySearchOptions, PaginationArgs, QueryParser,
};
use serde::Serialize;
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const SEARCH_LIMIT_MAX: usize = 500;

static LIBRARY_SEARCH_INDEX: OnceLock<Mutex<Option<LibrarySearchIndex>>> = OnceLock::new();

struct LibrarySearchIndex {
    root: PathBuf,
    picker: FilePicker,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LibrarySearchHit {
    path: String,
    relative_path: String,
    name: String,
    score: i32,
}

fn search_index() -> &'static Mutex<Option<LibrarySearchIndex>> {
    LIBRARY_SEARCH_INDEX.get_or_init(|| Mutex::new(None))
}

fn new_picker(root: &Path) -> Result<FilePicker, String> {
    FilePicker::new(FilePickerOptions {
        base_path: root.to_string_lossy().to_string(),
        enable_mmap_cache: false,
        enable_content_indexing: false,
        mode: FFFMode::Ai,
        cache_budget: None,
        watch: false,
        follow_symlinks: false,
        enable_fs_root_scanning: true,
        enable_home_dir_scanning: true,
    })
    .map_err(|err| format!("failed to create FFF search index: {err}"))
}

fn canonical_root(root: &Path) -> Result<PathBuf, String> {
    if !root.exists() {
        return Err(format!("library path does not exist: {}", root.display()));
    }
    if !root.is_dir() {
        return Err(format!(
            "library path is not a directory: {}",
            root.display()
        ));
    }
    root.canonicalize()
        .map_err(|err| format!("failed to resolve library path {}: {err}", root.display()))
}

fn canonical_search_path(path: &Path) -> Option<PathBuf> {
    path.is_file().then(|| path.canonicalize().ok()).flatten()
}

fn build_library_search_index(root: &Path, paths: &[String]) -> Result<LibrarySearchIndex, String> {
    let root = canonical_root(root)?;
    let mut picker = new_picker(&root)?;
    let mut seen = HashSet::new();

    for path in paths {
        let Some(path) = canonical_search_path(Path::new(path)) else {
            continue;
        };
        if !path.starts_with(&root) || !seen.insert(path.clone()) {
            continue;
        }
        if picker.add_new_file(&path).is_none() {
            return Err("FFF search index capacity was exhausted".to_string());
        }
    }

    Ok(LibrarySearchIndex { root, picker })
}

fn search_index_hits(
    index: &LibrarySearchIndex,
    query: &str,
    limit: usize,
) -> Vec<LibrarySearchHit> {
    let query = query.trim();
    if query.is_empty() {
        return Vec::new();
    }

    let parser = QueryParser::default();
    let parsed = parser.parse(query);
    let result = index.picker.fuzzy_search(
        &parsed,
        None,
        FuzzySearchOptions {
            max_threads: 0,
            current_file: None,
            project_path: Some(&index.root),
            combo_boost_score_multiplier: 0,
            min_combo_count: 0,
            pagination: PaginationArgs {
                offset: 0,
                limit: limit.min(SEARCH_LIMIT_MAX),
            },
        },
    );

    result
        .items
        .iter()
        .zip(result.scores.iter())
        .map(|(file, score)| {
            let absolute_path = file.absolute_path(&index.picker, &index.root);
            let relative_path = file.relative_path(&index.picker);
            let name = file.file_name(&index.picker);
            LibrarySearchHit {
                path: absolute_path.to_string_lossy().to_string(),
                relative_path,
                name,
                score: score.total,
            }
        })
        .collect()
}

#[tauri::command]
pub async fn index_library_search(root: String, paths: Vec<String>) -> Result<(), String> {
    let root = PathBuf::from(root);
    let index = tokio::task::spawn_blocking(move || build_library_search_index(&root, &paths))
        .await
        .map_err(|err| format!("FFF search index task failed: {err}"))??;

    let mut guard = search_index()
        .lock()
        .map_err(|_| "library search index lock is poisoned".to_string())?;
    *guard = Some(index);
    Ok(())
}

#[tauri::command]
pub fn search_library(
    query: String,
    limit: Option<usize>,
) -> Result<Vec<LibrarySearchHit>, String> {
    let guard = search_index()
        .lock()
        .map_err(|_| "library search index lock is poisoned".to_string())?;
    let Some(index) = guard.as_ref() else {
        return Ok(Vec::new());
    };

    Ok(search_index_hits(
        index,
        &query,
        limit.unwrap_or(50).min(SEARCH_LIMIT_MAX),
    ))
}

#[tauri::command]
pub fn clear_library_search() -> Result<(), String> {
    let mut guard = search_index()
        .lock()
        .map_err(|_| "library search index lock is poisoned".to_string())?;
    *guard = None;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    fn temp_root(name: &str) -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("melearner-fff-{name}-{suffix}"));
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
    fn fff_search_finds_indexed_nested_course_media() {
        let root = temp_root("nested-course-media");
        let welcome = root.join("DSA Hitesh Choudhary/01 - Intro/001 - Welcome.mp4");
        let heaps = root.join("DSA Ztm/10. Trees/14. Binary Heaps.mp4");
        touch(&welcome);
        touch(&heaps);

        let index = build_library_search_index(
            &root,
            &[
                welcome.to_string_lossy().to_string(),
                heaps.to_string_lossy().to_string(),
            ],
        )
        .expect("build FFF index");
        let hits = search_index_hits(&index, "binary heaps", 10);

        assert!(
            hits.iter()
                .any(|hit| hit.relative_path == "DSA Ztm/10. Trees/14. Binary Heaps.mp4"),
            "expected FFF search to find indexed course video, got {hits:?}"
        );

        cleanup(&root);
    }

    #[test]
    fn fff_search_ignores_paths_outside_library_root() {
        let root = temp_root("outside-root");
        let outside = temp_root("outside-file").join("Other/001 - Welcome.mp4");
        touch(&outside);

        let index = build_library_search_index(&root, &[outside.to_string_lossy().to_string()])
            .expect("build FFF index");

        assert!(search_index_hits(&index, "welcome", 10).is_empty());

        cleanup(&root);
        cleanup(outside.parent().and_then(Path::parent).unwrap_or(&outside));
    }

    #[test]
    fn fff_search_returns_empty_hits_for_blank_query() {
        let root = temp_root("blank-query");
        let welcome = root.join("Course/01 Intro/001 Welcome.mp4");
        touch(&welcome);

        let index = build_library_search_index(&root, &[welcome.to_string_lossy().to_string()])
            .expect("build FFF index");
        assert!(search_index_hits(&index, "   ", 10).is_empty());

        cleanup(&root);
    }

    #[test]
    fn fff_search_rejects_missing_library_path() {
        let root = temp_root("missing-root");
        cleanup(&root);

        let err = match build_library_search_index(&root, &[]) {
            Ok(_) => panic!("missing root should fail"),
            Err(err) => err,
        };
        assert!(err.contains("does not exist"));
    }

    #[test]
    fn fff_search_completes_quickly_on_small_library() {
        let root = temp_root("small-library");
        let welcome = root.join("Course/01 Intro/001 Welcome.mp4");
        touch(&welcome);

        let started = std::time::Instant::now();
        let index = build_library_search_index(&root, &[welcome.to_string_lossy().to_string()])
            .expect("build FFF index");
        let hits = search_index_hits(&index, "welcome", 10);

        assert!(!hits.is_empty());
        assert!(
            started.elapsed() < Duration::from_secs(1),
            "small FFF library search took {:?}",
            started.elapsed()
        );

        cleanup(&root);
    }
}
