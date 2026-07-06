use fff_search::case_insensitive_memmem;
use fff_search::{FuzzyQuery, QueryParser};
use serde::Serialize;
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const SEARCH_LIMIT_MAX: usize = 500;

static LIBRARY_SEARCH_INDEX: OnceLock<Mutex<Option<LibrarySearchIndex>>> = OnceLock::new();

struct LibrarySearchIndex {
    entries: Box<[LibrarySearchEntry]>,
}

struct LibrarySearchEntry {
    path: String,
    relative_path: String,
    name: String,
    searchable: String,
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

fn normalized_relative_path(path: &Path, root: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .components()
        .map(|component| component.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
}

fn file_name(path: &Path) -> String {
    path.file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default()
        .to_string()
}

fn build_library_search_index(root: &Path, paths: &[String]) -> Result<LibrarySearchIndex, String> {
    let root = canonical_root(root)?;
    let mut seen = HashSet::new();
    let mut entries = Vec::new();

    for path in paths {
        let Some(path) = canonical_search_path(Path::new(path)) else {
            continue;
        };
        if !path.starts_with(&root) || !seen.insert(path.clone()) {
            continue;
        }

        let relative_path = normalized_relative_path(&path, &root);
        let name = file_name(&path);
        let searchable = format!("{relative_path} {name}");
        entries.push(LibrarySearchEntry {
            path: path.to_string_lossy().to_string(),
            relative_path,
            name,
            searchable,
        });
    }

    Ok(LibrarySearchIndex {
        entries: entries.into_boxed_slice(),
    })
}

fn query_parts(query: &str) -> Vec<String> {
    let parser = QueryParser::default();
    let parsed = parser.parse(query);

    match parsed.fuzzy_query {
        FuzzyQuery::Empty => Vec::new(),
        FuzzyQuery::Text(text) => vec![text.to_ascii_lowercase()],
        FuzzyQuery::Parts(parts) => parts
            .iter()
            .filter(|part| !part.is_empty())
            .map(|part| part.to_ascii_lowercase())
            .collect(),
    }
}

fn contains_part(haystack: &str, needle_lower: &str) -> bool {
    case_insensitive_memmem::search(haystack.as_bytes(), needle_lower.as_bytes())
}

fn score_entry(entry: &LibrarySearchEntry, parts: &[String]) -> Option<i32> {
    if parts.is_empty() {
        return None;
    }

    if !parts
        .iter()
        .all(|part| contains_part(&entry.searchable, part))
    {
        return None;
    }

    let mut score = 1000_i32;
    for part in parts {
        if contains_part(&entry.name, part) {
            score += 400;
        }
        if entry.name.to_ascii_lowercase().starts_with(part) {
            score += 300;
        }
    }

    Some(score.saturating_sub(entry.relative_path.len().min(500) as i32))
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

    let parts = query_parts(query);
    let mut hits = index
        .entries
        .iter()
        .filter_map(|entry| {
            score_entry(entry, &parts).map(|score| LibrarySearchHit {
                path: entry.path.clone(),
                relative_path: entry.relative_path.clone(),
                name: entry.name.clone(),
                score,
            })
        })
        .collect::<Vec<_>>();

    hits.sort_by(|a, b| b.score.cmp(&a.score).then_with(|| a.name.cmp(&b.name)));
    hits.truncate(limit.min(SEARCH_LIMIT_MAX));
    hits
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
    use std::time::{SystemTime, UNIX_EPOCH};

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
        let outside_root = temp_root("outside-file");
        let outside = outside_root.join("Other/001 - Welcome.mp4");
        touch(&outside);

        let index = build_library_search_index(&root, &[outside.to_string_lossy().to_string()])
            .expect("build FFF index");

        assert!(search_index_hits(&index, "welcome", 10).is_empty());

        cleanup(&root);
        cleanup(&outside_root);
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
}
