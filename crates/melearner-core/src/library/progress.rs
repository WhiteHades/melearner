use serde::Serialize;
use sqlx::{Connection, Row, Sqlite, Transaction};

use super::{LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE};
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

fn response_fits(update: &ProgressUpdate, max_payload_bytes: usize) -> bool {
    serde_json::to_vec(update).is_ok_and(|payload| payload.len() <= max_payload_bytes)
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

    use super::{ActivityPageInput, ProgressInput};
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
            "INSERT INTO courses (
                 id, identity_id, name, path, fingerprint, last_scanned_at
             ) VALUES (
                 'course', 'identity', 'Course', '/library/Course', 'fingerprint',
                 '2026-07-15T00:00:00.000Z'
             );
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
        (temp, library)
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
