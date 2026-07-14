use std::cmp::Ordering;
use std::collections::HashMap;
use std::num::NonZeroU64;
use std::ops::Range;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering as AtomicOrdering};
use std::sync::{Arc, LazyLock};
use std::time::Duration;

use icu_collator::preferences::CollationNumericOrdering;
use icu_collator::{CollatorBorrowed, CollatorPreferences};
use sha2::{Digest, Sha384};
use sqlx::sqlite::{SqliteConnectOptions, SqliteRow};
use sqlx::{Connection, QueryBuilder, Row, Sqlite, SqliteConnection};

use crate::migrations::{MIGRATIONS, MigrationDefinition};

const SUPPORTED_MIGRATION_VERSION: i64 = 16;
const SQLITE_PROGRESS_INTERVAL: i32 = 1_000;
pub(crate) const MAX_COURSE_PAGE_SIZE: u32 = 200;
static NATURAL_COLLATOR: LazyLock<CollatorBorrowed<'static>> = LazyLock::new(|| {
    let mut preferences = CollatorPreferences::default();
    preferences.numeric_ordering = Some(CollationNumericOrdering::True);
    CollatorBorrowed::try_new(preferences, Default::default())
        .expect("compiled ICU data supports natural collation")
});

#[derive(Debug)]
pub(crate) enum LibraryError {
    Database(String),
    MigrationRequired { current: i64, supported: i64 },
    DatabaseTooNew { current: i64, supported: i64 },
    InvalidMigrationLedger,
    InvalidPageSize { limit: u32 },
    InvalidOffset { offset: u64 },
    StaleRevision { expected: u64, actual: u64 },
}
impl std::fmt::Display for LibraryError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Database(message) => formatter.write_str(message),
            Self::MigrationRequired { current, supported } => {
                write!(
                    formatter,
                    "database migration {current} requires {supported}"
                )
            }
            Self::DatabaseTooNew { current, supported } => {
                write!(
                    formatter,
                    "database migration {current} exceeds {supported}"
                )
            }
            Self::InvalidMigrationLedger => {
                formatter.write_str("invalid database migration ledger")
            }
            Self::InvalidPageSize { limit } => {
                write!(formatter, "invalid page size {limit}")
            }
            Self::InvalidOffset { offset } => {
                write!(formatter, "invalid page offset {offset}")
            }
            Self::StaleRevision { expected, actual } => {
                write!(
                    formatter,
                    "stale library revision {expected}; current is {actual}"
                )
            }
        }
    }
}

impl std::error::Error for LibraryError {}

impl From<sqlx::Error> for LibraryError {
    fn from(error: sqlx::Error) -> Self {
        Self::Database(error.to_string())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct CourseSummary {
    pub(crate) id: String,
    pub(crate) identity_id: String,
    pub(crate) name: String,
    pub(crate) path: String,
    pub(crate) fingerprint: Option<String>,
    pub(crate) missing_since: Option<String>,
    pub(crate) stored_total_duration: i64,
    pub(crate) stored_watched_duration: i64,
    pub(crate) last_accessed: Option<String>,
    pub(crate) thumbnail_source_path: Option<String>,
    pub(crate) section_count: u64,
    pub(crate) first_section_name: Option<String>,
    pub(crate) lesson_count: u64,
    pub(crate) completed_lesson_count: u64,
    pub(crate) progress_percent: u32,
    pub(crate) lesson_total_duration: i64,
    pub(crate) lesson_watched_duration: i64,
    pub(crate) lesson_bytes: i64,
    pub(crate) leading_lesson_kind: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct CoursePage {
    pub(crate) revision: u64,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) rows: Vec<CourseSummary>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct SubtitleSummary {
    pub(crate) path: String,
    pub(crate) language: String,
    pub(crate) label: String,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LessonSummary {
    pub(crate) id: String,
    pub(crate) course_id: String,
    pub(crate) section_id: Option<String>,
    pub(crate) section_name: String,
    pub(crate) name: String,
    pub(crate) path: String,
    pub(crate) relative_path: Option<String>,
    pub(crate) kind: String,
    pub(crate) duration: i64,
    pub(crate) file_size: i64,
    pub(crate) completed: bool,
    pub(crate) watched_time: i64,
    pub(crate) last_position: f64,
    pub(crate) order: i64,
    pub(crate) subtitles: Vec<SubtitleSummary>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LessonPage {
    pub(crate) revision: u64,
    pub(crate) course_id: String,
    pub(crate) section_id: Option<String>,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) rows: Vec<LessonSummary>,
}

struct LessonOrderIndex {
    sections: Vec<IndexedSection>,
    lessons: Vec<IndexedLesson>,
    section_ranges: HashMap<String, Range<usize>>,
}

struct IndexedSection {
    id: String,
    name: String,
}

struct IndexedLesson {
    id: String,
    section_index: usize,
    order: i64,
}

struct PendingSection {
    id: String,
    name: String,
    order: Option<i64>,
    lessons: Vec<PendingLesson>,
}

struct PendingLesson {
    id: String,
    name: String,
    order: i64,
}

struct UnresolvedLesson {
    id: String,
    section_id: Option<String>,
    section_name: String,
    name: String,
    order: Option<i64>,
}

struct LessonPageKey {
    id: String,
    section_id: String,
    section_name: String,
    order: i64,
}

pub(crate) struct LibraryDatabase {
    connection: SqliteConnection,
    revision: u64,
    library_path: Option<String>,
    lesson_order_indexes: HashMap<String, LessonOrderIndex>,
    #[cfg(test)]
    lesson_order_index_builds: usize,
}

impl LibraryDatabase {
    /// Opens an isolated, checkpointed database copy for read-only projection.
    /// Live databases use the coordinator's migration-aware open path instead.
    pub(crate) async fn open_snapshot_read_only(
        path: &Path,
        revision: NonZeroU64,
    ) -> Result<Self, LibraryError> {
        Self::open_snapshot_read_only_interruptible(
            path,
            revision,
            Arc::new(AtomicBool::new(false)),
        )
        .await
    }

    pub(crate) async fn open_snapshot_read_only_interruptible(
        path: &Path,
        revision: NonZeroU64,
        stopping: Arc<AtomicBool>,
    ) -> Result<Self, LibraryError> {
        let options = SqliteConnectOptions::new()
            .filename(path)
            .create_if_missing(false)
            .read_only(true)
            .immutable(true)
            .foreign_keys(true)
            .collation("MELEARNER_NATURAL", natural_cmp)
            .busy_timeout(Duration::from_secs(10));
        let mut connection = SqliteConnection::connect_with(&options).await?;
        connection
            .lock_handle()
            .await?
            .set_progress_handler(SQLITE_PROGRESS_INTERVAL, move || {
                !stopping.load(AtomicOrdering::Acquire)
            });

        if let Err(error) = verify_migration_ledger(&mut connection).await {
            let _ = connection.close().await;
            return Err(error);
        }

        let library_path = match load_library_path(&mut connection).await {
            Ok(value) => value,
            Err(error) => {
                let _ = connection.close().await;
                return Err(error);
            }
        };

        Ok(Self {
            connection,
            revision: revision.get(),
            library_path,
            lesson_order_indexes: HashMap::new(),
            #[cfg(test)]
            lesson_order_index_builds: 0,
        })
    }

    pub(crate) fn revision(&self) -> u64 {
        self.revision
    }

    pub(crate) fn library_path(&self) -> Option<&str> {
        self.library_path.as_deref()
    }

    pub(crate) async fn close(self) -> Result<(), LibraryError> {
        self.connection.close().await?;
        Ok(())
    }

    #[cfg(test)]
    pub(crate) async fn run_until_interrupted(
        &mut self,
        entered: std::sync::mpsc::Sender<()>,
    ) -> Result<(), LibraryError> {
        let _ = entered.send(());
        sqlx::query_scalar::<_, i64>(
            "WITH RECURSIVE numbers(value) AS (
                 VALUES (0)
                 UNION ALL
                 SELECT value + 1 FROM numbers WHERE value < 1000000000
             )
             SELECT MAX(value) FROM numbers",
        )
        .fetch_one(&mut self.connection)
        .await?;
        Ok(())
    }

    pub(crate) async fn course_page(
        &mut self,
        expected_revision: u64,
        offset: u64,
        limit: u32,
    ) -> Result<CoursePage, LibraryError> {
        if expected_revision != self.revision {
            return Err(LibraryError::StaleRevision {
                expected: expected_revision,
                actual: self.revision,
            });
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&limit) {
            return Err(LibraryError::InvalidPageSize { limit });
        }
        let sqlite_offset =
            i64::try_from(offset).map_err(|_| LibraryError::InvalidOffset { offset })?;
        let library_path = self
            .library_path
            .as_deref()
            .filter(|library_path| !library_path.is_empty());
        let path_range = library_path.map(child_path_range);
        let mut transaction = self.connection.begin().await?;
        let total: i64 = if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            sqlx::query_scalar(
                "SELECT SUM(course_count)
                 FROM (
                     SELECT COUNT(*) AS course_count
                     FROM courses INDEXED BY idx_courses_path
                     WHERE path = ?1
                     UNION ALL
                     SELECT COUNT(*) AS course_count
                     FROM courses INDEXED BY idx_courses_path
                     WHERE path > ?2 AND path < ?3
                 )",
            )
            .bind(library_path)
            .bind(prefix)
            .bind(upper_bound)
            .fetch_one(&mut *transaction)
            .await?
        } else {
            sqlx::query_scalar("SELECT COUNT(*) FROM courses")
                .fetch_one(&mut *transaction)
                .await?
        };

        let page_sql = course_page_sql(library_path.is_some());
        let mut page_query = sqlx::query(&page_sql);
        if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            page_query = page_query.bind(library_path).bind(prefix).bind(upper_bound);
        }
        let records = page_query
            .bind(i64::from(limit))
            .bind(sqlite_offset)
            .fetch_all(&mut *transaction)
            .await?;
        transaction.commit().await?;

        let total = u64::try_from(total)
            .map_err(|_| LibraryError::Database("negative course count".to_string()))?;
        let mut rows = Vec::with_capacity(records.len());
        for record in records {
            rows.push(course_summary(record)?);
        }
        Ok(CoursePage {
            revision: self.revision,
            offset,
            total,
            rows,
        })
    }

    pub(crate) async fn lesson_page(
        &mut self,
        expected_revision: u64,
        course_id: &str,
        section_id: Option<&str>,
        offset: u64,
        limit: u32,
    ) -> Result<LessonPage, LibraryError> {
        if expected_revision != self.revision {
            return Err(LibraryError::StaleRevision {
                expected: expected_revision,
                actual: self.revision,
            });
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&limit) {
            return Err(LibraryError::InvalidPageSize { limit });
        }
        i64::try_from(offset).map_err(|_| LibraryError::InvalidOffset { offset })?;
        let page_offset =
            usize::try_from(offset).map_err(|_| LibraryError::InvalidOffset { offset })?;
        let library_path = self
            .library_path
            .as_deref()
            .filter(|library_path| !library_path.is_empty());
        let path_range = library_path.map(child_path_range);
        let course_is_visible: bool = if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            sqlx::query_scalar(
                "SELECT EXISTS(
                     SELECT 1
                     FROM courses
                     WHERE id = ?1
                       AND (path = ?2 OR (path > ?3 AND path < ?4))
                 )",
            )
            .bind(course_id)
            .bind(library_path)
            .bind(prefix)
            .bind(upper_bound)
            .fetch_one(&mut self.connection)
            .await?
        } else {
            sqlx::query_scalar("SELECT EXISTS(SELECT 1 FROM courses WHERE id = ?1)")
                .bind(course_id)
                .fetch_one(&mut self.connection)
                .await?
        };

        if !course_is_visible {
            return Ok(LessonPage {
                revision: self.revision,
                course_id: course_id.to_string(),
                section_id: section_id.map(str::to_string),
                offset,
                total: 0,
                rows: Vec::new(),
            });
        }

        if !self.lesson_order_indexes.contains_key(course_id) {
            let index = load_lesson_order_index(&mut self.connection, course_id).await?;
            self.lesson_order_indexes
                .insert(course_id.to_string(), index);
            #[cfg(test)]
            {
                self.lesson_order_index_builds += 1;
            }
        }
        let (total, page_keys) = {
            let index = self
                .lesson_order_indexes
                .get(course_id)
                .ok_or_else(|| LibraryError::Database("missing lesson order index".to_string()))?;
            let range = section_id
                .and_then(|section_id| index.section_ranges.get(section_id).cloned())
                .unwrap_or_else(|| {
                    if section_id.is_some() {
                        0..0
                    } else {
                        0..index.lessons.len()
                    }
                });
            let total = u64::try_from(range.len())
                .map_err(|_| LibraryError::Database("lesson count exceeds u64".to_string()))?;
            let start = range.start + page_offset.min(range.len());
            let end = start.saturating_add(limit as usize).min(range.end);
            let mut page_keys = Vec::with_capacity(end - start);
            for lesson in &index.lessons[start..end] {
                let section = index.sections.get(lesson.section_index).ok_or_else(|| {
                    LibraryError::Database("invalid lesson order index".to_string())
                })?;
                page_keys.push(LessonPageKey {
                    id: lesson.id.clone(),
                    section_id: section.id.clone(),
                    section_name: section.name.clone(),
                    order: lesson.order,
                });
            }
            (total, page_keys)
        };

        if page_keys.is_empty() {
            return Ok(LessonPage {
                revision: self.revision,
                course_id: course_id.to_string(),
                section_id: section_id.map(str::to_string),
                offset,
                total,
                rows: Vec::new(),
            });
        }

        let mut transaction = self.connection.begin().await?;
        let mut details = QueryBuilder::<Sqlite>::new(
            "SELECT id, course_id, name, path, relative_path, type,
                    COALESCE(duration, 0) AS duration,
                    COALESCE(file_size, 0) AS file_size,
                    COALESCE(completed, 0) AS completed,
                    COALESCE(watched_time, 0) AS watched_time,
                    COALESCE(last_position, 0) AS last_position
             FROM lessons
             WHERE id IN (",
        );
        let mut separated = details.separated(", ");
        for lesson in &page_keys {
            separated.push_bind(&lesson.id);
        }
        separated.push_unseparated(")");
        let mut record_by_id = HashMap::with_capacity(page_keys.len());
        for record in details.build().fetch_all(&mut *transaction).await? {
            let id = record.try_get::<String, _>("id")?;
            record_by_id.insert(id, record);
        }
        let mut rows = Vec::with_capacity(page_keys.len());
        for lesson in &page_keys {
            let record = record_by_id.remove(&lesson.id).ok_or_else(|| {
                LibraryError::Database("lesson changed inside immutable snapshot".to_string())
            })?;
            rows.push(lesson_summary(record, lesson)?);
        }

        let mut subtitles = QueryBuilder::<Sqlite>::new(
            "SELECT lesson_id, path,
                    COALESCE(language, 'default') AS language,
                    COALESCE(label, language, 'default') AS label
             FROM lesson_subtitles INDEXED BY idx_lesson_subtitles_lesson
             WHERE lesson_id IN (",
        );
        let mut separated = subtitles.separated(", ");
        for lesson in &rows {
            separated.push_bind(&lesson.id);
        }
        separated.push_unseparated(") ORDER BY order_index ASC");
        let row_by_id = rows
            .iter()
            .enumerate()
            .map(|(index, lesson)| (lesson.id.clone(), index))
            .collect::<HashMap<_, _>>();
        for record in subtitles.build().fetch_all(&mut *transaction).await? {
            let lesson_id = record.try_get::<String, _>("lesson_id")?;
            if let Some(index) = row_by_id.get(&lesson_id) {
                rows[*index].subtitles.push(SubtitleSummary {
                    path: record.try_get("path")?,
                    language: record.try_get("language")?,
                    label: record.try_get("label")?,
                });
            }
        }
        transaction.commit().await?;

        Ok(LessonPage {
            revision: self.revision,
            course_id: course_id.to_string(),
            section_id: section_id.map(str::to_string),
            offset,
            total,
            rows,
        })
    }
}

async fn load_lesson_order_index(
    connection: &mut SqliteConnection,
    course_id: &str,
) -> Result<LessonOrderIndex, LibraryError> {
    let mut sections = sqlx::query(
        "SELECT id, name, order_index
         FROM sections INDEXED BY idx_sections_course
         WHERE course_id = ?1",
    )
    .bind(course_id)
    .fetch_all(&mut *connection)
    .await?
    .into_iter()
    .map(|row| {
        Ok(PendingSection {
            id: row.try_get("id")?,
            name: row.try_get("name")?,
            order: row.try_get("order_index")?,
            lessons: Vec::new(),
        })
    })
    .collect::<Result<Vec<_>, LibraryError>>()?;
    sections.sort_by(|left, right| {
        left.order
            .unwrap_or(0)
            .cmp(&right.order.unwrap_or(0))
            .then_with(|| NATURAL_COLLATOR.compare(&left.name, &right.name))
    });
    for (position, section) in sections.iter_mut().enumerate() {
        if section.order.is_none() {
            section.order =
                Some(i64::try_from(position).map_err(|_| {
                    LibraryError::Database("section count exceeds i64".to_string())
                })?);
        }
    }

    let mut section_by_key = HashMap::with_capacity(sections.len().saturating_mul(2));
    for (index, section) in sections.iter().enumerate() {
        section_by_key.insert(section.id.clone(), index);
        section_by_key.insert(section.name.clone(), index);
    }

    let mut lessons = sqlx::query(
        "SELECT id, section_id, section_name, name, order_index
         FROM lessons INDEXED BY idx_lessons_course
         WHERE course_id = ?1",
    )
    .bind(course_id)
    .fetch_all(&mut *connection)
    .await?
    .into_iter()
    .map(|row| {
        Ok(UnresolvedLesson {
            id: row.try_get("id")?,
            section_id: row.try_get("section_id")?,
            section_name: row
                .try_get::<Option<String>, _>("section_name")?
                .unwrap_or_else(|| "Course".to_string()),
            name: row.try_get("name")?,
            order: row.try_get("order_index")?,
        })
    })
    .collect::<Result<Vec<_>, LibraryError>>()?;
    lessons.sort_by(|left, right| {
        left.order
            .unwrap_or(0)
            .cmp(&right.order.unwrap_or(0))
            .then_with(|| NATURAL_COLLATOR.compare(&left.name, &right.name))
    });

    for lesson in lessons {
        let section_index = lesson
            .section_id
            .as_ref()
            .filter(|section_id| !section_id.is_empty())
            .and_then(|section_id| section_by_key.get(section_id).copied())
            .or_else(|| section_by_key.get(&lesson.section_name).copied());
        let section_index = if let Some(section_index) = section_index {
            section_index
        } else {
            let section_index = sections.len();
            let order = i64::try_from(section_index)
                .map_err(|_| LibraryError::Database("section count exceeds i64".to_string()))?;
            let id = format!("{course_id}:section:{section_index}");
            sections.push(PendingSection {
                id: id.clone(),
                name: lesson.section_name.clone(),
                order: Some(order),
                lessons: Vec::new(),
            });
            section_by_key.insert(id, section_index);
            section_by_key.insert(lesson.section_name.clone(), section_index);
            section_index
        };
        let section = sections
            .get_mut(section_index)
            .ok_or_else(|| LibraryError::Database("invalid resolved lesson section".to_string()))?;
        let order = if let Some(order) = lesson.order {
            order
        } else {
            i64::try_from(section.lessons.len())
                .map_err(|_| LibraryError::Database("lesson count exceeds i64".to_string()))?
        };
        section.lessons.push(PendingLesson {
            id: lesson.id,
            name: lesson.name,
            order,
        });
    }

    for section in &mut sections {
        section.lessons.sort_by(|left, right| {
            left.order
                .cmp(&right.order)
                .then_with(|| NATURAL_COLLATOR.compare(&left.name, &right.name))
        });
    }
    sections.sort_by(|left, right| {
        left.order
            .unwrap_or(0)
            .cmp(&right.order.unwrap_or(0))
            .then_with(|| NATURAL_COLLATOR.compare(&left.name, &right.name))
    });

    let lesson_count = sections.iter().map(|section| section.lessons.len()).sum();
    let mut indexed_sections = Vec::with_capacity(sections.len());
    let mut indexed_lessons = Vec::with_capacity(lesson_count);
    let mut section_ranges = HashMap::with_capacity(sections.len());
    for section in sections {
        let section_index = indexed_sections.len();
        let start = indexed_lessons.len();
        indexed_lessons.extend(section.lessons.into_iter().map(|lesson| IndexedLesson {
            id: lesson.id,
            section_index,
            order: lesson.order,
        }));
        let end = indexed_lessons.len();
        section_ranges.insert(section.id.clone(), start..end);
        indexed_sections.push(IndexedSection {
            id: section.id,
            name: section.name,
        });
    }

    Ok(LessonOrderIndex {
        sections: indexed_sections,
        lessons: indexed_lessons,
        section_ranges,
    })
}

fn course_page_sql(rooted: bool) -> String {
    let (course_source, limit_parameter, offset_parameter) = if rooted {
        (
            "SELECT *
             FROM courses INDEXED BY idx_courses_path
             WHERE path = ?1
             UNION ALL
             SELECT *
             FROM courses INDEXED BY idx_courses_path
             WHERE path > ?2 AND path < ?3",
            "?4",
            "?5",
        )
    } else {
        ("SELECT * FROM courses", "?1", "?2")
    };

    format!(
        "WITH filtered_courses AS (
             {course_source}
         ),
         paged_courses AS (
             SELECT *
             FROM filtered_courses
             ORDER BY COALESCE(last_accessed, '') DESC,
                      name COLLATE NOCASE,
                      id
             LIMIT {limit_parameter} OFFSET {offset_parameter}
         ),
             section_order AS (
                 SELECT sections.course_id, sections.name,
                        ROW_NUMBER() OVER (
                            PARTITION BY sections.course_id
                            ORDER BY COALESCE(sections.order_index, 0),
                                     sections.name COLLATE MELEARNER_NATURAL,
                                     sections.id
                        ) AS position
                 FROM paged_courses
                 CROSS JOIN sections INDEXED BY idx_sections_course
                 WHERE sections.course_id = paged_courses.id
             ),
             section_stats AS (
                 SELECT sections.course_id, COUNT(*) AS section_count
                 FROM paged_courses
                 CROSS JOIN sections INDEXED BY idx_sections_course
                 WHERE sections.course_id = paged_courses.id
                 GROUP BY sections.course_id
             ),
             lesson_order AS (
                 SELECT lessons.course_id, lessons.type, lessons.path,
                        ROW_NUMBER() OVER (
                            PARTITION BY lessons.course_id
                            ORDER BY COALESCE(sections.order_index, 2147483647),
                                     COALESCE(sections.name, lessons.section_name, '')
                                         COLLATE MELEARNER_NATURAL,
                                     COALESCE(lessons.order_index, 0),
                                     lessons.name COLLATE MELEARNER_NATURAL,
                                     lessons.id
                        ) AS position
                 FROM paged_courses
                 CROSS JOIN lessons INDEXED BY idx_lessons_course
                 LEFT JOIN sections ON sections.id = lessons.section_id
                 WHERE lessons.course_id = paged_courses.id
             ),
             first_video_position AS (
                 SELECT course_id, MIN(position) AS position
                 FROM lesson_order
                 WHERE type = 'video'
                 GROUP BY course_id
             ),
             lesson_stats AS (
                 SELECT lessons.course_id,
                        COUNT(*) AS lesson_count,
                        SUM(CASE WHEN completed != 0 THEN 1 ELSE 0 END) AS completed_lesson_count,
                        SUM(COALESCE(duration, 0)) AS lesson_total_duration,
                        SUM(COALESCE(watched_time, 0)) AS lesson_watched_duration,
                        SUM(COALESCE(file_size, 0)) AS lesson_bytes
                 FROM paged_courses
                 CROSS JOIN lessons INDEXED BY idx_lessons_course
                 WHERE lessons.course_id = paged_courses.id
                 GROUP BY lessons.course_id
             )
             SELECT paged_courses.id,
                    COALESCE(paged_courses.identity_id, paged_courses.id) AS identity_id,
                    paged_courses.name,
                    paged_courses.path,
                    paged_courses.fingerprint,
                    paged_courses.missing_since,
                    COALESCE(paged_courses.total_duration, 0) AS stored_total_duration,
                    COALESCE(paged_courses.watched_duration, 0) AS stored_watched_duration,
                    paged_courses.last_accessed,
                    COALESCE(paged_courses.thumbnail_source_path, first_video.path)
                        AS thumbnail_source_path,
                    COALESCE(section_stats.section_count, 0) AS section_count,
                    section_order.name AS first_section_name,
                    COALESCE(lesson_stats.lesson_count, 0) AS lesson_count,
                    COALESCE(lesson_stats.completed_lesson_count, 0) AS completed_lesson_count,
                    COALESCE(lesson_stats.lesson_total_duration, 0) AS lesson_total_duration,
                    COALESCE(lesson_stats.lesson_watched_duration, 0) AS lesson_watched_duration,
                    COALESCE(lesson_stats.lesson_bytes, 0) AS lesson_bytes,
                    leading_lesson.type AS leading_lesson_kind
             FROM paged_courses
             LEFT JOIN section_stats ON section_stats.course_id = paged_courses.id
             LEFT JOIN section_order
                    ON section_order.course_id = paged_courses.id AND section_order.position = 1
             LEFT JOIN lesson_stats ON lesson_stats.course_id = paged_courses.id
             LEFT JOIN lesson_order AS leading_lesson
                    ON leading_lesson.course_id = paged_courses.id
                   AND leading_lesson.position = 1
             LEFT JOIN first_video_position
                    ON first_video_position.course_id = paged_courses.id
             LEFT JOIN lesson_order AS first_video
                    ON first_video.course_id = paged_courses.id
                   AND first_video.position = first_video_position.position
             ORDER BY COALESCE(paged_courses.last_accessed, '') DESC,
                      paged_courses.name COLLATE NOCASE,
                      paged_courses.id"
    )
}

async fn load_library_path(
    connection: &mut SqliteConnection,
) -> Result<Option<String>, LibraryError> {
    let stored = sqlx::query_scalar::<_, Option<String>>(
        "SELECT value FROM app_settings WHERE key = 'libraryPath'",
    )
    .fetch_optional(&mut *connection)
    .await?
    .flatten();
    if stored.is_some() {
        return Ok(stored);
    }

    let course_paths = sqlx::query_scalar::<_, String>("SELECT path FROM courses ORDER BY id")
        .fetch_all(&mut *connection)
        .await?;
    Ok(infer_library_path(&course_paths))
}

async fn verify_migration_ledger(connection: &mut SqliteConnection) -> Result<(), LibraryError> {
    let rows = sqlx::query(
        "SELECT version, description, success, checksum
         FROM _sqlx_migrations
         ORDER BY version",
    )
    .fetch_all(&mut *connection)
    .await?;
    let current = rows
        .last()
        .map(|row| row.get::<i64, _>("version"))
        .unwrap_or(0);
    if current < SUPPORTED_MIGRATION_VERSION {
        return Err(LibraryError::MigrationRequired {
            current,
            supported: SUPPORTED_MIGRATION_VERSION,
        });
    }
    if current > SUPPORTED_MIGRATION_VERSION {
        return Err(LibraryError::DatabaseTooNew {
            current,
            supported: SUPPORTED_MIGRATION_VERSION,
        });
    }
    if rows.len() != supported_migrations().count()
        || supported_migrations()
            .last()
            .map(|migration| migration.version)
            != Some(SUPPORTED_MIGRATION_VERSION)
    {
        return Err(LibraryError::InvalidMigrationLedger);
    }
    for (row, migration) in rows.iter().zip(supported_migrations()) {
        if row.get::<i64, _>("version") != migration.version
            || row.get::<String, _>("description") != migration.description
            || !row.get::<bool, _>("success")
            || row.get::<Vec<u8>, _>("checksum") != Sha384::digest(migration.sql).to_vec()
        {
            return Err(LibraryError::InvalidMigrationLedger);
        }
    }
    Ok(())
}

fn supported_migrations() -> impl Iterator<Item = &'static MigrationDefinition> {
    MIGRATIONS
        .iter()
        .filter(|migration| migration.version <= SUPPORTED_MIGRATION_VERSION)
}

fn course_summary(row: SqliteRow) -> Result<CourseSummary, LibraryError> {
    let section_count = row.try_get::<i64, _>("section_count")?;
    let lesson_count = row.try_get::<i64, _>("lesson_count")?;
    let completed_lesson_count = row.try_get::<i64, _>("completed_lesson_count")?;
    let section_count = nonnegative_count(section_count)?;
    let lesson_count = nonnegative_count(lesson_count)?;
    let completed_lesson_count = nonnegative_count(completed_lesson_count)?;
    let progress_percent = if lesson_count == 0 {
        0
    } else {
        ((completed_lesson_count as f64 / lesson_count as f64) * 100.0).round() as u32
    };
    Ok(CourseSummary {
        id: row.try_get("id")?,
        identity_id: row.try_get("identity_id")?,
        name: row.try_get("name")?,
        path: row.try_get("path")?,
        fingerprint: row.try_get("fingerprint")?,
        missing_since: row.try_get("missing_since")?,
        stored_total_duration: row.try_get("stored_total_duration")?,
        stored_watched_duration: row.try_get("stored_watched_duration")?,
        last_accessed: row.try_get("last_accessed")?,
        thumbnail_source_path: row.try_get("thumbnail_source_path")?,
        section_count,
        first_section_name: row.try_get("first_section_name")?,
        lesson_count,
        completed_lesson_count,
        progress_percent,
        lesson_total_duration: row.try_get("lesson_total_duration")?,
        lesson_watched_duration: row.try_get("lesson_watched_duration")?,
        lesson_bytes: row.try_get("lesson_bytes")?,
        leading_lesson_kind: row.try_get("leading_lesson_kind")?,
    })
}

fn lesson_summary(row: SqliteRow, indexed: &LessonPageKey) -> Result<LessonSummary, LibraryError> {
    Ok(LessonSummary {
        id: row.try_get("id")?,
        course_id: row.try_get("course_id")?,
        section_id: Some(indexed.section_id.clone()),
        section_name: indexed.section_name.clone(),
        name: row.try_get("name")?,
        path: row.try_get("path")?,
        relative_path: row.try_get("relative_path")?,
        kind: row.try_get("type")?,
        duration: row.try_get("duration")?,
        file_size: row.try_get("file_size")?,
        completed: row.try_get("completed")?,
        watched_time: row.try_get("watched_time")?,
        last_position: row.try_get("last_position")?,
        order: indexed.order,
        subtitles: Vec::new(),
    })
}

fn nonnegative_count(value: i64) -> Result<u64, LibraryError> {
    u64::try_from(value).map_err(|_| LibraryError::Database("negative aggregate count".to_string()))
}

fn infer_library_path(paths: &[String]) -> Option<String> {
    if paths.is_empty() {
        return None;
    }
    if paths.len() == 1 {
        return Some(paths[0].clone());
    }

    let separator = path_separator(&paths[0]);
    let split_paths = paths
        .iter()
        .map(|path| {
            trim_trailing_separators(path)
                .split(separator)
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();
    let shortest = split_paths.iter().map(Vec::len).min()?;
    let mut common = Vec::new();
    for index in 0..shortest.saturating_sub(1) {
        let component = split_paths[0][index];
        if split_paths.iter().all(|parts| parts[index] == component) {
            common.push(component);
        } else {
            break;
        }
    }
    if common.is_empty() {
        return None;
    }
    if common.len() == 1 && common[0].is_empty() {
        return Some(separator.to_string());
    }
    let mut inferred = common.join(&separator.to_string());
    if inferred.len() == 2 && has_windows_drive_prefix(&inferred) {
        inferred.push(separator);
    }
    Some(inferred)
}

fn child_path_prefix(path: &str) -> String {
    let separator = path_separator(path);
    let normalized = trim_trailing_separators(path);
    let mut prefix = normalized.to_string();
    if !prefix.ends_with(['/', '\\']) {
        prefix.push(separator);
    }
    prefix
}

fn child_path_range(path: &str) -> (String, String) {
    let prefix = child_path_prefix(path);
    let mut upper_bound = prefix.clone();
    let separator = upper_bound.pop().expect("child prefix has a separator");
    upper_bound.push(match separator {
        '/' => '0',
        '\\' => ']',
        _ => unreachable!("child prefix ends in a path separator"),
    });
    (prefix, upper_bound)
}

fn trim_trailing_separators(path: &str) -> &str {
    if path == "/" {
        path
    } else {
        path.trim_end_matches(['/', '\\'])
    }
}

fn path_separator(path: &str) -> char {
    if path.starts_with('/') || path.contains('/') {
        '/'
    } else if has_windows_drive_prefix(path) || path.starts_with("\\\\") || path.contains('\\') {
        '\\'
    } else {
        std::path::MAIN_SEPARATOR
    }
}

fn has_windows_drive_prefix(path: &str) -> bool {
    let bytes = path.as_bytes();
    bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':'
}

fn natural_cmp(left: &str, right: &str) -> Ordering {
    NATURAL_COLLATOR.compare(left, right)
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::future::Future;
    use std::num::NonZeroU64;
    use std::path::{Path, PathBuf};
    use std::time::Duration;

    use super::{
        LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE, child_path_prefix, child_path_range,
        course_page_sql, natural_cmp,
    };
    use sqlx::sqlite::SqliteConnectOptions;
    use sqlx::{Connection, Row, SqliteConnection};

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build library test runtime")
            .block_on(future)
    }

    fn revision(value: u64) -> NonZeroU64 {
        NonZeroU64::new(value).expect("nonzero test revision")
    }

    fn copied_fixture() -> (tempfile::TempDir, PathBuf) {
        let temp = tempfile::tempdir().expect("create library fixture tempdir");
        let copied = temp.path().join("database-v16.sqlite");
        fs::copy(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../fixtures/parity/database-v16.sqlite"),
            &copied,
        )
        .expect("copy v16 database fixture");
        (temp, copied)
    }

    async fn mutate_fixture(path: &Path, sql: &str) {
        let options = SqliteConnectOptions::new()
            .filename(path)
            .create_if_missing(false)
            .foreign_keys(true)
            .busy_timeout(Duration::from_secs(10));
        let mut connection = SqliteConnection::connect_with(&options)
            .await
            .expect("open mutable fixture copy");
        sqlx::raw_sql(sql)
            .execute(&mut connection)
            .await
            .expect("mutate fixture copy");
        sqlx::raw_sql("PRAGMA wal_checkpoint(TRUNCATE)")
            .execute(&mut connection)
            .await
            .expect("checkpoint mutable fixture copy");
        connection
            .close()
            .await
            .expect("close mutable fixture copy");
    }

    fn assert_no_sidecars(path: &Path) {
        for suffix in ["-wal", "-shm", "-journal"] {
            assert!(!PathBuf::from(format!("{}{suffix}", path.display())).exists());
        }
    }

    #[test]
    fn course_pages_match_the_frozen_v16_library() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let original = fs::read(&path).expect("read original fixture copy");
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(7))
                .await
                .expect("open copied v16 library");
            let revision = library.revision();
            assert_ne!(revision, 0);
            assert_eq!(library.library_path(), Some("/fixtures/library"));

            let first = library
                .course_page(revision, 0, 2)
                .await
                .expect("load first course page");
            assert_eq!(first.revision, revision);
            assert_eq!(first.offset, 0);
            assert_eq!(first.total, 3);
            assert_eq!(first.rows.len(), 2);
            assert_eq!(
                first,
                library
                    .course_page(revision, 0, 2)
                    .await
                    .expect("repeat first course page")
            );

            let marker = &first.rows[0];
            assert_eq!(marker.id, "course-marker");
            assert_eq!(marker.identity_id, "identity-marker");
            assert_eq!(marker.name, "Systems 日本語");
            assert_eq!(marker.path, "/fixtures/library/Systems 日本語");
            assert_eq!(marker.fingerprint.as_deref(), Some("fp-marker"));
            assert_eq!(marker.missing_since, None);
            assert_eq!(marker.stored_total_duration, 900);
            assert_eq!(marker.stored_watched_duration, 320);
            assert_eq!(
                marker.last_accessed.as_deref(),
                Some("2026-07-09T12:00:00.000Z")
            );
            assert_eq!(
                marker.thumbnail_source_path.as_deref(),
                Some("/fixtures/library/Systems 日本語/01 入門/01 welcome.mp4")
            );
            assert_eq!(marker.section_count, 2);
            assert_eq!(marker.first_section_name.as_deref(), Some("01 入門"));
            assert_eq!(marker.lesson_count, 2);
            assert_eq!(marker.completed_lesson_count, 1);
            assert_eq!(marker.progress_percent, 50);
            assert_eq!(marker.lesson_total_duration, 600);
            assert_eq!(marker.lesson_watched_duration, 320);
            assert_eq!(marker.lesson_bytes, 1_052_672);
            assert_eq!(marker.leading_lesson_kind.as_deref(), Some("video"));

            let missing = &first.rows[1];
            assert_eq!(missing.id, "course-missing");
            assert_eq!(missing.identity_id, "identity-missing");
            assert_eq!(missing.lesson_count, 1);
            assert_eq!(missing.completed_lesson_count, 1);
            assert_eq!(missing.progress_percent, 100);
            assert_eq!(missing.lesson_total_duration, 300);
            assert_eq!(missing.lesson_watched_duration, 300);
            assert_eq!(missing.lesson_bytes, 2_097_152);
            assert_eq!(
                missing.missing_since.as_deref(),
                Some("2026-07-08T09:30:00.000Z")
            );

            let second = library
                .course_page(revision, 2, 2)
                .await
                .expect("load second course page");
            assert_eq!(second.total, 3);
            assert_eq!(second.rows.len(), 1);
            assert_eq!(second.rows[0].id, "course-copy");
            assert_eq!(second.rows[0].progress_percent, 0);

            let exhausted = library
                .course_page(revision, 3, 2)
                .await
                .expect("load exhausted course page");
            assert_eq!(exhausted.total, 3);
            assert!(exhausted.rows.is_empty());
            library.close().await.expect("close copied v16 library");

            assert_eq!(fs::read(&path).expect("reread fixture copy"), original);
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn course_pages_reject_invalid_bounds_and_stale_revisions() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(11))
                .await
                .expect("open copied v16 library");
            let revision = library.revision();

            assert!(matches!(
                library.course_page(revision, 0, 0).await,
                Err(LibraryError::InvalidPageSize { limit: 0 })
            ));
            assert!(matches!(
                library
                    .course_page(revision, 0, MAX_COURSE_PAGE_SIZE + 1)
                    .await,
                Err(LibraryError::InvalidPageSize { .. })
            ));
            assert!(matches!(
                library.course_page(revision + 1, 0, 1).await,
                Err(LibraryError::StaleRevision {
                    expected,
                    actual
                }) if expected == revision + 1 && actual == revision
            ));
            assert!(matches!(
                library.course_page(0, 0, 1).await,
                Err(LibraryError::StaleRevision {
                    expected: 0,
                    actual
                }) if actual == revision
            ));
            assert!(matches!(
                library.course_page(revision, i64::MAX as u64 + 1, 1).await,
                Err(LibraryError::InvalidOffset { .. })
            ));
            library.close().await.expect("close invalid-page library");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn current_open_rejects_incompatible_migration_ledgers_without_writing() {
        block_on(async {
            let (_v15_temp, v15) = copied_fixture();
            mutate_fixture(&v15, "DELETE FROM _sqlx_migrations WHERE version = 16").await;
            let v15_before = fs::read(&v15).expect("read v15 fixture");
            assert!(matches!(
                LibraryDatabase::open_snapshot_read_only(&v15, revision(1)).await,
                Err(LibraryError::MigrationRequired {
                    current: 15,
                    supported: 16
                })
            ));
            assert_eq!(fs::read(&v15).expect("reread v15 fixture"), v15_before);
            assert_no_sidecars(&v15);

            let (_v17_temp, v17) = copied_fixture();
            mutate_fixture(
                &v17,
                "INSERT INTO _sqlx_migrations
                 (version, description, installed_on, success, checksum, execution_time)
                 VALUES (17, 'future', '2026-07-14 00:00:00', 1, X'00', 0)",
            )
            .await;
            let v17_before = fs::read(&v17).expect("read v17 fixture");
            assert!(matches!(
                LibraryDatabase::open_snapshot_read_only(&v17, revision(1)).await,
                Err(LibraryError::DatabaseTooNew {
                    current: 17,
                    supported: 16
                })
            ));
            assert_eq!(fs::read(&v17).expect("reread v17 fixture"), v17_before);
            assert_no_sidecars(&v17);

            let (_changed_temp, changed) = copied_fixture();
            mutate_fixture(
                &changed,
                "UPDATE _sqlx_migrations SET checksum = X'00' WHERE version = 16",
            )
            .await;
            let changed_before = fs::read(&changed).expect("read changed-ledger fixture");
            assert!(matches!(
                LibraryDatabase::open_snapshot_read_only(&changed, revision(1)).await,
                Err(LibraryError::InvalidMigrationLedger)
            ));
            assert_eq!(
                fs::read(&changed).expect("reread changed-ledger fixture"),
                changed_before
            );
            assert_no_sidecars(&changed);
        });
    }

    #[test]
    fn course_pages_escape_library_roots_and_use_an_id_tiebreaker() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "UPDATE app_settings SET value = '/fixtures/weird%_~' WHERE key = 'libraryPath';
                 INSERT INTO courses (id, name, path, last_accessed)
                 VALUES
                   ('tie-b', 'Same', '/fixtures/weird%_~/B', '2026-07-14T00:00:00.000Z'),
                   ('tie-a', 'Same', '/fixtures/weird%_~', '2026-07-14T00:00:00.000Z'),
                   ('wildcard-decoy', 'Same', '/fixtures/weirdXX~/C', '2026-07-14T00:00:00.000Z')",
            )
            .await;
            let before = fs::read(&path).expect("read escaped-root fixture");
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(19))
                .await
                .expect("open escaped-root fixture");
            let page = library
                .course_page(library.revision(), 0, 10)
                .await
                .expect("load escaped-root page");
            assert_eq!(page.total, 2);
            assert_eq!(
                page.rows
                    .iter()
                    .map(|row| row.id.as_str())
                    .collect::<Vec<_>>(),
                ["tie-a", "tie-b"]
            );
            library.close().await.expect("close escaped-root library");
            assert_eq!(
                fs::read(&path).expect("reread escaped-root fixture"),
                before
            );
            assert_no_sidecars(&path);

            let (_all_temp, all_path) = copied_fixture();
            mutate_fixture(
                &all_path,
                "UPDATE app_settings SET value = NULL WHERE key = 'libraryPath'",
            )
            .await;
            let mut all_library = LibraryDatabase::open_snapshot_read_only(&all_path, revision(20))
                .await
                .expect("open fixture without a library root");
            assert_eq!(all_library.library_path(), Some("/fixtures/library"));
            let all_page = all_library
                .course_page(all_library.revision(), 0, 10)
                .await
                .expect("load unfiltered course page");
            assert_eq!(all_page.total, 3);
            assert_eq!(
                all_page
                    .rows
                    .iter()
                    .map(|row| row.id.as_str())
                    .collect::<Vec<_>>(),
                ["course-marker", "course-missing", "course-copy"]
            );
            all_library.close().await.expect("close unfiltered library");
            assert_no_sidecars(&all_path);
        });
    }

    #[test]
    fn replacement_snapshots_reject_the_previous_revision() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let first = LibraryDatabase::open_snapshot_read_only(&path, revision(41))
                .await
                .expect("open first snapshot");
            assert_eq!(first.revision(), 41);
            first.close().await.expect("close first snapshot");

            let mut replacement = LibraryDatabase::open_snapshot_read_only(&path, revision(42))
                .await
                .expect("open replacement snapshot");
            assert!(matches!(
                replacement.course_page(41, 0, 1).await,
                Err(LibraryError::StaleRevision {
                    expected: 41,
                    actual: 42
                })
            ));
            assert_eq!(
                replacement
                    .course_page(42, 0, 1)
                    .await
                    .expect("load replacement page")
                    .revision,
                42
            );
            replacement
                .close()
                .await
                .expect("close replacement snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn missing_library_setting_infers_the_legacy_root() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(&path, "DELETE FROM app_settings WHERE key = 'libraryPath'").await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(51))
                .await
                .expect("open fixture without saved root");
            assert_eq!(library.library_path(), Some("/fixtures/library"));
            assert_eq!(
                library
                    .course_page(51, 0, 10)
                    .await
                    .expect("load inferred-root page")
                    .total,
                3
            );
            library.close().await.expect("close inferred-root snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn missing_stored_thumbnail_uses_the_first_video_lesson() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "UPDATE courses SET thumbnail_source_path = NULL WHERE id = 'course-marker'",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(61))
                .await
                .expect("open fixture without stored thumbnail");
            let page = library
                .course_page(61, 0, 2)
                .await
                .expect("load thumbnail fallback page");
            assert_eq!(
                page.rows[0].thumbnail_source_path.as_deref(),
                Some("/fixtures/library/Systems 日本語/01 入門/01 welcome.mp4")
            );
            library.close().await.expect("close thumbnail snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn root_filters_preserve_platform_separators_and_posix_case() {
        assert_eq!(child_path_prefix(r"C:\"), r"C:\");
        assert_eq!(child_path_prefix(r"\\server\share\"), r"\\server\share\");
        assert_eq!(
            child_path_prefix(r"/tmp/name\with-backslash"),
            r"/tmp/name\with-backslash/"
        );

        block_on(async {
            let (_windows_temp, windows_path) = copied_fixture();
            mutate_fixture(
                &windows_path,
                "DELETE FROM courses;
                 UPDATE app_settings SET value = 'C:\\' WHERE key = 'libraryPath';
                 INSERT INTO courses (id, name, path)
                 VALUES
                   ('windows-child', 'Windows child', 'C:\\Course'),
                   ('windows-decoy', 'Windows decoy', 'C:/Course')",
            )
            .await;
            let mut windows = LibraryDatabase::open_snapshot_read_only(&windows_path, revision(71))
                .await
                .expect("open Windows-root fixture");
            let windows_page = windows
                .course_page(71, 0, 10)
                .await
                .expect("load Windows-root page");
            assert_eq!(windows_page.total, 1);
            assert_eq!(windows_page.rows[0].id, "windows-child");
            windows.close().await.expect("close Windows-root snapshot");
            assert_no_sidecars(&windows_path);

            let (_posix_temp, posix_path) = copied_fixture();
            mutate_fixture(
                &posix_path,
                "INSERT INTO courses (id, name, path)
                 VALUES ('case-decoy', 'Case decoy', '/fixtures/Library/Case decoy')",
            )
            .await;
            let mut posix = LibraryDatabase::open_snapshot_read_only(&posix_path, revision(72))
                .await
                .expect("open POSIX-root fixture");
            let posix_page = posix
                .course_page(72, 0, 10)
                .await
                .expect("load POSIX-root page");
            assert_eq!(posix_page.total, 3);
            assert!(posix_page.rows.iter().all(|row| row.id != "case-decoy"));
            posix.close().await.expect("close POSIX-root snapshot");
            assert_no_sidecars(&posix_path);
        });
    }

    #[test]
    fn trailing_separator_roots_do_not_duplicate_the_exact_root() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "UPDATE app_settings SET value = '/fixtures/library/' WHERE key = 'libraryPath';
                 INSERT INTO courses (id, name, path)
                 VALUES ('exact-root', 'Exact root', '/fixtures/library/')",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(76))
                .await
                .expect("open trailing-root fixture");
            let page = library
                .course_page(76, 0, 10)
                .await
                .expect("load trailing-root page");
            assert_eq!(page.total, 4);
            assert_eq!(
                page.rows
                    .iter()
                    .filter(|row| row.id == "exact-root")
                    .count(),
                1
            );
            library.close().await.expect("close trailing-root snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn missing_windows_setting_infers_an_absolute_drive_root() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "DELETE FROM courses;
                 DELETE FROM app_settings WHERE key = 'libraryPath';
                 INSERT INTO courses (id, name, path)
                 VALUES
                   ('drive-a', 'Drive A', 'C:\\A'),
                   ('drive-b', 'Drive B', 'C:\\B')",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(81))
                .await
                .expect("open fixture without Windows root setting");
            assert_eq!(library.library_path(), Some(r"C:\"));
            assert_eq!(
                library
                    .course_page(81, 0, 10)
                    .await
                    .expect("load inferred Windows-root page")
                    .total,
                2
            );
            library.close().await.expect("close Windows-root snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn thumbnail_fallback_uses_natural_section_order() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "DELETE FROM courses;
                 UPDATE app_settings SET value = '/natural' WHERE key = 'libraryPath';
                 INSERT INTO courses (id, name, path)
                 VALUES ('natural-course', 'Natural course', '/natural/Course');
                 INSERT INTO sections (id, course_id, name, order_index)
                 VALUES
                   ('section-10', 'natural-course', '10 Advanced', 0),
                   ('section-2', 'natural-course', '2 Intro', 0);
                 INSERT INTO lessons
                   (id, course_id, section_id, section_name, name, path, type, order_index)
                 VALUES
                   ('lesson-10', 'natural-course', 'section-10', '10 Advanced', '1 Video', '/natural/Course/10 Advanced/video.mp4', 'video', 0),
                   ('lesson-2', 'natural-course', 'section-2', '2 Intro', '1 Video', '/natural/Course/2 Intro/video.mp4', 'video', 0)",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(91))
                .await
                .expect("open natural-order fixture");
            let page = library
                .course_page(91, 0, 10)
                .await
                .expect("load natural-order page");
            assert_eq!(page.rows.len(), 1);
            assert_eq!(
                page.rows[0].thumbnail_source_path.as_deref(),
                Some("/natural/Course/2 Intro/video.mp4")
            );
            library.close().await.expect("close natural-order snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn rooted_course_plan_uses_the_path_index_and_one_lesson_window() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(101))
                .await
                .expect("open query-plan fixture");
            let page_sql = course_page_sql(true);
            assert_eq!(
                page_sql.matches("PARTITION BY lessons.course_id").count(),
                1
            );

            let (prefix, upper_bound) = child_path_range("/fixtures/library");
            let plan_sql = format!("EXPLAIN QUERY PLAN {page_sql}");
            let details = sqlx::query(&plan_sql)
                .bind("/fixtures/library")
                .bind(prefix)
                .bind(upper_bound)
                .bind(10_i64)
                .bind(0_i64)
                .fetch_all(&mut library.connection)
                .await
                .expect("explain rooted course page")
                .into_iter()
                .map(|row| row.get::<String, _>("detail"))
                .collect::<Vec<_>>();
            assert_eq!(
                details
                    .iter()
                    .filter(|detail| {
                        detail.contains("SEARCH courses USING INDEX idx_courses_path")
                    })
                    .count(),
                2,
                "{details:#?}"
            );
            assert!(
                details.iter().all(|detail| detail != "SCAN courses"),
                "{details:#?}"
            );
            library.close().await.expect("close query-plan snapshot");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn lesson_pages_match_the_frozen_v16_course() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let original = fs::read(&path).expect("read original fixture copy");
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(111))
                .await
                .expect("open lesson-page fixture");

            let first = library
                .lesson_page(111, "course-marker", None, 0, 1)
                .await
                .expect("load first lesson page");
            assert_eq!(first.revision, 111);
            assert_eq!(first.course_id, "course-marker");
            assert_eq!(first.section_id, None);
            assert_eq!(first.offset, 0);
            assert_eq!(first.total, 2);
            assert_eq!(first.rows.len(), 1);
            assert_eq!(
                first,
                library
                    .lesson_page(111, "course-marker", None, 0, 1)
                    .await
                    .expect("repeat first lesson page")
            );

            let video = &first.rows[0];
            assert_eq!(video.id, "lesson-video");
            assert_eq!(video.course_id, "course-marker");
            assert_eq!(video.section_id.as_deref(), Some("section-marker-intro"));
            assert_eq!(video.section_name, "01 入門");
            assert_eq!(video.name, "01 welcome");
            assert_eq!(
                video.path,
                "/fixtures/library/Systems 日本語/01 入門/01 welcome.mp4"
            );
            assert_eq!(
                video.relative_path.as_deref(),
                Some("01 入門/01 welcome.mp4")
            );
            assert_eq!(video.kind, "video");
            assert_eq!(video.duration, 600);
            assert_eq!(video.file_size, 1_048_576);
            assert!(!video.completed);
            assert_eq!(video.watched_time, 320);
            assert_eq!(video.last_position, 318.5);
            assert_eq!(video.order, 0);
            assert_eq!(video.subtitles.len(), 2);
            assert_eq!(video.subtitles[0].language, "en");
            assert_eq!(video.subtitles[0].label, "English");
            assert_eq!(video.subtitles[1].language, "ja");
            assert_eq!(video.subtitles[1].label, "日本語");

            let second = library
                .lesson_page(111, "course-marker", None, 1, 1)
                .await
                .expect("load second lesson page");
            assert_eq!(second.total, 2);
            assert_eq!(second.rows.len(), 1);
            assert_eq!(second.rows[0].id, "lesson-document");
            assert_eq!(second.rows[0].kind, "document");
            assert!(second.rows[0].completed);
            assert!(second.rows[0].subtitles.is_empty());

            let section = library
                .lesson_page(111, "course-marker", Some("section-marker-deep"), 0, 10)
                .await
                .expect("load section lesson page");
            assert_eq!(section.section_id.as_deref(), Some("section-marker-deep"));
            assert_eq!(section.total, 1);
            assert_eq!(section.rows[0].id, "lesson-document");

            let exhausted = library
                .lesson_page(111, "course-marker", None, 2, 10)
                .await
                .expect("load exhausted lesson page");
            assert_eq!(exhausted.total, 2);
            assert!(exhausted.rows.is_empty());

            let missing_section = library
                .lesson_page(111, "course-marker", Some("missing-section"), 0, 10)
                .await
                .expect("load missing-section lesson page");
            assert_eq!(missing_section.total, 0);
            assert!(missing_section.rows.is_empty());

            let missing = library
                .lesson_page(111, "missing-course", None, 0, 10)
                .await
                .expect("load missing-course lesson page");
            assert_eq!(missing.total, 0);
            assert!(missing.rows.is_empty());

            library.close().await.expect("close lesson-page fixture");
            assert_eq!(fs::read(&path).expect("reread fixture copy"), original);
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn lesson_pages_reject_invalid_bounds_and_stale_revisions() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(121))
                .await
                .expect("open invalid lesson-page fixture");

            assert!(matches!(
                library.lesson_page(122, "course-marker", None, 0, 1).await,
                Err(LibraryError::StaleRevision {
                    expected: 122,
                    actual: 121
                })
            ));
            assert!(matches!(
                library.lesson_page(121, "course-marker", None, 0, 0).await,
                Err(LibraryError::InvalidPageSize { limit: 0 })
            ));
            assert!(matches!(
                library
                    .lesson_page(121, "course-marker", None, 0, 201)
                    .await,
                Err(LibraryError::InvalidPageSize { limit: 201 })
            ));
            assert!(matches!(
                library
                    .lesson_page(121, "course-marker", None, i64::MAX as u64 + 1, 1)
                    .await,
                Err(LibraryError::InvalidOffset { .. })
            ));

            library
                .close()
                .await
                .expect("close invalid lesson-page fixture");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn lesson_pages_preserve_resolved_section_and_natural_order() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "INSERT INTO courses (id, name, path)
                 VALUES
                   ('natural-lessons', 'Natural lessons', '/fixtures/library/Natural lessons'),
                   ('outside-course', 'Outside course', '/outside/Outside course');
                 INSERT INTO sections (id, course_id, name, order_index)
                 VALUES
                   ('natural-10', 'natural-lessons', '10 Advanced', 0),
                   ('natural-2', 'natural-lessons', '2 Intro', 0);
                 INSERT INTO lessons
                   (id, course_id, section_id, section_name, name, path, type, order_index)
                 VALUES
                   ('natural-fallback', 'natural-lessons', 'stale-section', '2 Intro', '1 Fallback', '/fixtures/library/Natural lessons/fallback.mp4', 'video', 0),
                   ('natural-two', 'natural-lessons', 'natural-2', '2 Intro', '2 Topic', '/fixtures/library/Natural lessons/two.mp4', 'video', 0),
                   ('natural-ten', 'natural-lessons', 'natural-2', '2 Intro', '10 Topic', '/fixtures/library/Natural lessons/ten.mp4', 'video', 0),
                   ('natural-tie-b', 'natural-lessons', 'natural-2', '2 Intro', '20 Topic', '/fixtures/library/Natural lessons/tie-b.mp4', 'video', 0),
                   ('natural-tie-a', 'natural-lessons', 'natural-2', '2 Intro', '20 Topic', '/fixtures/library/Natural lessons/tie-a.mp4', 'video', 0),
                   ('natural-advanced', 'natural-lessons', 'natural-10', '10 Advanced', '0 Global', '/fixtures/library/Natural lessons/advanced.mp4', 'video', -100),
                   ('outside-lesson', 'outside-course', NULL, 'Course', 'Outside', '/outside/Outside course/outside.mp4', 'video', 0);
                 INSERT INTO lesson_subtitles
                   (id, lesson_id, path, language, label, order_index)
                 VALUES
                   ('natural-default-subtitle', 'natural-fallback', '/fixtures/library/Natural lessons/fallback.srt', NULL, NULL, 0)",
            )
            .await;
            let before = fs::read(&path).expect("read natural lesson fixture");
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(131))
                .await
                .expect("open natural lesson fixture");

            let page = library
                .lesson_page(131, "natural-lessons", None, 0, 10)
                .await
                .expect("load naturally ordered lesson page");
            assert_eq!(page.total, 6);
            assert_eq!(
                page.rows
                    .iter()
                    .map(|lesson| lesson.id.as_str())
                    .collect::<Vec<_>>(),
                [
                    "natural-fallback",
                    "natural-two",
                    "natural-ten",
                    "natural-tie-b",
                    "natural-tie-a",
                    "natural-advanced",
                ]
            );
            assert_eq!(page.rows[0].section_id.as_deref(), Some("natural-2"));
            assert_eq!(page.rows[0].section_name, "2 Intro");
            assert_eq!(page.rows[0].subtitles[0].language, "default");
            assert_eq!(page.rows[0].subtitles[0].label, "default");

            let section = library
                .lesson_page(131, "natural-lessons", Some("natural-2"), 0, 10)
                .await
                .expect("load resolved section lesson page");
            assert_eq!(section.total, 5);
            assert_eq!(section.rows[0].id, "natural-fallback");

            let outside = library
                .lesson_page(131, "outside-course", None, 0, 10)
                .await
                .expect("load outside-root lesson page");
            assert_eq!(outside.total, 0);
            assert!(outside.rows.is_empty());

            library.close().await.expect("close natural lesson fixture");
            assert_eq!(
                fs::read(&path).expect("reread natural lesson fixture"),
                before
            );
            assert_no_sidecars(&path);
        });
    }
    #[test]
    fn lesson_order_matches_unicode_and_nullable_projection_rules() {
        assert!(natural_cmp("ä", "z").is_lt());
        assert!(natural_cmp("a", "A").is_lt());
        assert!(natural_cmp("file2", "file02").is_eq());
        assert!(natural_cmp("file02", "file10").is_lt());

        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "INSERT INTO courses (id, name, path)
                 VALUES ('unicode-course', 'Unicode course', '/fixtures/library/Unicode course');
                 INSERT INTO sections (id, course_id, name, order_index)
                 VALUES
                   ('unicode-lower', 'unicode-course', 'a', 0),
                   ('unicode-upper', 'unicode-course', 'A', 0),
                   ('unicode-umlaut', 'unicode-course', 'ä', NULL),
                   ('unicode-z', 'unicode-course', 'z', 5);
                 INSERT INTO lessons
                   (id, course_id, section_id, section_name, name, path, type, order_index)
                 VALUES
                   ('unicode-lower-a', 'unicode-course', '', 'a', 'a', '/fixtures/library/Unicode course/lower-a.mp4', 'video', NULL),
                   ('unicode-lower-umlaut', 'unicode-course', 'unicode-lower', 'a', 'ä', '/fixtures/library/Unicode course/lower-umlaut.mp4', 'video', NULL),
                   ('unicode-lower-z', 'unicode-course', 'unicode-lower', 'a', 'z', '/fixtures/library/Unicode course/lower-z.mp4', 'video', 5),
                   ('unicode-upper-lesson', 'unicode-course', 'unicode-upper', 'A', 'Only', '/fixtures/library/Unicode course/upper.mp4', 'video', 0),
                   ('unicode-umlaut-lesson', 'unicode-course', 'unicode-umlaut', 'ä', 'Only', '/fixtures/library/Unicode course/umlaut.mp4', 'video', 0),
                   ('unicode-synthetic-lesson', 'unicode-course', NULL, 'Synthetic', 'Only', '/fixtures/library/Unicode course/synthetic.mp4', 'video', 0),
                   ('unicode-z-lesson', 'unicode-course', 'unicode-z', 'z', 'Only', '/fixtures/library/Unicode course/z.mp4', 'video', 0)",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(136))
                .await
                .expect("open Unicode lesson fixture");
            let page = library
                .lesson_page(136, "unicode-course", None, 0, 10)
                .await
                .expect("load Unicode lesson page");

            assert_eq!(
                page.rows
                    .iter()
                    .map(|lesson| lesson.id.as_str())
                    .collect::<Vec<_>>(),
                [
                    "unicode-lower-a",
                    "unicode-lower-umlaut",
                    "unicode-lower-z",
                    "unicode-upper-lesson",
                    "unicode-umlaut-lesson",
                    "unicode-synthetic-lesson",
                    "unicode-z-lesson",
                ]
            );
            assert_eq!(
                page.rows
                    .iter()
                    .take(3)
                    .map(|lesson| lesson.order)
                    .collect::<Vec<_>>(),
                [0, 1, 5]
            );

            let synthetic = library
                .lesson_page(
                    136,
                    "unicode-course",
                    Some("unicode-course:section:4"),
                    0,
                    10,
                )
                .await
                .expect("load synthetic section page");
            assert_eq!(synthetic.total, 1);
            assert_eq!(synthetic.rows[0].id, "unicode-synthetic-lesson");
            assert_eq!(
                synthetic.rows[0].section_id.as_deref(),
                Some("unicode-course:section:4")
            );

            library.close().await.expect("close Unicode lesson fixture");
            assert_no_sidecars(&path);
        });
    }

    #[test]
    fn large_lesson_pages_build_one_revision_scoped_order_index() {
        block_on(async {
            let (_temp, path) = copied_fixture();
            mutate_fixture(
                &path,
                "INSERT INTO courses (id, name, path)
                 VALUES ('bulk-course', 'Bulk course', '/fixtures/library/Bulk course');
                 INSERT INTO sections (id, course_id, name, order_index)
                 VALUES ('bulk-section', 'bulk-course', 'Course', 0);
                 WITH RECURSIVE numbers(value) AS (
                     VALUES (0)
                     UNION ALL
                     SELECT value + 1 FROM numbers WHERE value < 99999
                 )
                 INSERT INTO lessons
                   (id, course_id, section_id, section_name, name, path, type, order_index)
                 SELECT printf('bulk-%06d', value),
                        'bulk-course',
                        'bulk-section',
                        'Course',
                        printf('Lesson %d', value),
                        printf('/fixtures/library/Bulk course/%06d.mp4', value),
                        'video',
                        0
                 FROM numbers",
            )
            .await;
            let mut library = LibraryDatabase::open_snapshot_read_only(&path, revision(138))
                .await
                .expect("open 100,000-lesson fixture");

            let deep = library
                .lesson_page(138, "bulk-course", None, 99_998, 2)
                .await
                .expect("load deep lesson page");
            assert_eq!(deep.total, 100_000);
            assert_eq!(
                deep.rows
                    .iter()
                    .map(|lesson| lesson.id.as_str())
                    .collect::<Vec<_>>(),
                ["bulk-099998", "bulk-099999"]
            );
            assert_eq!(library.lesson_order_index_builds, 1);
            assert_eq!(
                library
                    .lesson_order_indexes
                    .get("bulk-course")
                    .expect("cached bulk lesson order")
                    .lessons
                    .len(),
                100_000
            );

            let section = library
                .lesson_page(138, "bulk-course", Some("bulk-section"), 0, 2)
                .await
                .expect("load cached section page");
            assert_eq!(section.total, 100_000);
            assert_eq!(section.rows[0].id, "bulk-000000");
            assert_eq!(library.lesson_order_index_builds, 1);

            library.close().await.expect("close 100,000-lesson fixture");
            assert_no_sidecars(&path);
        });
    }
}
