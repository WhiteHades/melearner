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

fn fixture_migrator() -> Migrator {
    Migrator {
        migrations: Cow::Owned(
            fixture_migrations()
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
