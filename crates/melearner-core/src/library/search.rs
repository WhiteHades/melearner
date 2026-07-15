use std::cmp::Reverse;
use std::collections::BTreeMap;
use std::path::Path;

use fff_query_parser::{FuzzyQuery, QueryParser};
use futures_util::TryStreamExt;
use serde::Serialize;
use sqlx::{Connection, Row, Sqlite, Transaction};

use super::{LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE};
use crate::{ML_MAX_SEARCH_QUERY_BYTES, MutationControl};

pub(super) struct SearchIndex {
    revision: u64,
    entries: Vec<SearchEntry>,
}

struct SearchEntry {
    id: String,
    course_id: String,
    course_name: String,
    section_id: String,
    section_name: String,
    name: String,
    name_folded: String,
    path: String,
    relative_path: String,
    kind: String,
    searchable: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchIndexReady {
    pub(crate) index_revision: u64,
    pub(crate) entry_count: u64,
}

#[derive(Debug)]
pub(crate) struct SearchPageInput {
    pub(crate) expected_index_revision: u64,
    pub(crate) query_id: u64,
    pub(crate) query: String,
    pub(crate) offset: u64,
    pub(crate) limit: u32,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchHit {
    pub(crate) id: String,
    pub(crate) course_id: String,
    pub(crate) course_name: String,
    pub(crate) section_id: String,
    pub(crate) section_name: String,
    pub(crate) name: String,
    pub(crate) path: String,
    pub(crate) relative_path: String,
    pub(crate) kind: String,
    pub(crate) score: i32,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchPage {
    pub(crate) query_id: u64,
    pub(crate) index_revision: u64,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) rows: Vec<SearchHit>,
}

impl LibraryDatabase {
    pub(crate) async fn rebuild_search_index(
        &mut self,
        expected_revision: u64,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<SearchIndexReady, LibraryError> {
        self.require_revision(expected_revision)?;
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }

        let root = self
            .library_path
            .as_deref()
            .filter(|path| !path.is_empty())
            .and_then(|path| std::fs::canonicalize(path).ok());
        let entries = if let Some(root) = root {
            let mut transaction = self.connection.begin().await?;
            let entries = load_entries(&mut transaction, &root, control).await;
            match entries {
                Ok(entries) => {
                    transaction.commit().await?;
                    entries
                }
                Err(error) => {
                    transaction.rollback().await?;
                    return Err(error);
                }
            }
        } else {
            Vec::new()
        };
        let entry_count = u64::try_from(entries.len())
            .map_err(|_| LibraryError::Database("search index exceeds u64".to_string()))?;
        let ready = SearchIndexReady {
            index_revision: self.revision,
            entry_count,
        };
        if !serde_json::to_vec(&ready).is_ok_and(|payload| payload.len() <= max_payload_bytes) {
            return Err(LibraryError::ResponseTooLarge {
                limit: max_payload_bytes,
            });
        }
        if control.is_cancelled() || !control.begin_commit() {
            return Err(LibraryError::Cancelled);
        }
        self.search_index = Some(SearchIndex {
            revision: self.revision,
            entries,
        });
        Ok(ready)
    }

    pub(crate) fn search_page(
        &self,
        input: SearchPageInput,
        control: &MutationControl,
    ) -> Result<SearchPage, LibraryError> {
        if input.query.len() > ML_MAX_SEARCH_QUERY_BYTES as usize || input.query.contains('\0') {
            return Err(LibraryError::InvalidSearchQuery);
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&input.limit) {
            return Err(LibraryError::InvalidPageSize { limit: input.limit });
        }
        i64::try_from(input.offset).map_err(|_| LibraryError::InvalidOffset {
            offset: input.offset,
        })?;
        let offset = usize::try_from(input.offset).map_err(|_| LibraryError::InvalidOffset {
            offset: input.offset,
        })?;
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }
        let actual_revision = self.search_index.as_ref().map(|index| index.revision);
        let Some(index) = self
            .search_index
            .as_ref()
            .filter(|index| index.revision == input.expected_index_revision)
        else {
            return Err(LibraryError::StaleSearchIndex {
                expected: input.expected_index_revision,
                actual: actual_revision,
            });
        };

        let parts = query_parts(&input.query);
        let mut hits = BTreeMap::new();
        if !parts.is_empty() {
            for entry in &index.entries {
                if control.is_cancelled() {
                    return Err(LibraryError::Cancelled);
                }
                if let Some(score) = score_entry(entry, &parts) {
                    hits.insert(
                        (
                            Reverse(score),
                            entry.name.as_str(),
                            entry.relative_path.as_str(),
                            entry.id.as_str(),
                        ),
                        (entry, score),
                    );
                }
            }
        }
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }
        let total = u64::try_from(hits.len())
            .map_err(|_| LibraryError::Database("search results exceed u64".to_string()))?;
        let start = offset.min(hits.len());
        let end = start.saturating_add(input.limit as usize).min(hits.len());
        let rows = hits
            .iter()
            .skip(start)
            .take(end - start)
            .map(|(_, (entry, score))| entry.hit(*score))
            .collect();
        Ok(SearchPage {
            query_id: input.query_id,
            index_revision: index.revision,
            offset: input.offset,
            total,
            rows,
        })
    }

    pub(super) fn invalidate_search_index(&mut self) {
        self.search_index = None;
    }
}

impl SearchEntry {
    fn hit(&self, score: i32) -> SearchHit {
        SearchHit {
            id: self.id.clone(),
            course_id: self.course_id.clone(),
            course_name: self.course_name.clone(),
            section_id: self.section_id.clone(),
            section_name: self.section_name.clone(),
            name: self.name.clone(),
            path: self.path.clone(),
            relative_path: self.relative_path.clone(),
            kind: self.kind.clone(),
            score,
        }
    }
}

async fn load_entries(
    transaction: &mut Transaction<'_, Sqlite>,
    root: &Path,
    control: &MutationControl,
) -> Result<Vec<SearchEntry>, LibraryError> {
    let mut rows = sqlx::query(
        "SELECT lessons.id,
                lessons.course_id,
                courses.name AS course_name,
                lessons.section_id,
                sections.name AS section_name,
                lessons.name,
                lessons.path,
                lessons.relative_path,
                lessons.type
         FROM lessons
         INNER JOIN courses ON courses.id = lessons.course_id
         INNER JOIN sections ON sections.id = lessons.section_id
                            AND sections.course_id = lessons.course_id
         WHERE courses.missing_since IS NULL
         ORDER BY lessons.id",
    )
    .fetch(&mut **transaction);
    let mut entries = Vec::new();
    while let Some(row) = rows.try_next().await? {
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }
        let path: String = row.try_get("path")?;
        let path_value = Path::new(&path);
        if !path_value.is_file() {
            continue;
        }
        let Ok(canonical_path) = std::fs::canonicalize(path_value) else {
            continue;
        };
        if !canonical_path.starts_with(root) {
            continue;
        }
        let name: String = row.try_get("name")?;
        let course_name: String = row.try_get("course_name")?;
        let section_name: String = row.try_get("section_name")?;
        let relative_path: String = row.try_get("relative_path")?;
        let mut searchable = String::new();
        append_search_text(&mut searchable, &course_name);
        append_search_text(&mut searchable, &section_name);
        append_search_text(&mut searchable, &name);
        append_search_text(&mut searchable, &relative_path);
        append_search_text(&mut searchable, file_name(&path));
        entries.push(SearchEntry {
            id: row.try_get("id")?,
            course_id: row.try_get("course_id")?,
            course_name,
            section_id: row.try_get("section_id")?,
            section_name,
            name_folded: name.to_ascii_lowercase(),
            name,
            path,
            relative_path,
            kind: row.try_get("type")?,
            searchable: searchable.to_ascii_lowercase(),
        });
    }
    drop(rows);
    Ok(entries)
}

fn append_search_text(searchable: &mut String, value: &str) {
    let value = value.trim();
    if value.is_empty() {
        return;
    }
    if !searchable.is_empty() {
        searchable.push(' ');
    }
    searchable.push_str(value);
}

fn file_name(path: &str) -> &str {
    path.rsplit(['/', '\\']).next().unwrap_or_default()
}

fn query_parts(query: &str) -> Vec<String> {
    let parsed = QueryParser::default().parse(query.trim());
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

fn score_entry(entry: &SearchEntry, parts: &[String]) -> Option<i32> {
    if !parts.iter().all(|part| entry.searchable.contains(part)) {
        return None;
    }

    let mut score = 1000_i32;
    for part in parts {
        if entry.name_folded.contains(part) {
            score += 400;
        }
        if entry.name_folded.starts_with(part) {
            score += 300;
        }
    }
    Some(score.saturating_sub(entry.relative_path.len().min(500) as i32))
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::path::{Path, PathBuf};
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use super::{SearchEntry, SearchIndex, SearchPageInput};
    use crate::library::progress::ProgressInput;
    use crate::library::{LibraryDatabase, LibraryError};
    use crate::{ML_MAX_SEARCH_QUERY_BYTES, MutationControl, next_library_revision};

    struct SearchFixture {
        _temp: tempfile::TempDir,
        root: PathBuf,
        library: LibraryDatabase,
    }

    struct LessonSeed<'a> {
        id: &'a str,
        course_id: &'a str,
        name: &'a str,
        path: &'a Path,
        relative_path: &'a str,
        kind: &'a str,
        order: i64,
    }

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build search test runtime")
            .block_on(future)
    }

    fn touch(path: &Path) {
        std::fs::create_dir_all(path.parent().expect("search fixture file has a parent"))
            .expect("create search fixture directory");
        std::fs::write(path, b"fixture").expect("write search fixture file");
    }

    async fn insert_lesson(library: &mut LibraryDatabase, lesson: LessonSeed<'_>) {
        sqlx::query(
            "INSERT INTO lessons (
                 id, course_id, section_id, name, path, relative_path, type,
                 duration, watched_time, completed, order_index, last_position
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 120, 0, 0, ?8, 0)",
        )
        .bind(lesson.id)
        .bind(lesson.course_id)
        .bind(format!("section-{}", lesson.course_id))
        .bind(lesson.name)
        .bind(lesson.path.to_string_lossy().as_ref())
        .bind(lesson.relative_path)
        .bind(lesson.kind)
        .bind(lesson.order)
        .execute(&mut library.connection)
        .await
        .expect("insert search fixture lesson");
    }

    async fn search_fixture() -> SearchFixture {
        let temp = tempfile::tempdir().expect("create search fixture");
        let root = temp.path().join("Library");
        let outside = temp.path().join("Outside");
        let state = temp.path().join("State");
        std::fs::create_dir_all(&root).expect("create search root");
        std::fs::create_dir_all(&outside).expect("create outside root");
        let mut library = LibraryDatabase::open_current(
            &state,
            next_library_revision().expect("allocate search fixture revision"),
            Arc::new(AtomicBool::new(false)),
        )
        .await
        .expect("open search fixture");

        let course_path = root.join("Systems");
        let missing_path = root.join("Missing");
        let outside_course_path = outside.join("Outside Course");
        for (id, identity_id, name, path, missing_since) in [
            (
                "course",
                "identity-course",
                "Systems",
                course_path.as_path(),
                None,
            ),
            (
                "missing",
                "identity-missing",
                "Missing",
                missing_path.as_path(),
                Some("2026-07-15T00:00:00Z"),
            ),
            (
                "outside",
                "identity-outside",
                "Outside",
                outside_course_path.as_path(),
                None,
            ),
        ] {
            sqlx::query(
                "INSERT INTO courses (
                     id, identity_id, name, path, fingerprint, last_scanned_at, missing_since
                 ) VALUES (?1, ?2, ?3, ?4, ?5, '2026-07-15T00:00:00Z', ?6)",
            )
            .bind(id)
            .bind(identity_id)
            .bind(name)
            .bind(path.to_string_lossy().as_ref())
            .bind(format!("fingerprint-{id}"))
            .bind(missing_since)
            .execute(&mut library.connection)
            .await
            .expect("insert search fixture course");
            sqlx::query(
                "INSERT INTO sections (id, course_id, name, order_index)
                 VALUES (?1, ?2, 'Core Concepts', 0)",
            )
            .bind(format!("section-{id}"))
            .bind(id)
            .execute(&mut library.connection)
            .await
            .expect("insert search fixture section");
        }

        let heaps = course_path.join("Core Concepts/14 Binary Heaps.mp4");
        let notes = course_path.join("Core Concepts/notes.pdf");
        let lecture_a = course_path.join("Core Concepts/A Lecture.mp4");
        let lecture_b = course_path.join("Core Concepts/B Lecture.mp4");
        let missing = missing_path.join("Core Concepts/Hidden Lesson.mp4");
        let outside_file = outside_course_path.join("Core Concepts/Outside Lesson.mp4");
        for path in [
            &heaps,
            &notes,
            &lecture_a,
            &lecture_b,
            &missing,
            &outside_file,
        ] {
            touch(path);
        }
        for (id, course_id, name, path, relative_path, kind, order) in [
            (
                "heaps",
                "course",
                "Binary Heaps",
                heaps.as_path(),
                "Core Concepts/14 Binary Heaps.mp4",
                "video",
                0,
            ),
            (
                "notes",
                "course",
                "Notes",
                notes.as_path(),
                "Core Concepts/notes.pdf",
                "document",
                1,
            ),
            (
                "lecture-a",
                "course",
                "Lecture",
                lecture_a.as_path(),
                "Core Concepts/A Lecture.mp4",
                "video",
                2,
            ),
            (
                "lecture-b",
                "course",
                "Lecture",
                lecture_b.as_path(),
                "Core Concepts/B Lecture.mp4",
                "video",
                3,
            ),
            (
                "hidden",
                "missing",
                "Hidden Lesson",
                missing.as_path(),
                "Core Concepts/Hidden Lesson.mp4",
                "video",
                0,
            ),
            (
                "outside-lesson",
                "outside",
                "Outside Lesson",
                outside_file.as_path(),
                "Core Concepts/Outside Lesson.mp4",
                "video",
                0,
            ),
        ] {
            insert_lesson(
                &mut library,
                LessonSeed {
                    id,
                    course_id,
                    name,
                    path,
                    relative_path,
                    kind,
                    order,
                },
            )
            .await;
        }

        sqlx::query("INSERT INTO app_settings (key, value) VALUES ('libraryPath', ?1)")
            .bind(root.to_string_lossy().as_ref())
            .execute(&mut library.connection)
            .await
            .expect("store search library path");
        library.library_path = Some(root.to_string_lossy().into_owned());

        SearchFixture {
            _temp: temp,
            root,
            library,
        }
    }

    fn query_input(
        revision: u64,
        query_id: u64,
        query: &str,
        offset: u64,
        limit: u32,
    ) -> SearchPageInput {
        SearchPageInput {
            expected_index_revision: revision,
            query_id,
            query: query.to_string(),
            offset,
            limit,
        }
    }

    #[test]
    fn rebuilt_search_returns_current_lesson_metadata() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let revision = fixture.library.revision();
            let ready = fixture
                .library
                .rebuild_search_index(revision, usize::MAX, &MutationControl::new())
                .await
                .expect("rebuild search index");
            assert_eq!(ready.index_revision, revision);
            assert_eq!(ready.entry_count, 4);

            let page = fixture
                .library
                .search_page(
                    query_input(revision, 7, "binary heaps", 0, 10),
                    &MutationControl::new(),
                )
                .expect("search binary heaps");
            assert_eq!(page.query_id, 7);
            assert_eq!(page.index_revision, revision);
            assert_eq!(page.total, 1);
            assert_eq!(page.rows[0].id, "heaps");
            assert_eq!(page.rows[0].course_id, "course");
            assert_eq!(page.rows[0].course_name, "Systems");
            assert_eq!(page.rows[0].section_id, "section-course");
            assert_eq!(page.rows[0].section_name, "Core Concepts");
            assert_eq!(
                page.rows[0].relative_path,
                "Core Concepts/14 Binary Heaps.mp4"
            );
            assert_eq!(page.rows[0].kind, "video");
            assert!(page.rows[0].score > 0);

            for excluded in ["hidden lesson", "outside lesson"] {
                assert_eq!(
                    fixture
                        .library
                        .search_page(
                            query_input(revision, 8, excluded, 0, 10),
                            &MutationControl::new(),
                        )
                        .expect("search excluded lesson")
                        .total,
                    0
                );
            }
        });
    }

    #[test]
    fn search_pages_are_stable_and_whitespace_is_empty() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let revision = fixture.library.revision();
            fixture
                .library
                .rebuild_search_index(revision, usize::MAX, &MutationControl::new())
                .await
                .expect("rebuild search index");

            let first = fixture
                .library
                .search_page(
                    query_input(revision, 11, "lecture", 0, 1),
                    &MutationControl::new(),
                )
                .expect("load first search page");
            let second = fixture
                .library
                .search_page(
                    query_input(revision, 11, "lecture", 1, 1),
                    &MutationControl::new(),
                )
                .expect("load second search page");
            assert_eq!(first.total, 2);
            assert_eq!(first.rows[0].id, "lecture-a");
            assert_eq!(second.total, 2);
            assert_eq!(second.rows[0].id, "lecture-b");

            let empty = fixture
                .library
                .search_page(
                    query_input(revision, 12, " \t\n ", 0, 10),
                    &MutationControl::new(),
                )
                .expect("search whitespace");
            assert_eq!(empty.total, 0);
            assert!(empty.rows.is_empty());
        });
    }

    #[test]
    fn undersized_rebuild_response_does_not_install_the_index() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let revision = fixture.library.revision();
            assert!(matches!(
                fixture
                    .library
                    .rebuild_search_index(
                        revision,
                        crate::ML_MIN_EVENT_PAYLOAD_BYTES as usize,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::ResponseTooLarge { .. })
            ));
            assert!(matches!(
                fixture.library.search_page(
                    query_input(revision, 13, "lesson", 0, 10),
                    &MutationControl::new(),
                ),
                Err(LibraryError::StaleSearchIndex { actual: None, .. })
            ));
        });
    }

    #[test]
    fn broad_search_pages_one_hundred_thousand_entries() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let revision = fixture.library.revision();
            fixture.library.search_index = Some(SearchIndex {
                revision,
                entries: (0..100_000)
                    .map(|index| SearchEntry {
                        id: format!("lesson-{index:06}"),
                        course_id: String::new(),
                        course_name: String::new(),
                        section_id: String::new(),
                        section_name: String::new(),
                        name: "Lesson".to_string(),
                        name_folded: "lesson".to_string(),
                        path: String::new(),
                        relative_path: String::new(),
                        kind: "video".to_string(),
                        searchable: "lesson".to_string(),
                    })
                    .collect(),
            });

            let page = fixture
                .library
                .search_page(
                    query_input(revision, 14, "lesson", 99_980, 20),
                    &MutationControl::new(),
                )
                .expect("search 100,000 lessons");
            assert_eq!(page.total, 100_000);
            assert_eq!(page.rows.len(), 20);
            assert_eq!(page.rows[0].id, "lesson-099980");
            assert_eq!(page.rows[19].id, "lesson-099999");
        });
    }

    #[test]
    fn progress_keeps_the_index_and_scan_invalidates_it() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let index_revision = fixture.library.revision();
            fixture
                .library
                .rebuild_search_index(index_revision, usize::MAX, &MutationControl::new())
                .await
                .expect("rebuild search index");
            fixture
                .library
                .put_progress(
                    ProgressInput {
                        expected_revision: fixture.library.revision(),
                        lesson_id: "heaps".to_string(),
                        watched_time: 12,
                        last_position: 12.0,
                        completed: false,
                    },
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("update progress");
            assert!(
                fixture
                    .library
                    .search_page(
                        query_input(index_revision, 20, "heaps", 0, 10),
                        &MutationControl::new(),
                    )
                    .is_ok()
            );

            fixture
                .library
                .scan_and_reconcile(
                    fixture.library.revision(),
                    fixture.root.to_string_lossy().as_ref(),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("scan current root");
            assert!(matches!(
                fixture.library.search_page(
                    query_input(index_revision, 21, "heaps", 0, 10),
                    &MutationControl::new(),
                ),
                Err(LibraryError::StaleSearchIndex { actual: None, .. })
            ));
        });
    }

    #[test]
    fn search_rejects_stale_invalid_and_cancelled_work() {
        block_on(async {
            let mut fixture = search_fixture().await;
            let revision = fixture.library.revision();
            assert!(matches!(
                fixture
                    .library
                    .rebuild_search_index(revision + 1, usize::MAX, &MutationControl::new())
                    .await,
                Err(LibraryError::StaleRevision { .. })
            ));
            let cancelled = MutationControl::new();
            assert!(cancelled.cancel());
            assert!(matches!(
                fixture
                    .library
                    .rebuild_search_index(revision, usize::MAX, &cancelled)
                    .await,
                Err(LibraryError::Cancelled)
            ));
            fixture
                .library
                .rebuild_search_index(revision, usize::MAX, &MutationControl::new())
                .await
                .expect("rebuild search index");

            for input in [
                query_input(revision + 1, 30, "lesson", 0, 10),
                query_input(revision, 31, "lesson", 0, 0),
                query_input(revision, 32, "lesson", i64::MAX as u64 + 1, 1),
                query_input(
                    revision,
                    33,
                    &"x".repeat(ML_MAX_SEARCH_QUERY_BYTES as usize + 1),
                    0,
                    1,
                ),
            ] {
                assert!(
                    fixture
                        .library
                        .search_page(input, &MutationControl::new())
                        .is_err()
                );
            }
            assert!(matches!(
                fixture
                    .library
                    .search_page(query_input(revision, 34, "lesson", 0, 10), &cancelled,),
                Err(LibraryError::Cancelled)
            ));
        });
    }
}
