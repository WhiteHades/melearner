use std::mem::size_of;
use std::path::Path;
use std::ptr;
use std::sync::mpsc;
use std::time::{Duration, Instant};

use melearner_core::*;
use sqlx::sqlite::SqliteConnectOptions;
use sqlx::{Connection, SqliteConnection};

const CURRENT_SEED: &str = include_str!("../../../fixtures/parity/database-current.sql");

fn config(state_dir: &[u8]) -> ml_config_v2 {
    ml_config_v2 {
        struct_size: size_of::<ml_config_v2>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 4,
        max_event_payload_bytes: 4096,
        state_dir: state_dir.as_ptr(),
        state_dir_len: state_dir.len(),
    }
}

fn event() -> ml_event_v1 {
    ml_event_v1 {
        struct_size: size_of::<ml_event_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        sequence: 0,
        request_id: 0,
        kind: 0,
        status: 0,
        payload_schema_version: 0,
        reserved: 0,
        payload: ptr::null(),
        payload_len: 0,
    }
}

fn poll(core: *mut ml_core_t) -> ml_event_v1 {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let mut next = event();
        match unsafe { ml_core_poll_event(core, &mut next) } {
            ML_STATUS_OK => return next,
            ML_STATUS_EMPTY if Instant::now() < deadline => std::thread::yield_now(),
            status => panic!("event did not arrive: status {status}"),
        }
    }
}

fn ready_revision(core: *mut ml_core_t) -> u64 {
    let mut ready = poll(core);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    assert_eq!(ready.status, ML_STATUS_OK);
    assert_eq!(ready.payload_schema_version, 1);
    assert!(!ready.payload.is_null());
    let payload = unsafe { std::slice::from_raw_parts(ready.payload, ready.payload_len) };
    let revision = std::str::from_utf8(payload)
        .expect("ready revision is UTF-8")
        .parse()
        .expect("ready revision is an integer");
    assert_ne!(revision, 0);
    unsafe { ml_core_release_event(core, &mut ready) };
    revision
}

fn seed_current_database(data_dir: &Path) {
    tokio::runtime::Builder::new_current_thread()
        .build()
        .expect("build progress seed runtime")
        .block_on(async {
            let options = SqliteConnectOptions::new()
                .filename(data_dir.join("melearner-native.sqlite3"))
                .foreign_keys(true)
                .busy_timeout(Duration::from_secs(10));
            let mut connection = SqliteConnection::connect_with(&options)
                .await
                .expect("open progress database");
            sqlx::raw_sql(CURRENT_SEED)
                .execute(&mut connection)
                .await
                .expect("seed progress database");
            connection.close().await.expect("close progress database");
        });
}

#[test]
fn course_page_returns_a_correlated_json_completion() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    assert_ne!(request_id, 0);

    let mut completed = poll(core);
    assert_eq!(completed.request_id, request_id);
    assert_eq!(completed.kind, ML_EVENT_LIBRARY_COURSE_PAGE);
    assert_eq!(completed.status, ML_STATUS_OK);
    assert_eq!(completed.payload_schema_version, 1);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(completed.payload, completed.payload_len) },
        format!(r#"{{"revision":{revision},"offset":0,"total":0,"rows":[]}}"#).as_bytes()
    );
    unsafe { ml_core_release_event(core, &mut completed) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn library_stats_round_trip_through_the_versioned_abi() {
    let data_dir = tempfile::tempdir().expect("create ABI stats state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary stats state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    ready_revision(core);
    unsafe { ml_core_destroy(core) };
    seed_current_database(data_dir.path());

    core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let mut request = ml_library_stats_request_v1 {
        struct_size: size_of::<ml_library_stats_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        reserved: 0,
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_library_stats_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    request.struct_size -= 1;
    assert_eq!(
        unsafe { ml_library_stats_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.struct_size += 1;
    request.reserved = 1;
    assert_eq!(
        unsafe { ml_library_stats_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.reserved = 0;
    assert_eq!(
        unsafe { ml_library_stats_v1(core, &request, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    assert_eq!(
        unsafe { ml_library_stats_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    assert_ne!(request_id, 0);
    let mut completed = poll(core);
    assert_eq!(completed.request_id, request_id);
    assert_eq!(completed.kind, ML_EVENT_LIBRARY_STATS);
    assert_eq!(completed.status, ML_STATUS_OK);
    assert_eq!(completed.payload_schema_version, 1);
    let payload = unsafe { std::slice::from_raw_parts(completed.payload, completed.payload_len) };
    let stats: serde_json::Value =
        serde_json::from_slice(payload).expect("parse aggregate Library stats");
    assert_eq!(stats["revision"], revision);
    assert_eq!(stats["totalCourses"], 3);
    assert_eq!(stats["availableCourses"], 1);
    assert_eq!(stats["missingCourses"], 2);
    assert_eq!(stats["sections"], 4);
    assert_eq!(stats["lessons"], 4);
    assert_eq!(stats["completedLessons"], 2);
    assert_eq!(stats["completionPercent"], 50);
    assert_eq!(stats["bytes"], 5_246_976);
    assert_eq!(stats["watchedSeconds"], 620);
    assert_eq!(stats["totalSeconds"], 1_200);
    assert_eq!(stats["mediaTypes"][0]["type"], "video");
    assert_eq!(stats["mediaTypes"][0]["lessons"], 3);
    assert_eq!(stats["mediaTypes"][1]["type"], "document");
    assert_eq!(stats["topCourses"][0]["id"], "course-marker");
    assert_eq!(stats["topCourses"][1]["id"], "course-missing");
    assert_eq!(stats["topCourses"][2]["id"], "course-copy");
    unsafe { ml_core_release_event(core, &mut completed) };

    request.expected_revision = revision + 1;
    assert_eq!(
        unsafe { ml_library_stats_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.kind, ML_EVENT_LIBRARY_STATS);
    assert_eq!(stale.status, ML_STATUS_STALE);
    let payload = unsafe { std::slice::from_raw_parts(stale.payload, stale.payload_len) };
    let error: serde_json::Value =
        serde_json::from_slice(payload).expect("parse stale Library-stats error");
    assert_eq!(error["error"], "staleRevision");
    assert_eq!(error["expected"], revision + 1);
    assert_eq!(error["actual"], revision);
    unsafe { ml_core_release_event(core, &mut stale) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn course_access_round_trips_through_the_versioned_abi() {
    let data_dir = tempfile::tempdir().expect("create ABI Course-access state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary Course-access state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    ready_revision(core);
    unsafe { ml_core_destroy(core) };
    seed_current_database(data_dir.path());

    core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let mut course_id = b"course-marker".to_vec();
    let mut request = ml_course_access_request_v1 {
        struct_size: size_of::<ml_course_access_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        reserved: 0,
        course_id: course_id.as_ptr(),
        course_id_len: course_id.len(),
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_course_access_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    request.struct_size -= 1;
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.struct_size += 1;
    request.reserved = 1;
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.reserved = 0;
    request.course_id = ptr::null();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let invalid_utf8 = [0xff];
    request.course_id = invalid_utf8.as_ptr();
    request.course_id_len = invalid_utf8.len();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let embedded_nul = b"bad\0course";
    request.course_id = embedded_nul.as_ptr();
    request.course_id_len = embedded_nul.len();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );

    request.course_id = course_id.as_ptr();
    request.course_id_len = course_id.len();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    course_id.fill(b'x');
    let mut accessed = poll(core);
    assert_eq!(accessed.request_id, request_id);
    assert_eq!(accessed.kind, ML_EVENT_COURSE_ACCESSED);
    assert_eq!(accessed.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(accessed.payload, accessed.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse Course access");
    let accessed_revision = result["revision"]
        .as_u64()
        .expect("Course access has revision");
    assert!(accessed_revision > revision);
    assert_eq!(result["courseId"], "course-marker");
    let last_accessed = result["lastAccessed"]
        .as_str()
        .expect("Course access has timestamp");
    assert!(last_accessed.contains('T'));
    assert!(last_accessed.ends_with('Z'));
    unsafe { ml_core_release_event(core, &mut accessed) };

    course_id = b"course-marker".to_vec();
    request.course_id = course_id.as_ptr();
    request.course_id_len = course_id.len();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.kind, ML_EVENT_COURSE_ACCESSED);
    assert_eq!(stale.status, ML_STATUS_STALE);
    unsafe { ml_core_release_event(core, &mut stale) };

    course_id = b"course-missing".to_vec();
    request.expected_revision = accessed_revision;
    request.course_id = course_id.as_ptr();
    request.course_id_len = course_id.len();
    assert_eq!(
        unsafe { ml_course_access_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut missing = poll(core);
    assert_eq!(missing.kind, ML_EVENT_COURSE_ACCESSED);
    assert_eq!(missing.status, ML_STATUS_NOT_FOUND);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(missing.payload, missing.payload_len) },
        br#"{"error":"courseNotFound"}"#
    );
    unsafe { ml_core_release_event(core, &mut missing) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn progress_and_activity_round_trip_through_the_versioned_abi() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    ready_revision(core);
    unsafe { ml_core_destroy(core) };
    seed_current_database(data_dir.path());

    core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let initial_revision = ready_revision(core);
    let mut lesson_id = b"lesson-video".to_vec();
    let mut progress = ml_progress_put_request_v1 {
        struct_size: size_of::<ml_progress_put_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: initial_revision,
        watched_time: 340,
        last_position: 338.5,
        completed: 0,
        reserved: 0,
        lesson_id: lesson_id.as_ptr(),
        lesson_id_len: lesson_id.len(),
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_progress_put_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    progress.struct_size -= 1;
    assert_eq!(
        unsafe { ml_progress_put_v1(core, &progress, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    progress.struct_size += 1;
    progress.completed = 2;
    assert_eq!(
        unsafe { ml_progress_put_v1(core, &progress, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    progress.completed = 0;
    assert_eq!(
        unsafe { ml_progress_put_v1(core, &progress, &mut request_id) },
        ML_STATUS_OK
    );
    assert_ne!(request_id, 0);
    lesson_id.fill(b'x');

    let mut updated = poll(core);
    assert_eq!(updated.request_id, request_id);
    assert_eq!(updated.kind, ML_EVENT_PROGRESS_UPDATED);
    assert_eq!(updated.status, ML_STATUS_OK);
    assert_eq!(updated.payload_schema_version, 1);
    let payload = unsafe { std::slice::from_raw_parts(updated.payload, updated.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse progress update");
    let revision = result["revision"]
        .as_u64()
        .expect("progress update has a revision");
    assert!(revision > initial_revision);
    assert_eq!(result["lessonId"], "lesson-video");
    assert_eq!(result["watchedTime"], 340);
    assert_eq!(result["lastPosition"], 338.5);
    assert_eq!(result["completed"], false);
    unsafe { ml_core_release_event(core, &mut updated) };

    let mut activity = ml_activity_day_page_request_v1 {
        struct_size: size_of::<ml_activity_day_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: initial_revision,
        offset: 0,
        lookback_days: 84,
        limit: 20,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_activity_day_page_v1(core, &activity, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.kind, ML_EVENT_ACTIVITY_DAY_PAGE);
    assert_eq!(stale.status, ML_STATUS_STALE);
    unsafe { ml_core_release_event(core, &mut stale) };

    activity.expected_revision = revision;
    assert_eq!(
        unsafe { ml_activity_day_page_v1(core, &activity, &mut request_id) },
        ML_STATUS_OK
    );
    let mut page = poll(core);
    assert_eq!(page.kind, ML_EVENT_ACTIVITY_DAY_PAGE);
    assert_eq!(page.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse activity page");
    assert_eq!(result["revision"], revision);
    let today = result["rows"]
        .as_array()
        .expect("activity rows are an array")
        .iter()
        .find(|day| day["watchedSeconds"] == 20)
        .expect("activity includes the progress delta");
    assert_eq!(today["lessonsTouched"], 1);
    assert_eq!(today["completions"], 0);
    unsafe { ml_core_release_event(core, &mut page) };

    activity.reserved = 1;
    assert_eq!(
        unsafe { ml_activity_day_page_v1(core, &activity, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn notes_list_save_update_and_delete_round_trip_through_the_versioned_abi() {
    let data_dir = tempfile::tempdir().expect("create ABI notes state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary notes state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    ready_revision(core);
    unsafe { ml_core_destroy(core) };
    seed_current_database(data_dir.path());

    core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let mut lesson_id = b"lesson-video".to_vec();
    let mut list = ml_notes_list_request_v1 {
        struct_size: size_of::<ml_notes_list_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
        lesson_id: lesson_id.as_ptr(),
        lesson_id_len: lesson_id.len(),
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_notes_list_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    list.struct_size -= 1;
    assert_eq!(
        unsafe { ml_notes_list_v1(core, &list, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    list.struct_size += 1;
    list.reserved = 1;
    assert_eq!(
        unsafe { ml_notes_list_v1(core, &list, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    list.reserved = 0;
    assert_eq!(
        unsafe { ml_notes_list_v1(core, &list, &mut request_id) },
        ML_STATUS_OK
    );
    lesson_id.fill(b'x');
    let mut page = poll(core);
    assert_eq!(page.request_id, request_id);
    assert_eq!(page.kind, ML_EVENT_NOTES_PAGE);
    assert_eq!(page.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let listed: serde_json::Value = serde_json::from_slice(payload).expect("parse note page");
    assert_eq!(listed["revision"], revision);
    assert_eq!(listed["lessonId"], "lesson-video");
    assert_eq!(listed["total"], 2);
    assert_eq!(listed["rows"][0]["id"], "note-video-1");
    assert_eq!(listed["rows"][1]["id"], "note-video-2");
    unsafe { ml_core_release_event(core, &mut page) };

    lesson_id = b"lesson-video".to_vec();
    let mut text = "  Native 復習 note  ".as_bytes().to_vec();
    let mut save = ml_notes_save_request_v1 {
        struct_size: size_of::<ml_notes_save_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        timestamp: 99.5,
        reserved: 0,
        lesson_id: lesson_id.as_ptr(),
        lesson_id_len: lesson_id.len(),
        note_id: ptr::null(),
        note_id_len: 0,
        text: text.as_ptr(),
        text_len: text.len(),
    };
    assert_eq!(
        unsafe { ml_notes_save_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    save.struct_size -= 1;
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    save.struct_size += 1;
    save.note_id_len = 1;
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    save.note_id_len = 0;
    let note_id_probe = b"note";
    save.note_id = note_id_probe.as_ptr();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    save.note_id = ptr::null();
    save.reserved = 1;
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    save.reserved = 0;
    save.timestamp = f64::NAN;
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    save.timestamp = 99.5;
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_OK
    );
    lesson_id.fill(b'x');
    text.fill(b'x');
    let mut saved = poll(core);
    assert_eq!(saved.request_id, request_id);
    assert_eq!(saved.kind, ML_EVENT_NOTE_SAVED);
    assert_eq!(saved.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(saved.payload, saved.payload_len) };
    let created: serde_json::Value = serde_json::from_slice(payload).expect("parse saved note");
    let created_revision = created["revision"]
        .as_u64()
        .expect("saved note has revision");
    let note_id = created["id"]
        .as_str()
        .expect("saved note has ID")
        .to_string();
    let created_at = created["createdAt"]
        .as_str()
        .expect("saved note has creation time")
        .to_string();
    assert!(created_revision > revision);
    assert_eq!(created["lessonId"], "lesson-video");
    assert_eq!(created["timestamp"], 99.5);
    assert_eq!(created["text"], "Native 復習 note");
    unsafe { ml_core_release_event(core, &mut saved) };

    let mut note_id_bytes = note_id.as_bytes().to_vec();
    lesson_id = b"lesson-video".to_vec();
    text = b"updated note".to_vec();
    save.expected_revision = created_revision;
    save.timestamp = 5.0;
    save.lesson_id = lesson_id.as_ptr();
    save.lesson_id_len = lesson_id.len();
    save.note_id = note_id_bytes.as_ptr();
    save.note_id_len = note_id_bytes.len();
    save.text = text.as_ptr();
    save.text_len = text.len();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_OK
    );
    note_id_bytes.fill(b'x');
    text.fill(b'x');
    let mut updated = poll(core);
    assert_eq!(updated.kind, ML_EVENT_NOTE_SAVED);
    assert_eq!(updated.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(updated.payload, updated.payload_len) };
    let changed: serde_json::Value = serde_json::from_slice(payload).expect("parse updated note");
    let updated_revision = changed["revision"]
        .as_u64()
        .expect("updated note has revision");
    assert!(updated_revision > created_revision);
    assert_eq!(changed["id"], note_id);
    assert_eq!(changed["createdAt"], created_at);
    assert_eq!(changed["text"], "updated note");
    unsafe { ml_core_release_event(core, &mut updated) };

    note_id_bytes = note_id.as_bytes().to_vec();
    let mut delete = ml_notes_delete_request_v1 {
        struct_size: size_of::<ml_notes_delete_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: updated_revision,
        reserved: 0,
        note_id: note_id_bytes.as_ptr(),
        note_id_len: note_id_bytes.len(),
    };
    assert_eq!(
        unsafe { ml_notes_delete_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    delete.struct_size -= 1;
    assert_eq!(
        unsafe { ml_notes_delete_v1(core, &delete, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    delete.struct_size += 1;
    delete.reserved = 1;
    assert_eq!(
        unsafe { ml_notes_delete_v1(core, &delete, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    delete.reserved = 0;
    assert_eq!(
        unsafe { ml_notes_delete_v1(core, &delete, &mut request_id) },
        ML_STATUS_OK
    );
    note_id_bytes.fill(b'x');
    let mut deleted = poll(core);
    assert_eq!(deleted.kind, ML_EVENT_NOTE_DELETED);
    assert_eq!(deleted.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(deleted.payload, deleted.payload_len) };
    let removed: serde_json::Value = serde_json::from_slice(payload).expect("parse deleted note");
    let deleted_revision = removed["revision"]
        .as_u64()
        .expect("deleted note has revision");
    assert!(deleted_revision > updated_revision);
    assert_eq!(removed["noteId"], note_id);
    unsafe { ml_core_release_event(core, &mut deleted) };

    let mut nul_text = b"a\0b".to_vec();
    save.expected_revision = deleted_revision;
    save.note_id = ptr::null();
    save.note_id_len = 0;
    save.text = nul_text.as_ptr();
    save.text_len = nul_text.len();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_OK
    );
    nul_text.fill(b'x');
    let mut nul_saved = poll(core);
    assert_eq!(nul_saved.kind, ML_EVENT_NOTE_SAVED);
    assert_eq!(nul_saved.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(nul_saved.payload, nul_saved.payload_len) };
    let nul_note: serde_json::Value = serde_json::from_slice(payload).expect("parse NUL note");
    let nul_revision = nul_note["revision"]
        .as_u64()
        .expect("NUL note has revision");
    assert_eq!(nul_note["text"], "a\0b");
    unsafe { ml_core_release_event(core, &mut nul_saved) };

    let byte_order_mark = "\u{FEFF}".as_bytes();
    save.expected_revision = nul_revision;
    save.text = byte_order_mark.as_ptr();
    save.text_len = byte_order_mark.len();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_OK
    );
    let mut invalid_note = poll(core);
    assert_eq!(invalid_note.kind, ML_EVENT_NOTE_SAVED);
    assert_eq!(invalid_note.status, ML_STATUS_INVALID_ARGUMENT);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(invalid_note.payload, invalid_note.payload_len) },
        br#"{"error":"invalidNote"}"#
    );
    unsafe { ml_core_release_event(core, &mut invalid_note) };

    let invalid_utf8 = [0xff];
    save.text = invalid_utf8.as_ptr();
    save.text_len = invalid_utf8.len();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let oversized = vec![b'x'; ML_MAX_NOTE_TEXT_BYTES as usize + 1];
    save.text = oversized.as_ptr();
    save.text_len = oversized.len();
    assert_eq!(
        unsafe { ml_notes_save_v1(core, &save, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn search_rebuild_and_query_round_trip_through_the_versioned_abi() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let library_dir = tempfile::tempdir().expect("create ABI library directory");
    for relative_path in [
        "Systems/Core Concepts/14 Binary Heaps.mp4",
        "Systems/Core Concepts/notes.pdf",
    ] {
        let path = library_dir.path().join(relative_path);
        std::fs::create_dir_all(path.parent().expect("search item has a parent"))
            .expect("create search item parent");
        std::fs::write(path, b"search fixture").expect("write search item");
    }

    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let root_path = library_dir
        .path()
        .to_str()
        .expect("temporary library directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let initial_revision = ready_revision(core);
    let scan = ml_library_scan_request_v1 {
        struct_size: size_of::<ml_library_scan_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: initial_revision,
        root_path: root_path.as_ptr(),
        root_path_len: root_path.len(),
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &scan, &mut request_id) },
        ML_STATUS_OK
    );
    let mut scanned = poll(core);
    let payload = unsafe { std::slice::from_raw_parts(scanned.payload, scanned.payload_len) };
    let scan_result: serde_json::Value =
        serde_json::from_slice(payload).expect("parse scan result");
    let revision = scan_result["revision"]
        .as_u64()
        .expect("scan result has revision");
    unsafe { ml_core_release_event(core, &mut scanned) };

    let mut query_bytes = b"binary heaps".to_vec();
    let mut query = ml_search_query_request_v1 {
        struct_size: size_of::<ml_search_query_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_index_revision: revision,
        query_id: 42,
        offset: 0,
        limit: 20,
        reserved: 0,
        query: query_bytes.as_ptr(),
        query_len: query_bytes.len(),
    };
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.kind, ML_EVENT_SEARCH_PAGE);
    assert_eq!(stale.status, ML_STATUS_STALE);
    let payload = unsafe { std::slice::from_raw_parts(stale.payload, stale.payload_len) };
    let stale_result: serde_json::Value =
        serde_json::from_slice(payload).expect("parse stale search result");
    assert_eq!(stale_result["error"], "staleSearchIndex");
    assert_eq!(stale_result["expected"], revision);
    assert!(stale_result["actual"].is_null());
    unsafe { ml_core_release_event(core, &mut stale) };

    let mut rebuild = ml_search_rebuild_request_v1 {
        struct_size: size_of::<ml_search_rebuild_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        reserved: 0,
    };
    request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_search_rebuild_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    rebuild.struct_size -= 1;
    assert_eq!(
        unsafe { ml_search_rebuild_v1(core, &rebuild, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    rebuild.struct_size += 1;
    rebuild.reserved = 1;
    assert_eq!(
        unsafe { ml_search_rebuild_v1(core, &rebuild, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    rebuild.reserved = 0;
    assert_eq!(
        unsafe { ml_search_rebuild_v1(core, &rebuild, &mut request_id) },
        ML_STATUS_OK
    );
    let mut ready = poll(core);
    assert_eq!(ready.request_id, request_id);
    assert_eq!(ready.kind, ML_EVENT_SEARCH_INDEX_READY);
    assert_eq!(ready.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(ready.payload, ready.payload_len) };
    let ready_result: serde_json::Value =
        serde_json::from_slice(payload).expect("parse search index result");
    assert_eq!(ready_result["indexRevision"], revision);
    assert_eq!(ready_result["entryCount"], 2);
    unsafe { ml_core_release_event(core, &mut ready) };

    assert_eq!(
        unsafe { ml_search_query_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    query.struct_size -= 1;
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    query.struct_size += 1;
    query.reserved = 1;
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    query.reserved = 0;
    let invalid_utf8 = [0xff];
    query.query = invalid_utf8.as_ptr();
    query.query_len = invalid_utf8.len();
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let oversized = vec![b'x'; ML_MAX_SEARCH_QUERY_BYTES as usize + 1];
    query.query = oversized.as_ptr();
    query.query_len = oversized.len();
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );

    query.query = query_bytes.as_ptr();
    query.query_len = query_bytes.len();
    assert_eq!(
        unsafe { ml_search_query_v1(core, &query, &mut request_id) },
        ML_STATUS_OK
    );
    query_bytes.fill(b'x');
    let mut page = poll(core);
    assert_eq!(page.request_id, request_id);
    assert_eq!(page.kind, ML_EVENT_SEARCH_PAGE);
    assert_eq!(page.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse search page");
    assert_eq!(result["queryId"], 42);
    assert_eq!(result["indexRevision"], revision);
    assert_eq!(result["offset"], 0);
    assert_eq!(result["total"], 1);
    let hit = &result["rows"][0];
    assert_eq!(hit["courseName"], "Systems");
    assert_eq!(hit["sectionName"], "Core Concepts");
    assert_eq!(hit["name"], "14 Binary Heaps");
    assert_eq!(hit["relativePath"], "Core Concepts/14 Binary Heaps.mp4");
    assert_eq!(hit["kind"], "video");
    assert!(hit["score"].as_i64().is_some_and(|score| score > 0));
    unsafe { ml_core_release_event(core, &mut page) };
    unsafe { ml_core_destroy(core) };

    let mut limited = config(state_dir);
    limited.max_event_payload_bytes = ML_MIN_EVENT_PAYLOAD_BYTES;
    core = ptr::null_mut();
    assert_eq!(unsafe { ml_core_create(&limited, &mut core) }, ML_STATUS_OK);
    rebuild.expected_revision = ready_revision(core);
    assert_eq!(
        unsafe { ml_search_rebuild_v1(core, &rebuild, &mut request_id) },
        ML_STATUS_OK
    );
    let mut rejected = poll(core);
    assert_eq!(rejected.request_id, request_id);
    assert_eq!(rejected.kind, ML_EVENT_SEARCH_INDEX_READY);
    assert_eq!(rejected.status, ML_STATUS_FAILED);
    assert_eq!(rejected.payload_schema_version, 0);
    assert_eq!(rejected.payload_len, 0);
    unsafe { ml_core_release_event(core, &mut rejected) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn scan_commits_a_new_revision_and_populates_native_pages() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let library_dir = tempfile::tempdir().expect("create ABI library directory");
    for relative_path in [
        "Rust Basics/01 Intro/01 welcome.mp4",
        "Docs Course/Reading/guide.pdf",
    ] {
        let path = library_dir.path().join(relative_path);
        std::fs::create_dir_all(path.parent().expect("learning item has a parent"))
            .expect("create learning item parent");
        std::fs::write(path, b"learning item").expect("write learning item");
    }

    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let initial_revision = ready_revision(core);

    let mut root_path = library_dir
        .path()
        .to_str()
        .expect("temporary library directory is UTF-8")
        .as_bytes()
        .to_vec();
    let request = ml_library_scan_request_v1 {
        struct_size: size_of::<ml_library_scan_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: initial_revision,
        root_path: root_path.as_ptr(),
        root_path_len: root_path.len(),
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    root_path.fill(b'x');

    let mut completed = poll(core);
    assert_eq!(completed.request_id, request_id);
    assert_eq!(completed.kind, ML_EVENT_LIBRARY_SCAN);
    assert_eq!(completed.status, ML_STATUS_OK);
    assert_eq!(completed.payload_schema_version, 1);
    assert!(!completed.payload.is_null());
    let payload = unsafe { std::slice::from_raw_parts(completed.payload, completed.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse scan result");
    let revision = result["revision"]
        .as_u64()
        .expect("scan result has a revision");
    assert!(revision > initial_revision);
    assert_eq!(result["courseCount"], 2);
    unsafe { ml_core_release_event(core, &mut completed) };

    let mut page_request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: initial_revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &page_request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.status, ML_STATUS_STALE);
    unsafe { ml_core_release_event(core, &mut stale) };

    page_request.expected_revision = revision;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &page_request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut page = poll(core);
    assert_eq!(page.status, ML_STATUS_OK);
    assert!(!page.payload.is_null());
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let page_result: serde_json::Value =
        serde_json::from_slice(payload).expect("parse course page");
    assert_eq!(page_result["revision"], revision);
    assert_eq!(page_result["total"], 2);
    assert_eq!(page_result["rows"].as_array().map(Vec::len), Some(2));
    unsafe { ml_core_release_event(core, &mut page) };

    for course in ["Rust Basics", "Docs Course"] {
        assert!(
            library_dir
                .path()
                .join(course)
                .join(".melearner-course.json")
                .is_file()
        );
    }
    unsafe { ml_core_destroy(core) };
}

#[test]
fn scan_validates_and_copies_its_versioned_root_path() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let library_dir = tempfile::tempdir().expect("create ABI library directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let root_path = library_dir
        .path()
        .to_str()
        .expect("temporary library directory is UTF-8")
        .as_bytes();
    let invalid_utf8 = [0xff];
    let mut request = ml_library_scan_request_v1 {
        struct_size: size_of::<ml_library_scan_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        root_path: root_path.as_ptr(),
        root_path_len: root_path.len(),
    };
    let mut request_id = u64::MAX;

    assert_eq!(
        unsafe { ml_library_scan_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    request.struct_size -= 1;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.struct_size += 1;
    request.abi_version += 1;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.abi_version = ML_ABI_VERSION;

    request.root_path = ptr::null();
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.root_path = root_path.as_ptr();
    request.root_path_len = 0;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.root_path = invalid_utf8.as_ptr();
    request.root_path_len = 1;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.root_path = b"invalid\0root".as_ptr();
    request.root_path_len = b"invalid\0root".len();
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let oversized_root = vec![b'x'; ML_MAX_EVENT_PAYLOAD_BYTES as usize + 1];
    request.root_path = oversized_root.as_ptr();
    request.root_path_len = oversized_root.len();
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let missing_root = library_dir.path().join("missing");
    let missing_root = missing_root
        .to_str()
        .expect("missing library path is UTF-8")
        .as_bytes();
    request.root_path = missing_root.as_ptr();
    request.root_path_len = missing_root.len();
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut invalid = poll(core);
    assert_eq!(invalid.request_id, request_id);
    assert_eq!(invalid.kind, ML_EVENT_LIBRARY_SCAN);
    assert_eq!(invalid.status, ML_STATUS_INVALID_ARGUMENT);
    assert!(!invalid.payload.is_null());
    assert_eq!(
        unsafe { std::slice::from_raw_parts(invalid.payload, invalid.payload_len) },
        br#"{"error":"invalidScan"}"#
    );
    unsafe { ml_core_release_event(core, &mut invalid) };

    let page_request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 1,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &page_request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut unchanged = poll(core);
    assert_eq!(unchanged.status, ML_STATUS_OK);
    unsafe { ml_core_release_event(core, &mut unchanged) };

    unsafe { ml_core_destroy(core) };
}

#[test]
fn scan_payload_limits_roll_back_before_revision_and_marker_writes() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let library_dir = tempfile::tempdir().expect("create ABI library directory");
    let lesson = library_dir.path().join("Course/01 Intro/01 lesson.mp4");
    std::fs::create_dir_all(lesson.parent().expect("lesson has a parent"))
        .expect("create lesson parent");
    std::fs::write(&lesson, b"learning item").expect("write lesson");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let root_path = library_dir
        .path()
        .to_str()
        .expect("temporary library directory is UTF-8")
        .as_bytes();
    let mut limited = config(state_dir);
    limited.max_event_payload_bytes = ML_MIN_EVENT_PAYLOAD_BYTES;
    let mut core = ptr::null_mut();
    assert_eq!(unsafe { ml_core_create(&limited, &mut core) }, ML_STATUS_OK);
    let revision = ready_revision(core);
    let request = ml_library_scan_request_v1 {
        struct_size: size_of::<ml_library_scan_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        root_path: root_path.as_ptr(),
        root_path_len: root_path.len(),
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut failed = poll(core);
    assert_eq!(failed.request_id, request_id);
    assert_eq!(failed.kind, ML_EVENT_LIBRARY_SCAN);
    assert_eq!(failed.status, ML_STATUS_FAILED);
    assert_eq!(failed.payload_schema_version, 0);
    assert!(failed.payload.is_null());
    assert_eq!(failed.payload_len, 0);
    unsafe { ml_core_release_event(core, &mut failed) };
    unsafe { ml_core_destroy(core) };
    assert!(
        !library_dir
            .path()
            .join("Course/.melearner-course.json")
            .exists()
    );

    core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let reopened_revision = ready_revision(core);
    let page_request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: reopened_revision,
        offset: 0,
        limit: 1,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &page_request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut page = poll(core);
    assert_eq!(page.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse course page");
    assert_eq!(result["total"], 0);
    unsafe { ml_core_release_event(core, &mut page) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn cancelling_an_active_scan_emits_one_event_and_leaves_no_writes() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let library_dir = tempfile::tempdir().expect("create ABI library directory");
    let section = library_dir.path().join("Course/01 Intro");
    std::fs::create_dir_all(&section).expect("create scan fixture section");
    for index in 0..2_000 {
        std::fs::write(
            section.join(format!("{index:04} lesson.mp4")),
            b"learning item",
        )
        .expect("write scan fixture lesson");
    }
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let root_path = library_dir
        .path()
        .to_str()
        .expect("temporary library directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let request = ml_library_scan_request_v1 {
        struct_size: size_of::<ml_library_scan_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        root_path: root_path.as_ptr(),
        root_path_len: root_path.len(),
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_scan_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    assert_eq!(ml_core_cancel(core, request_id), ML_STATUS_OK);

    let mut cancelled = poll(core);
    assert_eq!(cancelled.request_id, request_id);
    assert_eq!(cancelled.kind, ML_EVENT_REQUEST_CANCELLED);
    assert_eq!(cancelled.status, ML_STATUS_CANCELLED);
    unsafe { ml_core_release_event(core, &mut cancelled) };

    let page_request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 1,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &page_request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut page = poll(core);
    assert_eq!(page.kind, ML_EVENT_LIBRARY_COURSE_PAGE);
    assert_eq!(page.status, ML_STATUS_OK);
    let payload = unsafe { std::slice::from_raw_parts(page.payload, page.payload_len) };
    let result: serde_json::Value = serde_json::from_slice(payload).expect("parse course page");
    assert_eq!(result["total"], 0);
    unsafe { ml_core_release_event(core, &mut page) };
    assert!(
        !library_dir
            .path()
            .join("Course/.melearner-course.json")
            .exists()
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn replacement_core_rejects_the_previous_session_revision() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut first = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut first) },
        ML_STATUS_OK
    );
    let first_revision = ready_revision(first);
    unsafe { ml_core_destroy(first) };

    let mut replacement = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut replacement) },
        ML_STATUS_OK
    );
    let replacement_revision = ready_revision(replacement);
    assert!(replacement_revision > first_revision);

    let mut request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: first_revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_course_page_v1(replacement, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(replacement);
    assert_eq!(stale.request_id, request_id);
    assert_eq!(stale.kind, ML_EVENT_LIBRARY_COURSE_PAGE);
    assert_eq!(stale.status, ML_STATUS_STALE);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(stale.payload, stale.payload_len) },
        format!(
            r#"{{"actual":{replacement_revision},"error":"staleRevision","expected":{first_revision}}}"#
        )
        .as_bytes()
    );
    unsafe { ml_core_release_event(replacement, &mut stale) };

    request.expected_revision = replacement_revision;
    assert_eq!(
        unsafe { ml_library_course_page_v1(replacement, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut current = poll(replacement);
    assert_eq!(current.request_id, request_id);
    assert_eq!(current.status, ML_STATUS_OK);
    unsafe { ml_core_release_event(replacement, &mut current) };
    unsafe { ml_core_destroy(replacement) };
}

#[test]
fn course_page_validates_its_versioned_request() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let mut request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.struct_size -= 1;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.struct_size += 1;
    request.abi_version += 1;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.abi_version = ML_ABI_VERSION;
    request.reserved = 1;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn lesson_page_copies_unicode_filters_before_returning() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let mut course_id = "course α".as_bytes().to_vec();
    let mut section_id = "section β".as_bytes().to_vec();
    let request = ml_library_lesson_page_request_v1 {
        struct_size: size_of::<ml_library_lesson_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
        course_id: course_id.as_ptr(),
        course_id_len: course_id.len(),
        section_id: section_id.as_ptr(),
        section_id_len: section_id.len(),
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    course_id.fill(b'x');
    section_id.fill(b'y');

    let mut completed = poll(core);
    assert_eq!(completed.request_id, request_id);
    assert_eq!(completed.kind, ML_EVENT_LIBRARY_LESSON_PAGE);
    assert_eq!(completed.status, ML_STATUS_OK);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(completed.payload, completed.payload_len) },
        format!("{{\"revision\":{revision},\"courseId\":\"course α\",\"sectionId\":\"section β\",\"offset\":0,\"total\":0,\"rows\":[]}}").as_bytes()
    );
    unsafe { ml_core_release_event(core, &mut completed) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn lesson_page_validates_its_versioned_borrowed_inputs() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let course_id = b"course";
    let invalid_utf8 = [0xff];
    let mut request = ml_library_lesson_page_request_v1 {
        struct_size: size_of::<ml_library_lesson_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
        course_id: course_id.as_ptr(),
        course_id_len: course_id.len(),
        section_id: ptr::null(),
        section_id_len: 0,
    };
    let mut request_id = u64::MAX;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, ptr::null(), &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(request_id, 0);
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    request.struct_size -= 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.struct_size += 1;
    request.abi_version += 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_ABI_MISMATCH
    );
    request.abi_version = ML_ABI_VERSION;
    request.reserved = 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.reserved = 0;

    request.course_id = ptr::null();
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.course_id = course_id.as_ptr();
    request.course_id_len = 0;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.course_id = invalid_utf8.as_ptr();
    request.course_id_len = 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.course_id = ptr::dangling();
    request.course_id_len = ML_MAX_EVENT_PAYLOAD_BYTES as usize + 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.course_id = course_id.as_ptr();
    request.course_id_len = course_id.len();

    request.section_id_len = 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.section_id = course_id.as_ptr();
    request.section_id_len = 0;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.section_id = invalid_utf8.as_ptr();
    request.section_id_len = 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );
    request.section_id = ptr::dangling();
    request.section_id_len = ML_MAX_EVENT_PAYLOAD_BYTES as usize + 1;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_INVALID_ARGUMENT
    );

    request.section_id = ptr::null();
    request.section_id_len = 0;
    assert_eq!(
        unsafe { ml_library_lesson_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut completed = poll(core);
    assert_eq!(completed.request_id, request_id);
    assert_eq!(completed.kind, ML_EVENT_LIBRARY_LESSON_PAGE);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(completed.payload, completed.payload_len) },
        format!(r#"{{"revision":{revision},"courseId":"course","sectionId":null,"offset":0,"total":0,"rows":[]}}"#).as_bytes()
    );
    unsafe { ml_core_release_event(core, &mut completed) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn domain_errors_are_correlated_json_completions() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);
    let stale_revision = revision
        .checked_add(1)
        .expect("test revision has a successor");

    let mut request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: stale_revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut stale = poll(core);
    assert_eq!(stale.request_id, request_id);
    assert_eq!(stale.status, ML_STATUS_STALE);
    assert_eq!(stale.payload_schema_version, 1);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(stale.payload, stale.payload_len) },
        format!(r#"{{"actual":{revision},"error":"staleRevision","expected":{stale_revision}}}"#)
            .as_bytes()
    );
    unsafe { ml_core_release_event(core, &mut stale) };

    request.expected_revision = revision;
    request.limit = 0;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut invalid = poll(core);
    assert_eq!(invalid.request_id, request_id);
    assert_eq!(invalid.status, ML_STATUS_INVALID_ARGUMENT);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(invalid.payload, invalid.payload_len) },
        br#"{"error":"invalidPageSize","limit":0}"#
    );
    unsafe { ml_core_release_event(core, &mut invalid) };

    request.limit = 20;
    request.offset = u64::MAX;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let mut invalid = poll(core);
    assert_eq!(invalid.status, ML_STATUS_INVALID_ARGUMENT);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(invalid.payload, invalid.payload_len) },
        br#"{"error":"invalidOffset","offset":18446744073709551615}"#
    );
    unsafe { ml_core_release_event(core, &mut invalid) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn oversized_domain_payloads_fail_terminally_without_consuming_capacity() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut limited = config(state_dir);
    limited.event_queue_capacity = 2;
    limited.max_event_payload_bytes = ML_MIN_EVENT_PAYLOAD_BYTES;
    let mut core = ptr::null_mut();
    assert_eq!(unsafe { ml_core_create(&limited, &mut core) }, ML_STATUS_OK);
    let revision = ready_revision(core);

    let request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    for _ in 0..2 {
        let mut request_id = 0;
        assert_eq!(
            unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
            ML_STATUS_OK
        );
        let mut failed = poll(core);
        assert_eq!(failed.request_id, request_id);
        assert_eq!(failed.status, ML_STATUS_FAILED);
        assert_eq!(failed.payload_schema_version, 0);
        assert!(failed.payload.is_null());
        assert_eq!(failed.payload_len, 0);
        unsafe { ml_core_release_event(core, &mut failed) };
    }
    unsafe { ml_core_destroy(core) };
}

#[test]
fn cancellation_and_domain_completion_emit_one_terminal_event() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let cancel_status = ml_core_cancel(core, request_id);
    assert!(matches!(cancel_status, ML_STATUS_OK | ML_STATUS_NOT_FOUND));
    let mut terminal = poll(core);
    assert_eq!(terminal.request_id, request_id);
    if cancel_status == ML_STATUS_OK {
        assert_eq!(terminal.kind, ML_EVENT_REQUEST_CANCELLED);
        assert_eq!(terminal.status, ML_STATUS_CANCELLED);
    } else {
        assert_eq!(terminal.kind, ML_EVENT_LIBRARY_COURSE_PAGE);
    }
    unsafe { ml_core_release_event(core, &mut terminal) };
    std::thread::sleep(Duration::from_millis(20));
    let mut empty = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut empty) },
        ML_STATUS_EMPTY
    );
    unsafe { ml_core_destroy(core) };
}

struct WorkerWakeProbe {
    core: usize,
    out_request_id: *const u64,
    completed: mpsc::Sender<(u64, u64, ml_event_kind_t)>,
}

unsafe impl Send for WorkerWakeProbe {}
unsafe impl Sync for WorkerWakeProbe {}

unsafe extern "C" fn poll_and_destroy_from_worker(context: *mut std::ffi::c_void) {
    let probe = unsafe { &*(context.cast::<WorkerWakeProbe>()) };
    let core = ptr::without_provenance_mut(probe.core);
    let published_request_id = unsafe { *probe.out_request_id };
    let mut completed = event();
    let status = unsafe { ml_core_poll_event(core, &mut completed) };
    if status == ML_STATUS_OK {
        let event_request_id = completed.request_id;
        let event_kind = completed.kind;
        unsafe { ml_core_release_event(core, &mut completed) };
        unsafe { ml_core_destroy(core) };
        let _ = probe
            .completed
            .send((published_request_id, event_request_id, event_kind));
    } else {
        unsafe { ml_core_destroy(core) };
    }
}

#[test]
fn worker_waker_observes_the_request_id_and_can_destroy_the_core() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir), &mut core) },
        ML_STATUS_OK
    );
    let revision = ready_revision(core);

    let (completed, completed_receiver) = mpsc::channel();
    let mut request_id = 0;
    let probe = Box::new(WorkerWakeProbe {
        core: core.addr(),
        out_request_id: &raw const request_id,
        completed,
    });
    assert_eq!(
        unsafe {
            ml_core_set_waker(
                core,
                Some(poll_and_destroy_from_worker),
                (&raw const *probe).cast_mut().cast(),
            )
        },
        ML_STATUS_OK
    );
    let request = ml_library_course_page_request_v1 {
        struct_size: size_of::<ml_library_course_page_request_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        expected_revision: revision,
        offset: 0,
        limit: 20,
        reserved: 0,
    };
    assert_eq!(
        unsafe { ml_library_course_page_v1(core, &request, &mut request_id) },
        ML_STATUS_OK
    );
    let (published_request_id, event_request_id, event_kind) = completed_receiver
        .recv_timeout(Duration::from_secs(2))
        .expect("worker waker must poll and destroy without deadlock");
    assert_eq!(published_request_id, request_id);
    assert_eq!(event_request_id, request_id);
    assert_eq!(event_kind, ML_EVENT_LIBRARY_COURSE_PAGE);
    let mut stale = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut stale) },
        ML_STATUS_INVALID_HANDLE
    );
    drop(probe);
}
