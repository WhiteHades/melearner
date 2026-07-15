use std::mem::size_of;
use std::ptr;
use std::sync::mpsc;
use std::time::{Duration, Instant};

use melearner_core::*;

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
