use serde::Serialize;
use sqlx::{Connection, Row, Sqlite, Transaction};

use super::{
    LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE, child_path_range, completion_percent,
};
use crate::{MutationControl, next_library_revision};

#[derive(Debug)]
pub(crate) struct ProgressInput {
    pub(crate) expected_revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) watched_time: u64,
    pub(crate) last_position: f64,
    pub(crate) completed: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ProgressUpdate {
    pub(crate) revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) watched_time: u64,
    pub(crate) last_position: f64,
    pub(crate) completed: bool,
}

#[derive(Debug)]
pub(crate) struct CourseAccessInput {
    pub(crate) expected_revision: u64,
    pub(crate) course_id: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CourseAccess {
    pub(crate) revision: u64,
    pub(crate) course_id: String,
    pub(crate) course_name: String,
    pub(crate) lesson_count: u64,
    pub(crate) completed_lesson_count: u64,
    pub(crate) progress_percent: u32,
    pub(crate) resume_lesson_id: Option<String>,
    pub(crate) resume_lesson_offset: Option<u64>,
    pub(crate) last_accessed: String,
}

#[derive(Debug)]
pub(crate) struct ActivityPageInput {
    pub(crate) expected_revision: u64,
    pub(crate) lookback_days: u32,
    pub(crate) offset: u64,
    pub(crate) limit: u32,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ActivityDay {
    pub(crate) date: String,
    pub(crate) watched_seconds: u64,
    pub(crate) lessons_touched: u64,
    pub(crate) completions: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ActivityDayPage {
    pub(crate) revision: u64,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) rows: Vec<ActivityDay>,
}

impl LibraryDatabase {
    pub(crate) async fn access_course(
        &mut self,
        input: CourseAccessInput,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<CourseAccess, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.course_id.is_empty() || input.course_id.contains('\0') {
            return Err(LibraryError::InvalidCourseAccess);
        }
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        let library_path = self
            .library_path
            .as_deref()
            .filter(|library_path| !library_path.is_empty());
        let record = if let Some(library_path) = library_path {
            let (prefix, upper_bound) = child_path_range(library_path);
            sqlx::query(
                "UPDATE courses
                 SET last_accessed = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
                 WHERE id = ?1
                   AND missing_since IS NULL
                   AND (path = ?2 OR (path > ?3 AND path < ?4))
                 RETURNING last_accessed",
            )
            .bind(&input.course_id)
            .bind(library_path)
            .bind(prefix)
            .bind(upper_bound)
            .fetch_optional(&mut *transaction)
            .await
        } else {
            sqlx::query(
                "UPDATE courses
                 SET last_accessed = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
                 WHERE id = ?1 AND missing_since IS NULL
                 RETURNING last_accessed",
            )
            .bind(&input.course_id)
            .fetch_optional(&mut *transaction)
            .await
        };
        let record = match record {
            Ok(Some(record)) => record,
            Ok(None) => {
                return Err(rollback_error(transaction, LibraryError::CourseNotFound).await);
            }
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let last_accessed = match record.try_get("last_accessed") {
            Ok(last_accessed) => last_accessed,
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let summary = match sqlx::query(
            "WITH ordered_lessons AS (
                 SELECT lessons.id, lessons.completed,
                        ROW_NUMBER() OVER (
                            ORDER BY sections.order_index,
                                     sections.name COLLATE MELEARNER_NATURAL,
                                     sections.id,
                                     lessons.order_index,
                                     lessons.name COLLATE MELEARNER_NATURAL,
                                     lessons.id
                        ) - 1 AS lesson_offset
                 FROM lessons INDEXED BY idx_lessons_course
                 INNER JOIN sections ON sections.id = lessons.section_id
                                    AND sections.course_id = lessons.course_id
                 WHERE lessons.course_id = ?1
             )
             SELECT courses.name AS course_name,
                    (SELECT COUNT(*) FROM ordered_lessons) AS lesson_count,
                    (SELECT COUNT(*) FROM ordered_lessons WHERE completed != 0)
                        AS completed_lesson_count,
                    (SELECT id FROM ordered_lessons
                     ORDER BY CASE WHEN completed = 0 THEN 0 ELSE 1 END, lesson_offset
                     LIMIT 1) AS resume_lesson_id,
                    (SELECT lesson_offset FROM ordered_lessons
                     ORDER BY CASE WHEN completed = 0 THEN 0 ELSE 1 END, lesson_offset
                     LIMIT 1) AS resume_lesson_offset
             FROM courses
             WHERE courses.id = ?1",
        )
        .bind(&input.course_id)
        .fetch_one(&mut *transaction)
        .await
        {
            Ok(summary) => summary,
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let lesson_count = match summary.try_get::<i64, _>("lesson_count") {
            Ok(value) if value >= 0 => value as u64,
            Ok(value) => {
                return Err(rollback_error(
                    transaction,
                    LibraryError::Database(format!("negative Lesson count {value}")),
                )
                .await);
            }
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let completed_lesson_count = match summary.try_get::<i64, _>("completed_lesson_count") {
            Ok(value) if value >= 0 => value as u64,
            Ok(value) => {
                return Err(rollback_error(
                    transaction,
                    LibraryError::Database(format!("negative completed Lesson count {value}")),
                )
                .await);
            }
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let resume_lesson_offset = match summary.try_get::<Option<i64>, _>("resume_lesson_offset") {
            Ok(Some(value)) => match u64::try_from(value) {
                Ok(value) => Some(value),
                Err(error) => {
                    return Err(rollback_error(
                        transaction,
                        LibraryError::Database(error.to_string()),
                    )
                    .await);
                }
            },
            Ok(None) => None,
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        let progress_percent = match completion_percent(completed_lesson_count, lesson_count) {
            Ok(progress_percent) => progress_percent,
            Err(error) => return Err(rollback_error(transaction, error).await),
        };
        let Some(next_revision) = next_library_revision() else {
            return Err(rollback_error(transaction, LibraryError::RevisionExhausted).await);
        };
        let access = CourseAccess {
            revision: next_revision.get(),
            course_id: input.course_id,
            course_name: summary.try_get("course_name")?,
            lesson_count,
            completed_lesson_count,
            progress_percent,
            resume_lesson_id: summary.try_get("resume_lesson_id")?,
            resume_lesson_offset,
            last_accessed,
        };
        if !response_fits(&access, max_payload_bytes) {
            return Err(rollback_error(
                transaction,
                LibraryError::ResponseTooLarge {
                    limit: max_payload_bytes,
                },
            )
            .await);
        }
        if control.is_cancelled() || !control.begin_commit() {
            return Err(rollback_error(transaction, LibraryError::Cancelled).await);
        }
        transaction.commit().await?;
        self.revision = next_revision.get();
        Ok(access)
    }

    pub(crate) async fn put_progress(
        &mut self,
        input: ProgressInput,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<ProgressUpdate, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.lesson_id.is_empty()
            || !input.last_position.is_finite()
            || input.last_position < 0.0
        {
            return Err(LibraryError::InvalidProgress);
        }
        let watched_time =
            i64::try_from(input.watched_time).map_err(|_| LibraryError::InvalidProgress)?;
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        if let Err(error) = apply_progress(&mut transaction, &input, watched_time).await {
            return Err(rollback_error(transaction, error).await);
        }
        let Some(next_revision) = next_library_revision() else {
            return Err(rollback_error(transaction, LibraryError::RevisionExhausted).await);
        };
        let update = ProgressUpdate {
            revision: next_revision.get(),
            lesson_id: input.lesson_id,
            watched_time: input.watched_time,
            last_position: input.last_position,
            completed: input.completed,
        };
        if !response_fits(&update, max_payload_bytes) {
            return Err(rollback_error(
                transaction,
                LibraryError::ResponseTooLarge {
                    limit: max_payload_bytes,
                },
            )
            .await);
        }
        if control.is_cancelled() || !control.begin_commit() {
            return Err(rollback_error(transaction, LibraryError::Cancelled).await);
        }
        transaction.commit().await?;
        self.revision = next_revision.get();
        Ok(update)
    }

    pub(crate) async fn activity_day_page(
        &mut self,
        input: ActivityPageInput,
    ) -> Result<ActivityDayPage, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.lookback_days == 0 {
            return Err(LibraryError::InvalidActivityLookback {
                days: input.lookback_days,
            });
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&input.limit) {
            return Err(LibraryError::InvalidPageSize { limit: input.limit });
        }
        let offset = i64::try_from(input.offset).map_err(|_| LibraryError::InvalidOffset {
            offset: input.offset,
        })?;
        let lookback = format!("-{} days", input.lookback_days - 1);
        let mut transaction = self.connection.begin().await?;
        let total: i64 = sqlx::query_scalar(
            "SELECT COUNT(*)
             FROM (
                 SELECT activity_date
                 FROM lesson_activity
                 WHERE activity_date >= date('now', ?1)
                 GROUP BY activity_date
             )",
        )
        .bind(&lookback)
        .fetch_one(&mut *transaction)
        .await?;
        let records = sqlx::query(
            "SELECT activity_date,
                    SUM(watched_seconds) AS watched_seconds,
                    COUNT(DISTINCT lesson_id) AS lessons_touched,
                    SUM(completed) AS completions
             FROM lesson_activity
             WHERE activity_date >= date('now', ?1)
             GROUP BY activity_date
             ORDER BY activity_date ASC
             LIMIT ?2 OFFSET ?3",
        )
        .bind(&lookback)
        .bind(i64::from(input.limit))
        .bind(offset)
        .fetch_all(&mut *transaction)
        .await?;
        transaction.commit().await?;

        let mut rows = Vec::with_capacity(records.len());
        for record in records {
            rows.push(ActivityDay {
                date: record.try_get("activity_date")?,
                watched_seconds: nonnegative(record.try_get("watched_seconds")?)?,
                lessons_touched: nonnegative(record.try_get("lessons_touched")?)?,
                completions: nonnegative(record.try_get("completions")?)?,
            });
        }
        Ok(ActivityDayPage {
            revision: self.revision,
            offset: input.offset,
            total: nonnegative(total)?,
            rows,
        })
    }
}

async fn apply_progress(
    transaction: &mut Transaction<'_, Sqlite>,
    input: &ProgressInput,
    watched_time: i64,
) -> Result<(), LibraryError> {
    let previous = sqlx::query(
        "SELECT course_id, watched_time, completed
         FROM lessons
         WHERE id = ?1",
    )
    .bind(&input.lesson_id)
    .fetch_optional(&mut **transaction)
    .await?;
    let Some(previous) = previous else {
        return Err(LibraryError::LessonNotFound);
    };
    let course_id: String = previous.try_get("course_id")?;
    let previous_watched: i64 = previous.try_get("watched_time")?;
    let previous_completed: bool = previous.try_get("completed")?;
    let watched_delta = watched_time.saturating_sub(previous_watched).max(0);
    let completion_changed = previous_completed != input.completed;

    sqlx::query(
        "UPDATE lessons
         SET watched_time = ?1,
             last_position = ?2,
             completed = ?3,
             updated_at = CURRENT_TIMESTAMP
         WHERE id = ?4",
    )
    .bind(watched_time)
    .bind(input.last_position)
    .bind(input.completed)
    .bind(&input.lesson_id)
    .execute(&mut **transaction)
    .await?;

    if watched_delta > 0 || completion_changed {
        sqlx::query(
            "INSERT INTO lesson_activity (
                 id, course_id, lesson_id, activity_date,
                 watched_seconds, completed, created_at
             ) VALUES (
                 lower(hex(randomblob(16))), ?1, ?2, date('now'), ?3, ?4,
                 CURRENT_TIMESTAMP
             )",
        )
        .bind(course_id)
        .bind(&input.lesson_id)
        .bind(watched_delta)
        .bind(completion_changed && input.completed)
        .execute(&mut **transaction)
        .await?;
    }
    Ok(())
}

async fn rollback_error(transaction: Transaction<'_, Sqlite>, error: LibraryError) -> LibraryError {
    match transaction.rollback().await {
        Ok(()) => error,
        Err(rollback_error) => LibraryError::from(rollback_error),
    }
}

fn response_fits(value: &impl Serialize, max_payload_bytes: usize) -> bool {
    serde_json::to_vec(value).is_ok_and(|payload| payload.len() <= max_payload_bytes)
}

fn nonnegative(value: i64) -> Result<u64, LibraryError> {
    u64::try_from(value)
        .map_err(|_| LibraryError::Database("negative activity aggregate".to_string()))
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use sqlx::Row;

    use super::{ActivityPageInput, CourseAccessInput, ProgressInput};
    use crate::library::{LibraryDatabase, LibraryError};
    use crate::{MutationControl, next_library_revision};

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build progress test runtime")
            .block_on(future)
    }

    async fn progress_fixture() -> (tempfile::TempDir, LibraryDatabase) {
        let temp = tempfile::tempdir().expect("create progress fixture");
        let mut library = LibraryDatabase::open_current(
            temp.path(),
            next_library_revision().expect("allocate fixture revision"),
            Arc::new(AtomicBool::new(false)),
        )
        .await
        .expect("open progress fixture");
        sqlx::raw_sql(
            "INSERT INTO app_settings (key, value) VALUES ('libraryPath', '/library');
             INSERT INTO courses (
                 id, identity_id, name, path, fingerprint, last_scanned_at,
                 last_accessed, missing_since
             ) VALUES
                 ('course', 'identity', 'Course', '/library/Course', 'fingerprint',
                  '2026-07-15T00:00:00.000Z', NULL, NULL),
                 ('other-course', 'other-identity', 'Other', '/library/Other',
                  'other-fingerprint', '2026-07-15T00:00:00.000Z',
                  '2000-01-01T00:00:00.000Z', NULL),
                 ('missing-course', 'missing-identity', 'Missing', '/library/Missing',
                  'missing-fingerprint', '2026-07-15T00:00:00.000Z', NULL,
                  '2026-07-14T00:00:00.000Z'),
                 ('outside-course', 'outside-identity', 'Outside', '/outside/Course',
                  'outside-fingerprint', '2026-07-15T00:00:00.000Z', NULL, NULL);
             INSERT INTO sections (id, course_id, name, order_index) VALUES
                 ('section', 'course', 'Section', 0);
             INSERT INTO lessons (
                 id, course_id, section_id, name, path, relative_path, type,
                 duration, watched_time, completed, order_index, last_position
             ) VALUES (
                 'lesson', 'course', 'section', 'Lesson',
                 '/library/Course/Section/lesson.mp4', 'Section/lesson.mp4', 'video',
                 120, 0, 0, 0, 0
             );",
        )
        .execute(&mut library.connection)
        .await
        .expect("seed progress fixture");
        library.library_path = Some("/library".to_string());
        (temp, library)
    }

    fn course_access_input(revision: u64, course_id: &str) -> CourseAccessInput {
        CourseAccessInput {
            expected_revision: revision,
            course_id: course_id.to_string(),
        }
    }

    fn progress_input(
        revision: u64,
        watched_time: u64,
        last_position: f64,
        completed: bool,
    ) -> ProgressInput {
        ProgressInput {
            expected_revision: revision,
            lesson_id: "lesson".to_string(),
            watched_time,
            last_position,
            completed,
        }
    }

    async fn activity_count(library: &mut LibraryDatabase) -> i64 {
        sqlx::query_scalar("SELECT COUNT(*) FROM lesson_activity")
            .fetch_one(&mut library.connection)
            .await
            .expect("count activity")
    }

    #[test]
    fn course_access_commits_reorders_and_persists() {
        block_on(async {
            let (temp, mut library) = progress_fixture().await;
            let initial_revision = library.revision();
            let before = library
                .course_page(initial_revision, 0, 10)
                .await
                .expect("load Courses before access");
            assert_eq!(before.rows[0].id, "other-course");

            let accessed = library
                .access_course(
                    course_access_input(initial_revision, "course"),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("access Course");
            assert!(accessed.revision > initial_revision);
            assert_eq!(accessed.revision, library.revision());
            assert_eq!(accessed.course_id, "course");
            assert_eq!(accessed.course_name, "Course");
            assert_eq!(accessed.lesson_count, 1);
            assert_eq!(accessed.completed_lesson_count, 0);
            assert_eq!(accessed.progress_percent, 0);
            assert_eq!(accessed.resume_lesson_id.as_deref(), Some("lesson"));
            assert_eq!(accessed.resume_lesson_offset, Some(0));
            assert!(accessed.last_accessed.contains('T'));
            assert!(accessed.last_accessed.ends_with('Z'));

            assert!(matches!(
                library.course_page(initial_revision, 0, 10).await,
                Err(LibraryError::StaleRevision { .. })
            ));
            let after = library
                .course_page(accessed.revision, 0, 10)
                .await
                .expect("load Courses after access");
            assert_eq!(after.rows[0].id, "course");
            assert_eq!(
                after.rows[0].last_accessed.as_deref(),
                Some(accessed.last_accessed.as_str())
            );

            drop(library);
            let mut reopened = LibraryDatabase::open_current(
                temp.path(),
                next_library_revision().expect("allocate reopened revision"),
                Arc::new(AtomicBool::new(false)),
            )
            .await
            .expect("reopen Course access fixture");
            let persisted: Option<String> =
                sqlx::query_scalar("SELECT last_accessed FROM courses WHERE id = 'course'")
                    .fetch_one(&mut reopened.connection)
                    .await
                    .expect("load persisted Course access");
            assert_eq!(persisted.as_deref(), Some(accessed.last_accessed.as_str()));
        });
    }

    #[test]
    fn rejected_course_access_leaves_timestamp_and_revision_unchanged() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            let revision = library.revision();
            let cancelled = MutationControl::new();
            assert!(cancelled.cancel());

            for input in [
                course_access_input(revision + 1, "course"),
                course_access_input(revision, ""),
                course_access_input(revision, "bad\0id"),
                course_access_input(revision, "unknown-course"),
                course_access_input(revision, "missing-course"),
                course_access_input(revision, "outside-course"),
            ] {
                assert!(
                    library
                        .access_course(input, usize::MAX, &MutationControl::new())
                        .await
                        .is_err()
                );
            }
            assert!(
                library
                    .access_course(
                        course_access_input(revision, "course"),
                        usize::MAX,
                        &cancelled,
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .access_course(
                        course_access_input(revision, "course"),
                        1,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );

            assert_eq!(library.revision(), revision);
            assert_eq!(
                sqlx::query_scalar::<_, Option<String>>(
                    "SELECT last_accessed FROM courses WHERE id = 'course'"
                )
                .fetch_one(&mut library.connection)
                .await
                .expect("load unchanged Course access"),
                None
            );
        });
    }

    #[test]
    fn course_access_database_failure_rolls_back() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            sqlx::raw_sql(
                "CREATE TRIGGER reject_Course_access
                 BEFORE UPDATE OF last_accessed ON courses
                 BEGIN
                     SELECT RAISE(ABORT, 'Course access rejected');
                 END;",
            )
            .execute(&mut library.connection)
            .await
            .expect("install failing Course access trigger");
            let revision = library.revision();

            assert!(matches!(
                library
                    .access_course(
                        course_access_input(revision, "course"),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::Database(_))
            ));
            assert_eq!(library.revision(), revision);
            assert_eq!(
                sqlx::query_scalar::<_, Option<String>>(
                    "SELECT last_accessed FROM courses WHERE id = 'course'"
                )
                .fetch_one(&mut library.connection)
                .await
                .expect("load rolled back Course access"),
                None
            );
        });
    }

    #[test]
    fn progress_and_activity_commit_atomically_with_new_revisions() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            let initial_revision = library.revision();

            for (watched_time, completed) in [(10, false), (15, true), (12, true)] {
                let update = library
                    .put_progress(
                        progress_input(
                            library.revision(),
                            watched_time,
                            watched_time as f64,
                            completed,
                        ),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .expect("put progress");
                assert_eq!(update.revision, library.revision());
                assert_eq!(update.watched_time, watched_time);
                assert_eq!(update.completed, completed);
            }

            let page = library
                .activity_day_page(ActivityPageInput {
                    expected_revision: library.revision(),
                    lookback_days: 7,
                    offset: 0,
                    limit: 10,
                })
                .await
                .expect("load activity");
            assert_eq!(page.revision, library.revision());
            assert_eq!(page.total, 1);
            assert_eq!(page.rows.len(), 1);
            assert_eq!(page.rows[0].watched_seconds, 15);
            assert_eq!(page.rows[0].lessons_touched, 1);
            assert_eq!(page.rows[0].completions, 1);
            assert_eq!(activity_count(&mut library).await, 2);

            let lesson = sqlx::query(
                "SELECT watched_time, last_position, completed FROM lessons WHERE id = 'lesson'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load persisted progress");
            assert_eq!(lesson.get::<i64, _>("watched_time"), 12);
            assert_eq!(lesson.get::<f64, _>("last_position"), 12.0);
            assert!(lesson.get::<bool, _>("completed"));

            assert!(matches!(
                library
                    .put_progress(
                        progress_input(initial_revision, 20, 20.0, true),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::StaleRevision { .. })
            ));
            assert_eq!(activity_count(&mut library).await, 2);
        });
    }

    #[test]
    fn clearing_completion_records_an_activity_without_a_completion() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            for completed in [true, false] {
                library
                    .put_progress(
                        progress_input(library.revision(), 0, 0.0, completed),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .expect("change completion");
            }

            assert_eq!(activity_count(&mut library).await, 2);
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT SUM(completed) FROM lesson_activity")
                    .fetch_one(&mut library.connection)
                    .await
                    .expect("sum completions"),
                1
            );
        });
    }

    #[test]
    fn rejected_progress_leaves_the_lesson_and_revision_unchanged() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            let revision = library.revision();
            let cancelled = MutationControl::new();
            assert!(cancelled.cancel());

            assert!(
                library
                    .put_progress(
                        ProgressInput {
                            last_position: f64::NAN,
                            ..progress_input(revision, 1, 1.0, false)
                        },
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .put_progress(
                        ProgressInput {
                            lesson_id: String::new(),
                            ..progress_input(revision, 1, 1.0, false)
                        },
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .put_progress(
                        progress_input(revision, i64::MAX as u64 + 1, 1.0, false),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .put_progress(
                        ProgressInput {
                            lesson_id: "missing".to_string(),
                            ..progress_input(revision, 1, 1.0, false)
                        },
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .put_progress(
                        progress_input(revision, 1, 1.0, false),
                        usize::MAX,
                        &cancelled,
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .put_progress(
                        progress_input(revision, 1, 1.0, false),
                        1,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );

            assert_eq!(library.revision(), revision);
            assert_eq!(activity_count(&mut library).await, 0);
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT watched_time FROM lessons WHERE id = 'lesson'"
                )
                .fetch_one(&mut library.connection)
                .await
                .expect("load unchanged progress"),
                0
            );
        });
    }

    #[test]
    fn activity_insert_failure_rolls_back_progress() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            sqlx::raw_sql(
                "CREATE TRIGGER reject_activity
                 BEFORE INSERT ON lesson_activity
                 BEGIN
                     SELECT RAISE(ABORT, 'activity rejected');
                 END;",
            )
            .execute(&mut library.connection)
            .await
            .expect("install failing activity trigger");
            let revision = library.revision();

            assert!(matches!(
                library
                    .put_progress(
                        progress_input(revision, 10, 10.0, false),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::Database(_))
            ));
            assert_eq!(library.revision(), revision);
            assert_eq!(activity_count(&mut library).await, 0);
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT watched_time FROM lessons WHERE id = 'lesson'"
                )
                .fetch_one(&mut library.connection)
                .await
                .expect("load rolled back progress"),
                0
            );
        });
    }

    #[test]
    fn activity_pages_are_stable_and_validate_bounds() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            sqlx::raw_sql(
                "INSERT INTO lesson_activity (
                     id, course_id, lesson_id, activity_date, watched_seconds, completed
                 ) VALUES
                     ('day-2', 'course', 'lesson', date('now', '-2 days'), 2, 0),
                     ('day-1a', 'course', 'lesson', date('now', '-1 day'), 3, 0),
                     ('day-1b', 'course', 'lesson', date('now', '-1 day'), 4, 1),
                     ('day-0', 'course', 'lesson', date('now'), 5, 0);",
            )
            .execute(&mut library.connection)
            .await
            .expect("seed activity days");
            let revision = library.revision();

            let page = library
                .activity_day_page(ActivityPageInput {
                    expected_revision: revision,
                    lookback_days: 7,
                    offset: 1,
                    limit: 2,
                })
                .await
                .expect("load activity page");
            assert_eq!(page.total, 3);
            assert_eq!(page.offset, 1);
            assert_eq!(page.rows.len(), 2);
            assert_eq!(page.rows[0].watched_seconds, 7);
            assert_eq!(page.rows[0].completions, 1);
            assert_eq!(page.rows[1].watched_seconds, 5);

            for input in [
                ActivityPageInput {
                    expected_revision: revision + 1,
                    lookback_days: 7,
                    offset: 0,
                    limit: 1,
                },
                ActivityPageInput {
                    expected_revision: revision,
                    lookback_days: 0,
                    offset: 0,
                    limit: 1,
                },
                ActivityPageInput {
                    expected_revision: revision,
                    lookback_days: 7,
                    offset: 0,
                    limit: 0,
                },
            ] {
                assert!(library.activity_day_page(input).await.is_err());
            }
        });
    }

    #[test]
    fn activity_lookback_includes_exactly_the_requested_dates() {
        block_on(async {
            let (_temp, mut library) = progress_fixture().await;
            sqlx::raw_sql(
                "WITH RECURSIVE days(value) AS (
                     SELECT 0
                     UNION ALL
                     SELECT value + 1 FROM days WHERE value < 84
                 )
                 INSERT INTO lesson_activity (
                     id, course_id, lesson_id, activity_date, watched_seconds, completed
                 )
                 SELECT
                     'dense-' || value,
                     'course',
                     'lesson',
                     date('now', '-' || value || ' days'),
                     1,
                     0
                 FROM days;",
            )
            .execute(&mut library.connection)
            .await
            .expect("seed dense activity days");

            let today: String = sqlx::query_scalar("SELECT date('now')")
                .fetch_one(&mut library.connection)
                .await
                .expect("load current date");
            let page = library
                .activity_day_page(ActivityPageInput {
                    expected_revision: library.revision(),
                    lookback_days: 84,
                    offset: 0,
                    limit: 84,
                })
                .await
                .expect("load dense activity page");

            assert_eq!(page.total, 84);
            assert_eq!(page.rows.len(), 84);
            assert_eq!(
                page.rows.last().map(|day| day.date.as_str()),
                Some(today.as_str())
            );
        });
    }
}
