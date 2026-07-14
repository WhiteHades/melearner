use std::future::Future;

use melearner_core::schema;
use sqlx::sqlite::SqliteConnectOptions;
use sqlx::{Connection, Row, SqliteConnection};

fn block_on<T>(future: impl Future<Output = T>) -> T {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .expect("build schema runtime")
        .block_on(future)
}

#[test]
fn fresh_schema_is_strict_and_unversioned() {
    block_on(async {
        let options = SqliteConnectOptions::new()
            .filename(":memory:")
            .create_if_missing(true)
            .foreign_keys(true);
        let mut connection = SqliteConnection::connect_with(&options)
            .await
            .expect("open in-memory database");
        sqlx::raw_sql(schema::SQL)
            .execute(&mut connection)
            .await
            .expect("create current schema");

        let tables = sqlx::query_scalar::<_, String>(
            "SELECT name
             FROM sqlite_schema
             WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
             ORDER BY name",
        )
        .fetch_all(&mut connection)
        .await
        .expect("list current tables");
        assert_eq!(
            tables,
            [
                "app_settings",
                "courses",
                "lesson_activity",
                "lesson_subtitles",
                "lessons",
                "notes",
                "sections",
            ]
        );
        assert_eq!(
            sqlx::query_scalar::<_, String>("PRAGMA integrity_check")
                .fetch_one(&mut connection)
                .await
                .expect("check schema integrity"),
            "ok"
        );
        assert!(
            sqlx::query("PRAGMA foreign_key_check")
                .fetch_all(&mut connection)
                .await
                .expect("check schema foreign keys")
                .is_empty()
        );
        assert!(
            sqlx::raw_sql(schema::SQL)
                .execute(&mut connection)
                .await
                .is_err()
        );

        assert!(
            sqlx::query(
                "INSERT INTO courses (id, name, path, fingerprint, last_scanned_at)
                 VALUES ('missing-identity', 'Course', '/library/course', 'fingerprint', 'now')",
            )
            .execute(&mut connection)
            .await
            .is_err()
        );
        sqlx::query(
            "INSERT INTO courses
             (id, identity_id, name, path, fingerprint, last_scanned_at)
             VALUES ('course', 'identity', 'Course', '/library/course', 'fingerprint', 'now')",
        )
        .execute(&mut connection)
        .await
        .expect("insert current course");
        sqlx::query(
            "INSERT INTO sections (id, course_id, name)
             VALUES ('section', 'course', 'Section')",
        )
        .execute(&mut connection)
        .await
        .expect("insert current section");
        assert!(
            sqlx::query(
                "INSERT INTO lessons
                 (id, course_id, section_id, name, path, relative_path, type)
                 VALUES (
                    'wrong-section', 'course', 'missing', 'Lesson',
                    '/library/course/lesson.mp4', 'lesson.mp4', 'video'
                 )",
            )
            .execute(&mut connection)
            .await
            .is_err()
        );
        sqlx::query(
            "INSERT INTO lessons
             (id, course_id, section_id, name, path, relative_path, type)
             VALUES (
                'lesson', 'course', 'section', 'Lesson',
                '/library/course/lesson.mp4', 'lesson.mp4', 'video'
             )",
        )
        .execute(&mut connection)
        .await
        .expect("insert current lesson");
        assert!(
            sqlx::query("UPDATE lessons SET watched_time = -1 WHERE id = 'lesson'")
                .execute(&mut connection)
                .await
                .is_err()
        );

        let foreign_keys = sqlx::query("PRAGMA foreign_keys")
            .fetch_one(&mut connection)
            .await
            .expect("read foreign key mode")
            .get::<i64, _>(0);
        assert_eq!(foreign_keys, 1);
        connection.close().await.expect("close current database");
    });
}
