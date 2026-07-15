use serde::Serialize;
use sqlx::sqlite::SqliteRow;
use sqlx::{Connection, Row, Sqlite, Transaction};

use super::{LibraryDatabase, LibraryError, MAX_COURSE_PAGE_SIZE};
use crate::{MutationControl, next_library_revision};

const MAX_NOTE_UTF16_UNITS: usize = 2_000;

#[derive(Debug)]
pub(crate) struct NotePageInput {
    pub(crate) expected_revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) offset: u64,
    pub(crate) limit: u32,
}

#[derive(Debug)]
pub(crate) struct NoteSaveInput {
    pub(crate) expected_revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) note_id: Option<String>,
    pub(crate) timestamp: f64,
    pub(crate) text: String,
}

#[derive(Debug)]
pub(crate) struct NoteDeleteInput {
    pub(crate) expected_revision: u64,
    pub(crate) note_id: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct Note {
    pub(crate) id: String,
    pub(crate) lesson_id: String,
    pub(crate) timestamp: f64,
    pub(crate) text: String,
    pub(crate) created_at: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct NotePage {
    pub(crate) revision: u64,
    pub(crate) lesson_id: String,
    pub(crate) offset: u64,
    pub(crate) total: u64,
    pub(crate) rows: Vec<Note>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct NoteSaved {
    pub(crate) revision: u64,
    #[serde(flatten)]
    pub(crate) note: Note,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct NoteDelete {
    pub(crate) revision: u64,
    pub(crate) note_id: String,
}

impl LibraryDatabase {
    pub(crate) async fn note_page(
        &mut self,
        input: NotePageInput,
    ) -> Result<NotePage, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.lesson_id.is_empty() || input.lesson_id.contains('\0') {
            return Err(LibraryError::InvalidNote);
        }
        if !(1..=MAX_COURSE_PAGE_SIZE).contains(&input.limit) {
            return Err(LibraryError::InvalidPageSize { limit: input.limit });
        }
        let offset = i64::try_from(input.offset).map_err(|_| LibraryError::InvalidOffset {
            offset: input.offset,
        })?;
        let mut transaction = self.connection.begin().await?;
        let total: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM notes WHERE lesson_id = ?1")
            .bind(&input.lesson_id)
            .fetch_one(&mut *transaction)
            .await?;
        let records = sqlx::query(
            "SELECT id, lesson_id, timestamp, text, created_at
             FROM notes
             WHERE lesson_id = ?1
             ORDER BY timestamp ASC, created_at ASC, id ASC
             LIMIT ?2 OFFSET ?3",
        )
        .bind(&input.lesson_id)
        .bind(i64::from(input.limit))
        .bind(offset)
        .fetch_all(&mut *transaction)
        .await?;
        transaction.commit().await?;

        let rows = records
            .iter()
            .map(note_from_row)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(NotePage {
            revision: self.revision,
            lesson_id: input.lesson_id,
            offset: input.offset,
            total: nonnegative(total)?,
            rows,
        })
    }

    pub(crate) async fn save_note(
        &mut self,
        input: NoteSaveInput,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<NoteSaved, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.lesson_id.is_empty()
            || input.lesson_id.contains('\0')
            || input
                .note_id
                .as_deref()
                .is_some_and(|note_id| note_id.is_empty() || note_id.contains('\0'))
            || !input.timestamp.is_finite()
            || input.timestamp < 0.0
        {
            return Err(LibraryError::InvalidNote);
        }
        let text = validate_note_text(&input.text)?;
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        let lesson_exists: bool =
            sqlx::query_scalar("SELECT EXISTS(SELECT 1 FROM lessons WHERE id = ?1)")
                .bind(&input.lesson_id)
                .fetch_one(&mut *transaction)
                .await?;
        if !lesson_exists {
            return Err(rollback_error(transaction, LibraryError::LessonNotFound).await);
        }

        let record = if let Some(note_id) = input.note_id.as_deref() {
            match sqlx::query(
                "UPDATE notes
                 SET timestamp = ?1, text = ?2
                 WHERE id = ?3 AND lesson_id = ?4
                 RETURNING id, lesson_id, timestamp, text, created_at",
            )
            .bind(input.timestamp)
            .bind(&text)
            .bind(note_id)
            .bind(&input.lesson_id)
            .fetch_optional(&mut *transaction)
            .await
            {
                Ok(Some(record)) => record,
                Ok(None) => {
                    return Err(rollback_error(transaction, LibraryError::NoteNotFound).await);
                }
                Err(error) => {
                    return Err(rollback_error(transaction, LibraryError::from(error)).await);
                }
            }
        } else {
            match sqlx::query(
                "INSERT INTO notes (id, lesson_id, timestamp, text, created_at)
                 VALUES (
                     lower(hex(randomblob(16))), ?1, ?2, ?3,
                     strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
                 )
                 RETURNING id, lesson_id, timestamp, text, created_at",
            )
            .bind(&input.lesson_id)
            .bind(input.timestamp)
            .bind(&text)
            .fetch_one(&mut *transaction)
            .await
            {
                Ok(record) => record,
                Err(error) => {
                    return Err(rollback_error(transaction, LibraryError::from(error)).await);
                }
            }
        };
        let note = match note_from_row(&record) {
            Ok(note) => note,
            Err(error) => return Err(rollback_error(transaction, error).await),
        };
        let Some(next_revision) = next_library_revision() else {
            return Err(rollback_error(transaction, LibraryError::RevisionExhausted).await);
        };
        let saved = NoteSaved {
            revision: next_revision.get(),
            note,
        };
        if !response_fits(&saved, max_payload_bytes) {
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
        Ok(saved)
    }

    pub(crate) async fn delete_note(
        &mut self,
        input: NoteDeleteInput,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<NoteDelete, LibraryError> {
        self.require_revision(input.expected_revision)?;
        if input.note_id.is_empty() || input.note_id.contains('\0') {
            return Err(LibraryError::InvalidNote);
        }
        if control.is_cancelled() {
            return Err(LibraryError::Cancelled);
        }

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        let deleted = match sqlx::query("DELETE FROM notes WHERE id = ?1")
            .bind(&input.note_id)
            .execute(&mut *transaction)
            .await
        {
            Ok(result) => result.rows_affected() != 0,
            Err(error) => {
                return Err(rollback_error(transaction, LibraryError::from(error)).await);
            }
        };
        if !deleted {
            transaction.rollback().await?;
            let result = NoteDelete {
                revision: self.revision,
                note_id: input.note_id,
            };
            if !response_fits(&result, max_payload_bytes) {
                return Err(LibraryError::ResponseTooLarge {
                    limit: max_payload_bytes,
                });
            }
            if control.is_cancelled() {
                return Err(LibraryError::Cancelled);
            }
            return Ok(result);
        }

        let Some(next_revision) = next_library_revision() else {
            return Err(rollback_error(transaction, LibraryError::RevisionExhausted).await);
        };
        let result = NoteDelete {
            revision: next_revision.get(),
            note_id: input.note_id,
        };
        if !response_fits(&result, max_payload_bytes) {
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
        Ok(result)
    }
}

fn validate_note_text(text: &str) -> Result<String, LibraryError> {
    let text = text.trim_matches(is_ecmascript_whitespace);
    let length = text.encode_utf16().count();
    if !(1..=MAX_NOTE_UTF16_UNITS).contains(&length) {
        return Err(LibraryError::InvalidNote);
    }
    Ok(text.to_string())
}

fn is_ecmascript_whitespace(character: char) -> bool {
    matches!(
        character,
        '\u{0009}'..='\u{000D}'
            | '\u{0020}'
            | '\u{00A0}'
            | '\u{1680}'
            | '\u{2000}'..='\u{200A}'
            | '\u{2028}'
            | '\u{2029}'
            | '\u{202F}'
            | '\u{205F}'
            | '\u{3000}'
            | '\u{FEFF}'
    )
}

fn note_from_row(row: &SqliteRow) -> Result<Note, LibraryError> {
    Ok(Note {
        id: row.try_get("id")?,
        lesson_id: row.try_get("lesson_id")?,
        timestamp: row.try_get("timestamp")?,
        text: row.try_get("text")?,
        created_at: row.try_get("created_at")?,
    })
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
    u64::try_from(value).map_err(|_| LibraryError::Database("negative note count".to_string()))
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use sqlx::Row;

    use super::{NoteDeleteInput, NotePageInput, NoteSaveInput, validate_note_text};
    use crate::library::{LibraryDatabase, LibraryError};
    use crate::{MutationControl, next_library_revision};

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build notes test runtime")
            .block_on(future)
    }

    async fn notes_fixture() -> (tempfile::TempDir, LibraryDatabase) {
        let temp = tempfile::tempdir().expect("create notes fixture");
        let mut library = LibraryDatabase::open_current(
            temp.path(),
            next_library_revision().expect("allocate notes fixture revision"),
            Arc::new(AtomicBool::new(false)),
        )
        .await
        .expect("open notes fixture");
        sqlx::raw_sql(
            "INSERT INTO courses (
                 id, identity_id, name, path, fingerprint, last_scanned_at
             ) VALUES
                 ('course', 'identity', 'Course', '/library/Course', 'fingerprint',
                  '2026-07-15T00:00:00.000Z');
             INSERT INTO sections (id, course_id, name, order_index) VALUES
                 ('section', 'course', 'Section', 0);
             INSERT INTO lessons (
                 id, course_id, section_id, name, path, relative_path, type,
                 duration, watched_time, completed, order_index, last_position
             ) VALUES
                 ('lesson', 'course', 'section', 'Lesson',
                  '/library/Course/Section/lesson.mp4', 'Section/lesson.mp4', 'video',
                  120, 0, 0, 0, 0),
                 ('other-lesson', 'course', 'section', 'Other Lesson',
                  '/library/Course/Section/other.mp4', 'Section/other.mp4', 'video',
                  120, 0, 0, 1, 0);
             INSERT INTO notes (id, lesson_id, timestamp, text, created_at) VALUES
                 ('note-b', 'lesson', 10, 'second tie', '2026-07-15T00:00:00.000Z'),
                 ('note-a', 'lesson', 10, 'first tie', '2026-07-15T00:00:00.000Z');",
        )
        .execute(&mut library.connection)
        .await
        .expect("seed notes fixture");
        (temp, library)
    }

    fn page_input(revision: u64, lesson_id: &str, offset: u64, limit: u32) -> NotePageInput {
        NotePageInput {
            expected_revision: revision,
            lesson_id: lesson_id.to_string(),
            offset,
            limit,
        }
    }

    fn save_input(
        revision: u64,
        lesson_id: &str,
        note_id: Option<&str>,
        timestamp: f64,
        text: &str,
    ) -> NoteSaveInput {
        NoteSaveInput {
            expected_revision: revision,
            lesson_id: lesson_id.to_string(),
            note_id: note_id.map(str::to_string),
            timestamp,
            text: text.to_string(),
        }
    }

    async fn note_count(library: &mut LibraryDatabase) -> i64 {
        sqlx::query_scalar("SELECT COUNT(*) FROM notes")
            .fetch_one(&mut library.connection)
            .await
            .expect("count notes")
    }

    #[test]
    fn note_pages_are_stable_and_validate_bounds() {
        block_on(async {
            let (_temp, mut library) = notes_fixture().await;
            let revision = library.revision();
            let first = library
                .note_page(page_input(revision, "lesson", 0, 1))
                .await
                .expect("load first note page");
            let second = library
                .note_page(page_input(revision, "lesson", 1, 1))
                .await
                .expect("load second note page");
            assert_eq!(first.revision, revision);
            assert_eq!(first.lesson_id, "lesson");
            assert_eq!(first.total, 2);
            assert_eq!(first.rows[0].id, "note-a");
            assert_eq!(second.rows[0].id, "note-b");

            let missing = library
                .note_page(page_input(revision, "missing", 0, 10))
                .await
                .expect("missing Lesson has no notes");
            assert_eq!(missing.total, 0);
            assert!(missing.rows.is_empty());

            for input in [
                page_input(revision + 1, "lesson", 0, 1),
                page_input(revision, "lesson", 0, 0),
                page_input(revision, "lesson", 0, 201),
                page_input(revision, "lesson", i64::MAX as u64 + 1, 1),
                page_input(revision, "", 0, 1),
            ] {
                assert!(library.note_page(input).await.is_err());
            }
        });
    }

    #[test]
    fn note_save_creates_and_updates_without_moving_identity() {
        block_on(async {
            let (temp, mut library) = notes_fixture().await;
            let initial_revision = library.revision();
            let created = library
                .save_note(
                    save_input(initial_revision, "lesson", None, 42.5, "  復習する  "),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("create note");
            assert!(created.revision > initial_revision);
            assert_eq!(created.note.id.len(), 32);
            assert!(created.note.id.bytes().all(|byte| byte.is_ascii_hexdigit()));
            assert_eq!(created.note.lesson_id, "lesson");
            assert_eq!(created.note.text, "復習する");
            assert!(created.note.created_at.contains('T'));
            assert!(created.note.created_at.ends_with('Z'));
            let created_at = created.note.created_at.clone();

            assert!(matches!(
                library
                    .note_page(page_input(initial_revision, "lesson", 0, 10))
                    .await,
                Err(LibraryError::StaleRevision { .. })
            ));

            let updated = library
                .save_note(
                    save_input(
                        created.revision,
                        "lesson",
                        Some(&created.note.id),
                        5.0,
                        "changed",
                    ),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("update note");
            assert!(updated.revision > created.revision);
            assert_eq!(updated.note.id, created.note.id);
            assert_eq!(updated.note.created_at, created_at);
            assert_eq!(updated.note.timestamp, 5.0);
            assert_eq!(updated.note.text, "changed");
            assert_eq!(note_count(&mut library).await, 3);

            let page = library
                .note_page(page_input(updated.revision, "lesson", 0, 10))
                .await
                .expect("load updated notes");
            assert_eq!(page.rows[0].id, updated.note.id);

            drop(library);
            let mut reopened = LibraryDatabase::open_current(
                temp.path(),
                next_library_revision().expect("allocate reopened notes revision"),
                Arc::new(AtomicBool::new(false)),
            )
            .await
            .expect("reopen notes fixture");
            let reopened_revision = reopened.revision();
            let persisted = reopened
                .note_page(page_input(reopened_revision, "lesson", 0, 10))
                .await
                .expect("load persisted notes");
            assert_eq!(persisted.rows[0].id, updated.note.id);
            assert_eq!(persisted.rows[0].created_at, created_at);
            assert_eq!(persisted.rows[0].text, "changed");
        });
    }

    #[test]
    fn delete_is_atomic_and_missing_notes_are_a_noop() {
        block_on(async {
            let (_temp, mut library) = notes_fixture().await;
            let initial_revision = library.revision();
            let deleted = library
                .delete_note(
                    NoteDeleteInput {
                        expected_revision: initial_revision,
                        note_id: "note-a".to_string(),
                    },
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("delete note");
            assert!(deleted.revision > initial_revision);
            assert_eq!(deleted.note_id, "note-a");
            assert_eq!(note_count(&mut library).await, 1);

            let missing = library
                .delete_note(
                    NoteDeleteInput {
                        expected_revision: deleted.revision,
                        note_id: "missing".to_string(),
                    },
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("missing note delete is idempotent");
            assert_eq!(missing.revision, deleted.revision);
            assert_eq!(note_count(&mut library).await, 1);
        });
    }

    #[test]
    fn note_validation_matches_the_current_utf16_contract() {
        assert_eq!(validate_note_text("  note  ").unwrap(), "note");
        assert_eq!(validate_note_text("\u{FEFF}note\u{FEFF}").unwrap(), "note");
        assert_eq!(validate_note_text("\0").unwrap(), "\0");
        assert_eq!(validate_note_text("\u{0085}").unwrap(), "\u{0085}");
        assert!(validate_note_text("").is_err());
        assert!(validate_note_text("   ").is_err());
        assert!(validate_note_text("\u{FEFF}").is_err());
        assert!(validate_note_text(&"x".repeat(2_001)).is_err());
        assert!(validate_note_text(&"😀".repeat(1_000)).is_ok());
        assert!(validate_note_text(&"😀".repeat(1_001)).is_err());
    }

    #[test]
    fn rejected_note_mutations_leave_rows_and_revision_unchanged() {
        block_on(async {
            let (_temp, mut library) = notes_fixture().await;
            let revision = library.revision();
            let cancelled = MutationControl::new();
            assert!(cancelled.cancel());

            for input in [
                save_input(revision + 1, "lesson", None, 1.0, "note"),
                save_input(revision, "missing", None, 1.0, "note"),
                save_input(revision, "lesson", None, -1.0, "note"),
                save_input(revision, "lesson", None, f64::NAN, "note"),
                save_input(revision, "lesson", None, f64::INFINITY, "note"),
                save_input(revision, "lesson", None, 1.0, " "),
                save_input(revision, "lesson", Some("missing"), 1.0, "note"),
                save_input(revision, "other-lesson", Some("note-a"), 1.0, "note"),
            ] {
                assert!(
                    library
                        .save_note(input, usize::MAX, &MutationControl::new())
                        .await
                        .is_err()
                );
            }
            assert!(
                library
                    .save_note(
                        save_input(revision, "lesson", None, 1.0, "note"),
                        usize::MAX,
                        &cancelled,
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .save_note(
                        save_input(revision, "lesson", None, 1.0, "note"),
                        1,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .delete_note(
                        NoteDeleteInput {
                            expected_revision: revision,
                            note_id: "note-a".to_string(),
                        },
                        usize::MAX,
                        &cancelled,
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .delete_note(
                        NoteDeleteInput {
                            expected_revision: revision,
                            note_id: "note-a".to_string(),
                        },
                        1,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );

            assert_eq!(library.revision(), revision);
            assert_eq!(note_count(&mut library).await, 2);
        });
    }

    #[test]
    fn database_failures_roll_back_note_mutations() {
        block_on(async {
            let (_temp, mut library) = notes_fixture().await;
            sqlx::raw_sql(
                "CREATE TRIGGER reject_note_insert
                 BEFORE INSERT ON notes
                 BEGIN
                     SELECT RAISE(ABORT, 'note insert rejected');
                 END;
                 CREATE TRIGGER reject_note_delete
                 BEFORE DELETE ON notes
                 BEGIN
                     SELECT RAISE(ABORT, 'note delete rejected');
                 END;",
            )
            .execute(&mut library.connection)
            .await
            .expect("install failing note triggers");
            let revision = library.revision();

            assert!(
                library
                    .save_note(
                        save_input(revision, "lesson", None, 1.0, "note"),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert!(
                library
                    .delete_note(
                        NoteDeleteInput {
                            expected_revision: revision,
                            note_id: "note-a".to_string(),
                        },
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await
                    .is_err()
            );
            assert_eq!(library.revision(), revision);
            assert_eq!(note_count(&mut library).await, 2);

            let row = sqlx::query("SELECT text FROM notes WHERE id = 'note-a'")
                .fetch_one(&mut library.connection)
                .await
                .expect("load unchanged note");
            assert_eq!(row.get::<String, _>("text"), "first tie");
        });
    }
}
