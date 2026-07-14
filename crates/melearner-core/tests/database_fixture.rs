use std::borrow::Cow;
use std::fs;
use std::future::Future;
use std::path::{Path, PathBuf};
use std::time::Duration;

use melearner_core::migrations::{MIGRATIONS, MigrationDefinition};
use sha2::{Digest, Sha384};
use sqlx::migrate::{Migration, MigrationType, Migrator};
use sqlx::sqlite::{SqliteConnectOptions, SqliteJournalMode};
use sqlx::{Connection, Row, SqliteConnection};

const FIXTURE_DATA: &str = include_str!("../../../fixtures/parity/database-v16.sql");
const FIXTURE_VERSION: i64 = 16;
const DOMAIN_TABLES: &[&str] = &[
    "app_settings",
    "courses",
    "sections",
    "lessons",
    "lesson_subtitles",
    "notes",
    "lesson_activity",
];

fn block_on<T>(future: impl Future<Output = T>) -> T {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .expect("build fixture runtime")
        .block_on(future)
}

fn fixture_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../fixtures/parity")
        .canonicalize()
        .expect("canonical fixture root")
}

fn checked_fixture() -> PathBuf {
    fixture_root().join("database-v16.sqlite")
}

fn sidecars(path: &Path) -> [PathBuf; 3] {
    let path = path.as_os_str().to_string_lossy();
    [
        PathBuf::from(format!("{path}-wal")),
        PathBuf::from(format!("{path}-shm")),
        PathBuf::from(format!("{path}-journal")),
    ]
}

fn fixture_migrations() -> impl Iterator<Item = &'static MigrationDefinition> {
    MIGRATIONS
        .iter()
        .filter(|migration| migration.version <= FIXTURE_VERSION)
}

fn migrator_through(version: i64) -> Migrator {
    Migrator {
        migrations: Cow::Owned(
            MIGRATIONS
                .iter()
                .filter(|migration| migration.version <= version)
                .map(|migration| {
                    Migration::new(
                        migration.version,
                        migration.description.into(),
                        MigrationType::Simple,
                        migration.sql.into(),
                        false,
                    )
                })
                .collect(),
        ),
        ignore_missing: false,
        locking: true,
        no_tx: false,
    }
}

fn fixture_migrator() -> Migrator {
    migrator_through(FIXTURE_VERSION)
}

async fn connect(path: &Path, create: bool) -> SqliteConnection {
    let options = SqliteConnectOptions::new()
        .filename(path)
        .create_if_missing(create)
        .foreign_keys(true)
        .journal_mode(SqliteJournalMode::Wal)
        .busy_timeout(Duration::from_secs(10));
    SqliteConnection::connect_with(&options)
        .await
        .expect("open fixture database")
}

async fn generate_database(path: &Path) {
    assert!(!path.exists(), "fixture output must not already exist");
    let mut connection = connect(path, true).await;
    fixture_migrator()
        .run(&mut connection)
        .await
        .expect("apply fixture migrations");
    sqlx::raw_sql(FIXTURE_DATA)
        .execute(&mut connection)
        .await
        .expect("insert fixture rows");
    sqlx::query(
        "UPDATE _sqlx_migrations
         SET installed_on = '2026-07-09 12:00:00', execution_time = 0",
    )
    .execute(&mut connection)
    .await
    .expect("normalize migration ledger");
    verify_database(&mut connection).await;
    sqlx::raw_sql("VACUUM; PRAGMA wal_checkpoint(TRUNCATE);")
        .execute(&mut connection)
        .await
        .expect("compact fixture database");
    connection.close().await.expect("close fixture database");
    assert_no_sidecars(path);
}

fn assert_no_sidecars(path: &Path) {
    for sidecar in sidecars(path) {
        assert!(
            !sidecar.exists(),
            "unexpected SQLite sidecar: {}",
            sidecar.display()
        );
    }
}

async fn verify_database(connection: &mut SqliteConnection) {
    let foreign_keys: i64 = sqlx::query_scalar("PRAGMA foreign_keys")
        .fetch_one(&mut *connection)
        .await
        .expect("read foreign key mode");
    assert_eq!(foreign_keys, 1);
    let journal_mode: String = sqlx::query_scalar("PRAGMA journal_mode")
        .fetch_one(&mut *connection)
        .await
        .expect("read journal mode");
    assert_eq!(journal_mode, "wal");
    let busy_timeout: i64 = sqlx::query_scalar("PRAGMA busy_timeout")
        .fetch_one(&mut *connection)
        .await
        .expect("read busy timeout");
    assert_eq!(busy_timeout, 10_000);

    let integrity: String = sqlx::query_scalar("PRAGMA integrity_check")
        .fetch_one(&mut *connection)
        .await
        .expect("run integrity check");
    assert_eq!(integrity, "ok");

    let foreign_key_errors = sqlx::query("PRAGMA foreign_key_check")
        .fetch_all(&mut *connection)
        .await
        .expect("run foreign key check");
    assert!(foreign_key_errors.is_empty());

    let ledger = sqlx::query(
        "SELECT version, description, success, checksum,
                CAST(installed_on AS TEXT) AS installed_on, execution_time
         FROM _sqlx_migrations ORDER BY version",
    )
    .fetch_all(&mut *connection)
    .await
    .expect("read migration ledger");
    assert_eq!(
        fixture_migrations()
            .last()
            .map(|migration| migration.version),
        Some(FIXTURE_VERSION)
    );
    assert_eq!(ledger.len(), fixture_migrations().count());
    for (row, migration) in ledger.iter().zip(fixture_migrations()) {
        assert_eq!(row.get::<i64, _>("version"), migration.version);
        assert_eq!(row.get::<String, _>("description"), migration.description);
        assert!(row.get::<bool, _>("success"));
        assert_eq!(
            row.get::<Vec<u8>, _>("checksum"),
            Sha384::digest(migration.sql).to_vec()
        );
        assert_eq!(row.get::<String, _>("installed_on"), "2026-07-09 12:00:00");
        assert_eq!(row.get::<i64, _>("execution_time"), 0);
    }

    let library_path: String =
        sqlx::query_scalar("SELECT value FROM app_settings WHERE key = 'libraryPath'")
            .fetch_one(&mut *connection)
            .await
            .expect("read fixture library path");
    assert_eq!(library_path, "/fixtures/library");

    let paths: Vec<String> = sqlx::query_scalar(
        "SELECT path FROM courses
         UNION ALL SELECT path FROM lessons
         UNION ALL SELECT path FROM lesson_subtitles",
    )
    .fetch_all(&mut *connection)
    .await
    .expect("read fixture paths");
    assert!(paths.iter().all(|path| path.starts_with("/fixtures/")));
    assert!(paths.iter().any(|path| path.len() > 260));
    assert!(paths.iter().any(|path| path.contains("Systems 日本語")));
    for path in paths {
        assert!(!path.contains("/home/"));
        assert!(!path.contains("/Users/"));
        assert!(!path.contains("\\Users\\"));
    }
}

fn assert_no_private_paths(path: &Path) {
    let bytes = fs::read(path).expect("read checked database fixture");
    for private_prefix in [b"/home/".as_slice(), b"/Users/", b"\\Users\\"] {
        assert!(
            !bytes
                .windows(private_prefix.len())
                .any(|window| window == private_prefix),
            "fixture contains a private path prefix"
        );
    }
}

async fn logical_snapshot(path: &Path) -> Vec<String> {
    let mut connection = connect(path, false).await;
    verify_database(&mut connection).await;

    let mut snapshot = Vec::new();
    let schema = sqlx::query(
        "SELECT type, name, tbl_name, COALESCE(sql, '') AS sql
         FROM sqlite_schema
         WHERE name NOT LIKE 'sqlite_%'
         ORDER BY type, name",
    )
    .fetch_all(&mut connection)
    .await
    .expect("read fixture schema");
    for row in schema {
        snapshot.push(format!(
            "schema|{}|{}|{}|{}",
            row.get::<String, _>("type"),
            row.get::<String, _>("name"),
            row.get::<String, _>("tbl_name"),
            row.get::<String, _>("sql")
        ));
    }

    for table in std::iter::once("_sqlx_migrations").chain(DOMAIN_TABLES.iter().copied()) {
        snapshot.extend(table_snapshot(&mut connection, table).await);
    }
    connection.close().await.expect("close snapshot database");
    snapshot
}

async fn table_snapshot(connection: &mut SqliteConnection, table: &str) -> Vec<String> {
    let pragma = format!("PRAGMA table_info(\"{}\")", table.replace('"', "\"\""));
    let columns = sqlx::query(&pragma)
        .fetch_all(&mut *connection)
        .await
        .expect("read fixture table columns");
    let names: Vec<String> = columns
        .iter()
        .map(|row| row.get::<String, _>("name"))
        .collect();
    let mut primary_key: Vec<(i64, String)> = columns
        .iter()
        .filter_map(|row| {
            let order = row.get::<i64, _>("pk");
            (order != 0).then(|| (order, row.get::<String, _>("name")))
        })
        .collect();
    primary_key.sort_by_key(|(order, _)| *order);

    let quote = |name: &str| format!("\"{}\"", name.replace('"', "\"\""));
    let values = names
        .iter()
        .map(|name| format!("quote({})", quote(name)))
        .collect::<Vec<_>>()
        .join(" || char(31) || ");
    let order = if primary_key.is_empty() {
        names.iter().map(|name| quote(name)).collect::<Vec<_>>()
    } else {
        primary_key.iter().map(|(_, name)| quote(name)).collect()
    }
    .join(", ");
    let query = format!(
        "SELECT {values} AS row_value FROM {} ORDER BY {order}",
        quote(table)
    );
    sqlx::query_scalar::<_, String>(&query)
        .fetch_all(connection)
        .await
        .expect("read fixture table rows")
        .into_iter()
        .map(|row| format!("{table}|{row}"))
        .collect()
}

async fn seed_historical_database(connection: &mut SqliteConnection, version: i64) {
    let mut sql = String::from(
        "INSERT INTO courses
         (id, name, path, total_duration, watched_duration)
         VALUES ('course-sentinel', 'Legacy Course', '/fixtures/upgrade/course', 100, 10);",
    );
    if version >= 2 {
        sql.push_str(
            "INSERT INTO lessons
             (id, course_id, section_name, name, path, type, duration, watched_time,
              completed, order_index, last_position)
             VALUES (
               'lesson-sentinel', 'course-sentinel', 'Legacy Section', 'Legacy Lesson',
               '/fixtures/upgrade/course/Legacy Section/lesson.mp4', 'video',
               90, 9, 1, 3, 12.5
             );",
        );
    }
    if version >= 3 {
        sql.push_str(
            "INSERT INTO notes (id, lesson_id, timestamp, text)
             VALUES ('note-sentinel', 'lesson-sentinel', 12.5, 'legacy note');",
        );
    }
    if (4..7).contains(&version) {
        sql.push_str(
            "INSERT INTO bookmarks (id, lesson_id, timestamp, label)
             VALUES ('bookmark-sentinel', 'lesson-sentinel', 12.5, 'legacy bookmark');",
        );
    }
    if (5..7).contains(&version) {
        sql.push_str("INSERT INTO settings (key, value) VALUES ('legacy', 'setting');");
    }
    if version >= 11 {
        sql.push_str(
            "INSERT INTO sections (id, course_id, name, order_index)
             VALUES ('section-sentinel', 'course-sentinel', 'Legacy Section', 3);
             INSERT INTO lesson_subtitles
             (id, lesson_id, path, language, label, order_index)
             VALUES (
               'subtitle-sentinel', 'lesson-sentinel',
               '/fixtures/upgrade/course/Legacy Section/lesson.srt',
               'en', 'English', 2
             );
             INSERT INTO app_settings (key, value)
             VALUES ('libraryPath', '/fixtures/upgrade');",
        );
    }
    if version >= 12 {
        sql.push_str(
            "UPDATE lessons
             SET section_id = 'section-sentinel', file_size = 4096
             WHERE id = 'lesson-sentinel';",
        );
    }
    if version >= 15 {
        sql.push_str(
            "UPDATE courses
             SET identity_id = 'identity-sentinel',
                 fingerprint = 'fingerprint-sentinel',
                 missing_since = '2026-07-02T00:00:00.000Z'
             WHERE id = 'course-sentinel';
             UPDATE lessons
             SET relative_path = 'Legacy Section/lesson.mp4'
             WHERE id = 'lesson-sentinel';",
        );
    }
    sqlx::raw_sql(&sql)
        .execute(connection)
        .await
        .expect("seed historical database");
}

async fn verify_historical_upgrade(connection: &mut SqliteConnection, version: i64) {
    assert_eq!(
        sqlx::query_scalar::<_, String>("PRAGMA integrity_check")
            .fetch_one(&mut *connection)
            .await
            .expect("check upgraded database integrity"),
        "ok"
    );
    assert!(
        sqlx::query("PRAGMA foreign_key_check")
            .fetch_all(&mut *connection)
            .await
            .expect("check upgraded foreign keys")
            .is_empty()
    );
    let ledger = sqlx::query(
        "SELECT version, description, success, checksum
         FROM _sqlx_migrations ORDER BY version",
    )
    .fetch_all(&mut *connection)
    .await
    .expect("read upgraded migration ledger");
    assert_eq!(ledger.len(), MIGRATIONS.len());
    for (row, migration) in ledger.iter().zip(MIGRATIONS) {
        assert_eq!(row.get::<i64, _>("version"), migration.version);
        assert_eq!(row.get::<String, _>("description"), migration.description);
        assert!(row.get::<bool, _>("success"));
        assert_eq!(
            row.get::<Vec<u8>, _>("checksum"),
            Sha384::digest(migration.sql).to_vec()
        );
    }

    let actual = sqlx::query_scalar::<_, String>(
        "SELECT value FROM (
             SELECT 'course|' || id || '|' || name || '|' || path || '|' ||
                    total_duration || '|' || watched_duration || '|' || identity_id || '|' ||
                    COALESCE(fingerprint, '') || '|' || COALESCE(missing_since, '') AS value
             FROM courses WHERE id = 'course-sentinel'
             UNION ALL
             SELECT 'lesson|' || id || '|' || course_id || '|' || section_id || '|' ||
                    section_name || '|' || name || '|' || path || '|' || type || '|' ||
                    duration || '|' || watched_time || '|' || completed || '|' ||
                    order_index || '|' || last_position || '|' || file_size || '|' ||
                    COALESCE(relative_path, '')
             FROM lessons WHERE id = 'lesson-sentinel'
             UNION ALL
             SELECT 'section|' || id || '|' || course_id || '|' || name || '|' || order_index
             FROM sections WHERE course_id = 'course-sentinel'
             UNION ALL
             SELECT 'note|' || id || '|' || lesson_id || '|' || timestamp || '|' || text
             FROM notes WHERE id = 'note-sentinel'
             UNION ALL
             SELECT 'subtitle|' || id || '|' || lesson_id || '|' || path || '|' ||
                    language || '|' || label || '|' || order_index
             FROM lesson_subtitles WHERE id = 'subtitle-sentinel'
             UNION ALL
             SELECT 'setting|' || key || '|' || value
             FROM app_settings WHERE key = 'libraryPath'
         ) ORDER BY value",
    )
    .fetch_all(&mut *connection)
    .await
    .expect("read upgraded sentinel data");

    let identity = if version >= 15 {
        "identity-sentinel|fingerprint-sentinel|2026-07-02T00:00:00.000Z"
    } else {
        "course-sentinel||"
    };
    let section_id = if version >= 11 {
        "section-sentinel"
    } else {
        "course-sentinel:section:4c65676163792053656374696f6e"
    };
    let mut expected = vec![format!(
        "course|course-sentinel|Legacy Course|/fixtures/upgrade/course|100|10|{identity}"
    )];
    if version >= 2 {
        expected.extend([
            format!(
                "lesson|lesson-sentinel|course-sentinel|{section_id}|Legacy Section|\
                 Legacy Lesson|/fixtures/upgrade/course/Legacy Section/lesson.mp4|video|\
                 90|9|1|3|12.5|{}|{}",
                if version >= 12 { 4096 } else { 0 },
                if version >= 15 {
                    "Legacy Section/lesson.mp4"
                } else {
                    ""
                }
            ),
            format!("section|{section_id}|course-sentinel|Legacy Section|3"),
        ]);
    }
    if version >= 3 {
        expected.push("note|note-sentinel|lesson-sentinel|12.5|legacy note".to_string());
    }
    if version >= 11 {
        expected.extend([
            "setting|libraryPath|/fixtures/upgrade".to_string(),
            "subtitle|subtitle-sentinel|lesson-sentinel|/fixtures/upgrade/course/Legacy Section/lesson.srt|en|English|2".to_string(),
        ]);
    }
    expected.sort();
    assert_eq!(actual, expected);

    assert_eq!(
        sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM lesson_activity")
            .fetch_one(&mut *connection)
            .await
            .expect("count upgraded activity rows"),
        0
    );
    for removed in ["bookmarks", "settings"] {
        assert_eq!(
            sqlx::query_scalar::<_, i64>(
                "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = ?1",
            )
            .bind(removed)
            .fetch_one(&mut *connection)
            .await
            .expect("check removed historical table"),
            0
        );
    }
}

#[test]
fn every_shipped_migration_prefix_preserves_supported_data() {
    block_on(async {
        let temp = tempfile::tempdir().expect("create historical migration tempdir");

        for version in 1..FIXTURE_VERSION {
            let source = temp.path().join(format!("database-v{version}.sqlite"));
            let mut historical = connect(&source, true).await;
            migrator_through(version)
                .run(&mut historical)
                .await
                .expect("create historical schema prefix");
            seed_historical_database(&mut historical, version).await;
            sqlx::raw_sql("PRAGMA wal_checkpoint(TRUNCATE)")
                .execute(&mut historical)
                .await
                .expect("checkpoint historical database");
            historical.close().await.expect("close historical database");
            assert_no_sidecars(&source);

            let upgraded = temp.path().join(format!("upgraded-from-v{version}.sqlite"));
            fs::copy(&source, &upgraded).expect("copy historical database for migration");
            let mut connection = connect(&upgraded, false).await;
            fixture_migrator()
                .run(&mut connection)
                .await
                .expect("upgrade historical database to v16");
            verify_historical_upgrade(&mut connection, version).await;
            sqlx::raw_sql("PRAGMA wal_checkpoint(TRUNCATE)")
                .execute(&mut connection)
                .await
                .expect("checkpoint upgraded database");
            connection.close().await.expect("close upgraded database");

            assert_no_sidecars(&upgraded);
        }
    });
}

#[test]
fn failed_migration_rolls_back_schema_and_data() {
    block_on(async {
        let temp = tempfile::tempdir().expect("create failed migration tempdir");
        let copied = temp.path().join("database-v16.sqlite");
        fs::copy(checked_fixture(), &copied).expect("copy v16 database fixture");
        let before = logical_snapshot(&copied).await;

        let mut migrations = fixture_migrator().migrations.into_owned();
        migrations.push(Migration::new(
            17,
            "deliberately_failing_migration".into(),
            MigrationType::Simple,
            "UPDATE courses SET name = 'corrupted' WHERE id = 'course-marker';
             CREATE TABLE failed_migration_sentinel (value TEXT NOT NULL);
             INSERT INTO deliberately_missing_table (value) VALUES ('fail');"
                .into(),
            false,
        ));
        let failing_migrator = Migrator {
            migrations: Cow::Owned(migrations),
            ignore_missing: false,
            locking: true,
            no_tx: false,
        };

        let mut connection = connect(&copied, false).await;
        let error = failing_migrator
            .run(&mut connection)
            .await
            .expect_err("deliberate migration must fail");
        assert!(error.to_string().contains("deliberately_missing_table"));
        sqlx::raw_sql("PRAGMA wal_checkpoint(TRUNCATE)")
            .execute(&mut connection)
            .await
            .expect("checkpoint failed migration database");
        connection
            .close()
            .await
            .expect("close failed migration database");

        assert_eq!(logical_snapshot(&copied).await, before);
        assert_no_sidecars(&copied);
    });
}

#[test]
fn checked_database_v16_matches_fresh_logical_fixture() {
    block_on(async {
        let checked = checked_fixture();
        assert!(checked.is_file(), "missing {}", checked.display());
        assert_no_sidecars(&checked);
        assert_no_private_paths(&checked);

        let temp = tempfile::tempdir().expect("create fixture tempdir");
        let copied = temp.path().join("copied-v16.sqlite");
        fs::copy(&checked, &copied).expect("copy checked database fixture");
        let fresh = temp.path().join("fresh-v16.sqlite");
        generate_database(&fresh).await;

        let copied_snapshot = logical_snapshot(&copied).await;
        assert_eq!(copied_snapshot, logical_snapshot(&fresh).await);

        let mut connection = connect(&copied, false).await;
        fixture_migrator()
            .run(&mut connection)
            .await
            .expect("reopen checked migration ledger");
        connection
            .close()
            .await
            .expect("close migrated fixture copy");
        assert_eq!(copied_snapshot, logical_snapshot(&copied).await);
        assert_no_sidecars(&copied);
        assert_no_sidecars(&fresh);
    });
}

#[test]
#[ignore = "explicitly regenerates the checked physical SQLite fixture"]
fn regenerate_database_v16_fixture() {
    assert_eq!(
        std::env::var("MELEARNER_REGENERATE_DATABASE_V16").as_deref(),
        Ok("1"),
        "set MELEARNER_REGENERATE_DATABASE_V16=1"
    );
    let root = fixture_root();
    let checked = checked_fixture();
    assert_eq!(checked.parent(), Some(root.as_path()));
    let temporary = root.join("database-v16.sqlite.tmp");
    if temporary.exists() {
        fs::remove_file(&temporary).expect("remove stale fixture temp file");
    }

    block_on(async {
        generate_database(&temporary).await;
        let temp = tempfile::tempdir().expect("create verification tempdir");
        let verification = temp.path().join("database-v16.sqlite");
        fs::copy(&temporary, &verification).expect("copy fixture for verification");
        logical_snapshot(&verification).await;
        assert_no_sidecars(&verification);
    });

    if checked.exists() {
        fs::remove_file(&checked).expect("remove prior checked fixture");
    }
    fs::rename(&temporary, &checked).expect("install checked fixture");
    assert_no_sidecars(&checked);
}
