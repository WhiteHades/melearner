use serde::Serialize;
use sqlx::sqlite::SqliteRow;
use sqlx::{Connection, Row};

use super::{LibraryDatabase, LibraryError, child_path_range};

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LibraryStats {
    pub(crate) revision: u64,
    pub(crate) total_courses: u64,
    pub(crate) available_courses: u64,
    pub(crate) missing_courses: u64,
    pub(crate) sections: u64,
    pub(crate) lessons: u64,
    pub(crate) completed_lessons: u64,
    pub(crate) completion_percent: u32,
    pub(crate) bytes: u64,
    pub(crate) watched_seconds: u64,
    pub(crate) total_seconds: u64,
    pub(crate) media_types: Vec<MediaTypeStats>,
    pub(crate) top_courses: Vec<TopCourseStats>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct MediaTypeStats {
    #[serde(rename = "type")]
    pub(crate) kind: String,
    pub(crate) lessons: u64,
    pub(crate) bytes: u64,
    pub(crate) completed: u64,
    pub(crate) watched_seconds: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct TopCourseStats {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) lessons: u64,
    pub(crate) completed_lessons: u64,
    pub(crate) bytes: u64,
    pub(crate) watched_seconds: u64,
}

impl LibraryDatabase {
    pub(crate) async fn stats_snapshot(
        &mut self,
        expected_revision: u64,
    ) -> Result<LibraryStats, LibraryError> {
        if expected_revision != self.revision {
            return Err(LibraryError::StaleRevision {
                expected: expected_revision,
                actual: self.revision,
            });
        }

        let library_path = self
            .library_path
            .as_deref()
            .filter(|library_path| !library_path.is_empty());
        let path_range = library_path.map(child_path_range);
        let rooted = library_path.is_some();
        let mut transaction = self.connection.begin().await?;

        let totals_sql = totals_sql(rooted);
        let mut totals_query = sqlx::query(&totals_sql);
        if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            totals_query = totals_query
                .bind(library_path)
                .bind(prefix)
                .bind(upper_bound);
        }
        let totals = totals_query.fetch_one(&mut *transaction).await?;

        let media_sql = media_sql(rooted);
        let mut media_query = sqlx::query(&media_sql);
        if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            media_query = media_query
                .bind(library_path)
                .bind(prefix)
                .bind(upper_bound);
        }
        let media_records = media_query.fetch_all(&mut *transaction).await?;

        let top_courses_sql = top_courses_sql(rooted);
        let mut top_courses_query = sqlx::query(&top_courses_sql);
        if let (Some(library_path), Some((prefix, upper_bound))) =
            (library_path, path_range.as_ref())
        {
            top_courses_query = top_courses_query
                .bind(library_path)
                .bind(prefix)
                .bind(upper_bound);
        }
        let top_course_records = top_courses_query.fetch_all(&mut *transaction).await?;
        transaction.commit().await?;

        let total_courses = aggregate(&totals, "total_courses")?;
        let available_courses = aggregate(&totals, "available_courses")?;
        let missing_courses = aggregate(&totals, "missing_courses")?;
        let sections = aggregate(&totals, "sections")?;
        let lessons = aggregate(&totals, "lessons")?;
        let completed_lessons = aggregate(&totals, "completed_lessons")?;
        let bytes = aggregate(&totals, "bytes")?;
        let watched_seconds = aggregate(&totals, "watched_seconds")?;
        let total_seconds = aggregate(&totals, "total_seconds")?;

        let media_types = media_records
            .into_iter()
            .map(|row| {
                Ok(MediaTypeStats {
                    kind: row.try_get("type")?,
                    lessons: aggregate(&row, "lessons")?,
                    bytes: aggregate(&row, "bytes")?,
                    completed: aggregate(&row, "completed")?,
                    watched_seconds: aggregate(&row, "watched_seconds")?,
                })
            })
            .collect::<Result<Vec<_>, LibraryError>>()?;
        let top_courses = top_course_records
            .into_iter()
            .map(|row| {
                Ok(TopCourseStats {
                    id: row.try_get("id")?,
                    name: row.try_get("name")?,
                    lessons: aggregate(&row, "lessons")?,
                    completed_lessons: aggregate(&row, "completed_lessons")?,
                    bytes: aggregate(&row, "bytes")?,
                    watched_seconds: aggregate(&row, "watched_seconds")?,
                })
            })
            .collect::<Result<Vec<_>, LibraryError>>()?;

        Ok(LibraryStats {
            revision: self.revision,
            total_courses,
            available_courses,
            missing_courses,
            sections,
            lessons,
            completed_lessons,
            completion_percent: completion_percent(completed_lessons, lessons)?,
            bytes,
            watched_seconds,
            total_seconds,
            media_types,
            top_courses,
        })
    }
}

fn filtered_course_source(rooted: bool) -> &'static str {
    if rooted {
        "SELECT *
         FROM courses INDEXED BY idx_courses_path
         WHERE path = ?1
         UNION ALL
         SELECT *
         FROM courses INDEXED BY idx_courses_path
         WHERE path > ?2 AND path < ?3"
    } else {
        "SELECT * FROM courses"
    }
}

fn totals_sql(rooted: bool) -> String {
    let course_source = filtered_course_source(rooted);
    format!(
        "WITH filtered_courses AS (
             {course_source}
         )
         SELECT
             (SELECT COUNT(*) FROM filtered_courses) AS total_courses,
             (SELECT COUNT(*) FROM filtered_courses WHERE missing_since IS NULL)
                 AS available_courses,
             (SELECT COUNT(*) FROM filtered_courses WHERE missing_since IS NOT NULL)
                 AS missing_courses,
             (SELECT COUNT(*)
              FROM sections
              JOIN filtered_courses ON filtered_courses.id = sections.course_id) AS sections,
             (SELECT COUNT(*)
              FROM lessons
              JOIN filtered_courses ON filtered_courses.id = lessons.course_id) AS lessons,
             (SELECT COALESCE(SUM(lessons.completed), 0)
              FROM lessons
              JOIN filtered_courses ON filtered_courses.id = lessons.course_id)
                 AS completed_lessons,
             (SELECT COALESCE(SUM(lessons.file_size), 0)
              FROM lessons
              JOIN filtered_courses ON filtered_courses.id = lessons.course_id) AS bytes,
             (SELECT COALESCE(SUM(lessons.watched_time), 0)
              FROM lessons
              JOIN filtered_courses ON filtered_courses.id = lessons.course_id)
                 AS watched_seconds,
             (SELECT COALESCE(SUM(lessons.duration), 0)
              FROM lessons
              JOIN filtered_courses ON filtered_courses.id = lessons.course_id)
                 AS total_seconds"
    )
}

fn media_sql(rooted: bool) -> String {
    let course_source = filtered_course_source(rooted);
    format!(
        "WITH filtered_courses AS (
             {course_source}
         )
         SELECT lessons.type AS type,
                COUNT(*) AS lessons,
                COALESCE(SUM(lessons.file_size), 0) AS bytes,
                COALESCE(SUM(lessons.completed), 0) AS completed,
                COALESCE(SUM(lessons.watched_time), 0) AS watched_seconds
         FROM lessons
         JOIN filtered_courses ON filtered_courses.id = lessons.course_id
         GROUP BY lessons.type
         ORDER BY CASE lessons.type
             WHEN 'video' THEN 0
             WHEN 'audio' THEN 1
             WHEN 'document' THEN 2
             WHEN 'quiz' THEN 3
         END"
    )
}

fn top_courses_sql(rooted: bool) -> String {
    let course_source = filtered_course_source(rooted);
    format!(
        "WITH filtered_courses AS (
             {course_source}
         ),
         course_stats AS (
             SELECT filtered_courses.id,
                    filtered_courses.name,
                    COUNT(lessons.id) AS lessons,
                    COALESCE(SUM(lessons.completed), 0) AS completed_lessons,
                    COALESCE(SUM(lessons.file_size), 0) AS bytes,
                    COALESCE(SUM(lessons.watched_time), 0) AS watched_seconds
             FROM filtered_courses
             LEFT JOIN lessons ON lessons.course_id = filtered_courses.id
             GROUP BY filtered_courses.id, filtered_courses.name
         )
         SELECT id, name, lessons, completed_lessons, bytes, watched_seconds
         FROM course_stats
         ORDER BY watched_seconds DESC,
                  bytes DESC,
                  name COLLATE MELEARNER_NATURAL,
                  id
         LIMIT 4"
    )
}

fn aggregate(row: &SqliteRow, column: &str) -> Result<u64, LibraryError> {
    let value: i64 = row.try_get(column)?;
    u64::try_from(value)
        .map_err(|_| LibraryError::Database(format!("negative Library stats aggregate {column}")))
}

fn completion_percent(completed_lessons: u64, lessons: u64) -> Result<u32, LibraryError> {
    if lessons == 0 {
        return Ok(0);
    }
    if completed_lessons > lessons {
        return Err(LibraryError::Database(
            "completed lesson count exceeds lesson count".to_string(),
        ));
    }
    let percent =
        (u128::from(completed_lessons) * 100 + u128::from(lessons) / 2) / u128::from(lessons);
    u32::try_from(percent)
        .map_err(|_| LibraryError::Database("invalid completion percent".to_string()))
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::num::NonZeroU64;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use sqlx::Executor;

    use super::{LibraryStats, MediaTypeStats, TopCourseStats};
    use crate::library::{LibraryDatabase, LibraryError};

    const CURRENT_SEED: &str = include_str!("../../../../fixtures/parity/database-current.sql");

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build stats test runtime")
            .block_on(future)
    }

    async fn stats_fixture(revision: u64) -> (tempfile::TempDir, LibraryDatabase) {
        let temp = tempfile::tempdir().expect("create stats fixture directory");
        let mut library = LibraryDatabase::open_current(
            temp.path(),
            NonZeroU64::new(revision).expect("nonzero stats revision"),
            Arc::new(AtomicBool::new(false)),
        )
        .await
        .expect("open current stats database");
        library
            .connection
            .execute(sqlx::raw_sql(CURRENT_SEED))
            .await
            .expect("seed current stats database");
        library.library_path = Some("/fixtures/library".to_string());
        (temp, library)
    }

    #[test]
    fn library_stats_match_the_current_fixture() {
        block_on(async {
            let (_temp, mut library) = stats_fixture(7).await;

            assert_eq!(
                library
                    .stats_snapshot(7)
                    .await
                    .expect("derive Library stats"),
                LibraryStats {
                    revision: 7,
                    total_courses: 3,
                    available_courses: 1,
                    missing_courses: 2,
                    sections: 4,
                    lessons: 4,
                    completed_lessons: 2,
                    completion_percent: 50,
                    bytes: 5_246_976,
                    watched_seconds: 620,
                    total_seconds: 1_200,
                    media_types: vec![
                        MediaTypeStats {
                            kind: "video".to_string(),
                            lessons: 3,
                            bytes: 5_242_880,
                            completed: 1,
                            watched_seconds: 620,
                        },
                        MediaTypeStats {
                            kind: "document".to_string(),
                            lessons: 1,
                            bytes: 4_096,
                            completed: 1,
                            watched_seconds: 0,
                        },
                    ],
                    top_courses: vec![
                        TopCourseStats {
                            id: "course-marker".to_string(),
                            name: "Systems 日本語".to_string(),
                            lessons: 2,
                            completed_lessons: 1,
                            bytes: 1_052_672,
                            watched_seconds: 320,
                        },
                        TopCourseStats {
                            id: "course-missing".to_string(),
                            name: "Archived Course".to_string(),
                            lessons: 1,
                            completed_lessons: 1,
                            bytes: 2_097_152,
                            watched_seconds: 300,
                        },
                        TopCourseStats {
                            id: "course-copy".to_string(),
                            name: "Copied Course".to_string(),
                            lessons: 1,
                            completed_lessons: 0,
                            bytes: 2_097_152,
                            watched_seconds: 0,
                        },
                    ],
                }
            );
        });
    }

    #[test]
    fn library_stats_are_empty_and_revision_gated() {
        block_on(async {
            let temp = tempfile::tempdir().expect("create empty stats directory");
            let mut library = LibraryDatabase::open_current(
                temp.path(),
                NonZeroU64::new(11).expect("nonzero empty stats revision"),
                Arc::new(AtomicBool::new(false)),
            )
            .await
            .expect("open empty stats database");

            assert_eq!(
                library
                    .stats_snapshot(11)
                    .await
                    .expect("derive empty Library stats"),
                LibraryStats {
                    revision: 11,
                    total_courses: 0,
                    available_courses: 0,
                    missing_courses: 0,
                    sections: 0,
                    lessons: 0,
                    completed_lessons: 0,
                    completion_percent: 0,
                    bytes: 0,
                    watched_seconds: 0,
                    total_seconds: 0,
                    media_types: Vec::new(),
                    top_courses: Vec::new(),
                }
            );
            assert!(matches!(
                library.stats_snapshot(12).await,
                Err(LibraryError::StaleRevision {
                    expected: 12,
                    actual: 11,
                })
            ));
        });
    }

    #[test]
    fn library_stats_share_root_scope_and_stable_top_course_order() {
        block_on(async {
            let (_temp, mut library) = stats_fixture(19).await;
            library
                .connection
                .execute(sqlx::raw_sql(
                    "INSERT INTO courses (
                         id, identity_id, name, path, fingerprint, last_scanned_at, missing_since
                     ) VALUES
                         ('course-2-a', 'identity-2-a', 'Course 2',
                          '/fixtures/library/Course 2 a', 'fp-2-a', CURRENT_TIMESTAMP, NULL),
                         ('course-2-b', 'identity-2-b', 'Course 2',
                          '/fixtures/library/Course 2 b', 'fp-2-b', CURRENT_TIMESTAMP, NULL),
                         ('course-10', 'identity-10', 'Course 10',
                          '/fixtures/library/Course 10', 'fp-10', CURRENT_TIMESTAMP, NULL),
                         ('course-decoy', 'identity-decoy', 'Decoy',
                          '/fixtures/library-other/Decoy', 'fp-decoy', CURRENT_TIMESTAMP, NULL);

                     INSERT INTO sections (id, course_id, name, order_index) VALUES
                         ('section-2-a', 'course-2-a', 'Intro', 0),
                         ('section-2-b', 'course-2-b', 'Intro', 0),
                         ('section-10', 'course-10', 'Intro', 0),
                         ('section-decoy', 'course-decoy', 'Intro', 0);

                     INSERT INTO lessons (
                         id, course_id, section_id, name, path, relative_path, type,
                         duration, watched_time, completed, file_size
                     ) VALUES
                         ('lesson-2-a', 'course-2-a', 'section-2-a', 'Lesson',
                          '/fixtures/library/Course 2 a/Lesson.mp4', 'Lesson.mp4', 'audio',
                          1000, 1000, 0, 100),
                         ('lesson-2-b', 'course-2-b', 'section-2-b', 'Lesson',
                          '/fixtures/library/Course 2 b/Lesson.mp4', 'Lesson.mp4', 'document',
                          1000, 1000, 0, 100),
                         ('lesson-10', 'course-10', 'section-10', 'Lesson',
                          '/fixtures/library/Course 10/Lesson.mp4', 'Lesson.mp4', 'quiz',
                          1000, 1000, 0, 100),
                         ('lesson-decoy', 'course-decoy', 'section-decoy', 'Lesson',
                          '/fixtures/library-other/Decoy/Lesson.mp4', 'Lesson.mp4', 'video',
                          9999, 9999, 1, 9999);",
                ))
                .await
                .expect("seed root-scoped stats rows");

            let stats = library
                .stats_snapshot(19)
                .await
                .expect("derive root-scoped Library stats");
            assert_eq!(stats.total_courses, 6);
            assert_eq!(stats.lessons, 7);
            assert_eq!(
                stats
                    .media_types
                    .iter()
                    .map(|media| media.kind.as_str())
                    .collect::<Vec<_>>(),
                ["video", "audio", "document", "quiz"]
            );
            assert_eq!(stats.top_courses.len(), 4);
            assert_eq!(
                stats
                    .top_courses
                    .iter()
                    .map(|course| course.id.as_str())
                    .collect::<Vec<_>>(),
                ["course-2-a", "course-2-b", "course-10", "course-marker",]
            );

            library.library_path = None;
            assert_eq!(
                library
                    .stats_snapshot(19)
                    .await
                    .expect("derive unrooted Library stats")
                    .total_courses,
                7
            );
        });
    }

    #[test]
    fn completion_percent_uses_nonnegative_half_up_rounding() {
        assert_eq!(super::completion_percent(0, 0).expect("empty percent"), 0);
        assert_eq!(super::completion_percent(1, 3).expect("one third"), 33);
        assert_eq!(super::completion_percent(2, 3).expect("two thirds"), 67);
        assert_eq!(super::completion_percent(1, 8).expect("one eighth"), 13);
        assert!(super::completion_percent(2, 1).is_err());
    }
}
