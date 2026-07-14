use std::mem::{align_of, offset_of, size_of};
use std::path::Path;
use std::ptr;
use std::sync::mpsc;
use std::time::{Duration, Instant};

use melearner_core::*;

fn config_bytes(state_dir: &[u8]) -> ml_config_v2 {
    ml_config_v2 {
        struct_size: size_of::<ml_config_v2>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 4,
        max_event_payload_bytes: 1024,
        state_dir: state_dir.as_ptr(),
        state_dir_len: state_dir.len(),
    }
}

fn config(state_dir: &Path) -> ml_config_v2 {
    let state_dir = state_dir
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    config_bytes(state_dir)
}

fn assert_config_rejected(config: ml_config_v2) {
    let mut core = ptr::dangling_mut();
    assert_eq!(
        unsafe { ml_core_create(&config, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert!(core.is_null());
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

fn limits() -> ml_core_limits_v1 {
    ml_core_limits_v1 {
        struct_size: size_of::<ml_core_limits_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 0,
        max_event_payload_bytes: 0,
    }
}

fn poll_until_event(core: *mut ml_core_t) -> ml_event_v1 {
    let mut next = event();
    poll_until_event_into(core, &mut next);
    next
}

fn poll_until_event_into(core: *mut ml_core_t, next: &mut ml_event_v1) {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        match unsafe { ml_core_poll_event(core, next) } {
            ML_STATUS_OK => return,
            ML_STATUS_EMPTY if Instant::now() < deadline => std::thread::yield_now(),
            status => panic!("event did not arrive: status {status}"),
        }
    }
}

fn ready_revision(ready: &ml_event_v1) -> u64 {
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
    revision
}

#[test]
fn public_v2_config_copies_the_current_state_directory() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let previous_database = data_dir.path().join("melearner.db");
    std::fs::write(&previous_database, b"previous database sentinel")
        .expect("write previous database sentinel");
    let mut state_dir = data_dir
        .path()
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes()
        .to_vec();
    let config = ml_config_v2 {
        struct_size: size_of::<ml_config_v2>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 4,
        max_event_payload_bytes: 1024,
        state_dir: state_dir.as_ptr(),
        state_dir_len: state_dir.len(),
    };

    assert_eq!(ML_ABI_VERSION, 2);
    let mut core = ptr::null_mut();
    assert_eq!(unsafe { ml_core_create(&config, &mut core) }, ML_STATUS_OK);
    state_dir.fill(b'x');

    let mut ready = poll_until_event(core);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    unsafe { ml_core_release_event(core, &mut ready) };
    unsafe { ml_core_destroy(core) };

    assert!(data_dir.path().join("melearner-native.sqlite3").is_file());
    assert_eq!(
        std::fs::read(previous_database).expect("read previous database sentinel"),
        b"previous database sentinel"
    );
}

#[test]
fn public_v2_requires_valid_state_directory_bytes() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");

    let mut null_state_dir = config(data_dir.path());
    null_state_dir.state_dir = ptr::null();
    assert_config_rejected(null_state_dir);

    let mut empty_state_dir = config(data_dir.path());
    empty_state_dir.state_dir = ptr::dangling();
    empty_state_dir.state_dir_len = 0;
    assert_config_rejected(empty_state_dir);

    let mut oversized_state_dir = config(data_dir.path());
    oversized_state_dir.state_dir = ptr::dangling();
    oversized_state_dir.state_dir_len = ML_MAX_EVENT_PAYLOAD_BYTES as usize + 1;
    assert_config_rejected(oversized_state_dir);

    assert_config_rejected(config_bytes(b"state\0directory"));
    assert_config_rejected(config_bytes(&[0xff]));
}

#[test]
fn public_v2_creates_a_current_database_in_a_unicode_state_directory() {
    let root = tempfile::tempdir().expect("create Unicode ABI root directory");
    let state_dir = root.path().join("学习-日本語-শিক্ষা");
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(&state_dir), &mut core) },
        ML_STATUS_OK
    );

    let mut ready = poll_until_event(core);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    unsafe { ml_core_release_event(core, &mut ready) };
    unsafe { ml_core_destroy(core) };

    assert!(state_dir.join("melearner-native.sqlite3").is_file());
}

#[test]
fn public_v2_layout_is_pinned() {
    assert_eq!(size_of::<ml_config_v2>(), 16 + size_of::<usize>() * 2);
    assert_eq!(align_of::<ml_config_v2>(), align_of::<usize>());
    assert_eq!(offset_of!(ml_config_v2, struct_size), 0);
    assert_eq!(offset_of!(ml_config_v2, abi_version), 4);
    assert_eq!(offset_of!(ml_config_v2, event_queue_capacity), 8);
    assert_eq!(offset_of!(ml_config_v2, max_event_payload_bytes), 12);
    assert_eq!(offset_of!(ml_config_v2, state_dir), 16);
    assert_eq!(
        offset_of!(ml_config_v2, state_dir_len),
        16 + size_of::<usize>()
    );

    assert_eq!(size_of::<ml_library_course_page_request_v1>(), 32);
    assert_eq!(
        align_of::<ml_library_course_page_request_v1>(),
        align_of::<u64>()
    );
    assert_eq!(
        offset_of!(ml_library_course_page_request_v1, struct_size),
        0
    );
    assert_eq!(
        offset_of!(ml_library_course_page_request_v1, abi_version),
        4
    );
    assert_eq!(
        offset_of!(ml_library_course_page_request_v1, expected_revision),
        8
    );
    assert_eq!(offset_of!(ml_library_course_page_request_v1, offset), 16);
    assert_eq!(offset_of!(ml_library_course_page_request_v1, limit), 24);
    assert_eq!(offset_of!(ml_library_course_page_request_v1, reserved), 28);

    assert_eq!(
        size_of::<ml_library_lesson_page_request_v1>(),
        32 + size_of::<usize>() * 4
    );
    assert_eq!(
        align_of::<ml_library_lesson_page_request_v1>(),
        align_of::<u64>().max(align_of::<usize>())
    );
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, struct_size),
        0
    );
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, abi_version),
        4
    );
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, expected_revision),
        8
    );
    assert_eq!(offset_of!(ml_library_lesson_page_request_v1, offset), 16);
    assert_eq!(offset_of!(ml_library_lesson_page_request_v1, limit), 24);
    assert_eq!(offset_of!(ml_library_lesson_page_request_v1, reserved), 28);
    assert_eq!(offset_of!(ml_library_lesson_page_request_v1, course_id), 32);
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, course_id_len),
        32 + size_of::<usize>()
    );
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, section_id),
        32 + size_of::<usize>() * 2
    );
    assert_eq!(
        offset_of!(ml_library_lesson_page_request_v1, section_id_len),
        32 + size_of::<usize>() * 3
    );

    assert_eq!(size_of::<ml_core_limits_v1>(), 16);
    assert_eq!(align_of::<ml_core_limits_v1>(), 4);
    assert_eq!(offset_of!(ml_core_limits_v1, struct_size), 0);
    assert_eq!(offset_of!(ml_core_limits_v1, abi_version), 4);
    assert_eq!(offset_of!(ml_core_limits_v1, event_queue_capacity), 8);
    assert_eq!(offset_of!(ml_core_limits_v1, max_event_payload_bytes), 12);

    assert_eq!(offset_of!(ml_event_v1, struct_size), 0);
    assert_eq!(offset_of!(ml_event_v1, abi_version), 4);
    assert_eq!(offset_of!(ml_event_v1, sequence), 8);
    assert_eq!(offset_of!(ml_event_v1, request_id), 16);
    assert_eq!(offset_of!(ml_event_v1, kind), 24);
    assert_eq!(offset_of!(ml_event_v1, status), 28);
    assert_eq!(offset_of!(ml_event_v1, payload_schema_version), 32);
    assert_eq!(offset_of!(ml_event_v1, reserved), 36);
    assert_eq!(offset_of!(ml_event_v1, payload), 40);
    assert_eq!(
        offset_of!(ml_event_v1, payload_len),
        40 + size_of::<usize>()
    );
    assert_eq!(size_of::<ml_event_v1>(), 40 + size_of::<usize>() * 2);
}

#[test]
fn public_abi_creates_polls_releases_and_rejects_stale_handles() {
    assert_eq!(ml_abi_version(), ML_ABI_VERSION);
    let data_dir = tempfile::tempdir().expect("create ABI state directory");

    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(data_dir.path()), &mut core) },
        ML_STATUS_OK
    );
    assert!(!core.is_null());

    let mut ready = poll_until_event(core);
    assert!(ready.sequence > 0);
    assert_eq!(ready.request_id, 0);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    assert_eq!(ready.status, ML_STATUS_OK);
    assert_eq!(ready.payload_schema_version, 1);
    assert!(!ready.payload.is_null());
    ready_revision(&ready);

    unsafe { ml_core_release_event(core, &mut ready) };
    assert_eq!(ready.struct_size, size_of::<ml_event_v1>() as u32);
    assert_eq!(ready.abi_version, ML_ABI_VERSION);
    assert_eq!(ready.kind, 0);
    assert!(ready.payload.is_null());
    assert_eq!(ready.payload_len, 0);
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut ready) },
        ML_STATUS_EMPTY
    );

    unsafe { ml_core_destroy(core) };
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut ready) },
        ML_STATUS_INVALID_HANDLE
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn capacity_one_startup_emits_fatal_for_a_noncurrent_native_database() {
    let data_dir = tempfile::tempdir().expect("create failed-startup ABI state directory");
    std::fs::File::create(data_dir.path().join("melearner-native.sqlite3"))
        .expect("create noncurrent native database");
    let mut startup_config = config(data_dir.path());
    startup_config.event_queue_capacity = 1;
    startup_config.max_event_payload_bytes = 1;
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&startup_config, &mut core) },
        ML_STATUS_OK
    );

    let mut fatal = poll_until_event(core);
    assert_eq!(fatal.request_id, 0);
    assert_eq!(fatal.kind, ML_EVENT_FATAL);
    assert_eq!(fatal.status, ML_STATUS_FAILED);
    assert_eq!(fatal.payload_schema_version, 1);
    assert!(!fatal.payload.is_null());
    assert_eq!(fatal.payload_len, 1);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(fatal.payload, fatal.payload_len) },
        b"0"
    );
    unsafe { ml_core_release_event(core, &mut fatal) };

    let mut empty = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut empty) },
        ML_STATUS_EMPTY
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn public_abi_returns_limits_and_accepts_prefix_only_event_initialization() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(data_dir.path()), &mut core) },
        ML_STATUS_OK
    );

    let mut returned_limits = limits();
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut returned_limits) },
        ML_STATUS_OK
    );
    assert_eq!(returned_limits.event_queue_capacity, 4);
    assert_eq!(returned_limits.max_event_payload_bytes, 1024);

    let mut ready = ml_event_v1 {
        struct_size: size_of::<ml_event_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        sequence: u64::MAX,
        request_id: u64::MAX,
        kind: u32::MAX,
        status: u32::MAX,
        payload_schema_version: u32::MAX,
        reserved: u32::MAX,
        payload: ptr::dangling(),
        payload_len: usize::MAX,
    };
    poll_until_event_into(core, &mut ready);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    unsafe { ml_core_release_event(core, &mut ready) };

    let mut too_small = limits();
    too_small.struct_size -= 1;
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut too_small) },
        ML_STATUS_ABI_MISMATCH
    );
    let mut wrong_version = limits();
    wrong_version.abi_version += 1;
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut wrong_version) },
        ML_STATUS_ABI_MISMATCH
    );
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    assert_eq!(ml_core_cancel(core, 0), ML_STATUS_INVALID_ARGUMENT);
    assert_eq!(ml_core_cancel(core, 42), ML_STATUS_NOT_FOUND);
    assert_eq!(
        unsafe { ml_core_set_waker(core, None, ptr::dangling_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(
        unsafe { ml_core_set_waker(core, None, ptr::null_mut()) },
        ML_STATUS_OK
    );

    unsafe { ml_core_destroy(core) };
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut returned_limits) },
        ML_STATUS_INVALID_HANDLE
    );
}

#[test]
fn public_abi_rejects_null_and_incompatible_structs() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let mut core = ptr::dangling_mut();
    assert_eq!(
        unsafe { ml_core_create(ptr::null(), &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert!(core.is_null());
    assert_eq!(
        unsafe { ml_core_create(&config(data_dir.path()), ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut too_small = config(data_dir.path());
    too_small.struct_size -= 1;
    assert_eq!(
        unsafe { ml_core_create(&too_small, &mut core) },
        ML_STATUS_ABI_MISMATCH
    );
    assert!(core.is_null());

    let mut wrong_version = config(data_dir.path());
    wrong_version.abi_version += 1;
    assert_eq!(
        unsafe { ml_core_create(&wrong_version, &mut core) },
        ML_STATUS_ABI_MISMATCH
    );
    assert!(core.is_null());

    let mut zero_queue = config(data_dir.path());
    zero_queue.event_queue_capacity = 0;
    assert_eq!(
        unsafe { ml_core_create(&zero_queue, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut zero_payload = config(data_dir.path());
    zero_payload.max_event_payload_bytes = 0;
    assert_eq!(
        unsafe { ml_core_create(&zero_payload, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut oversized_queue = config(data_dir.path());
    oversized_queue.event_queue_capacity = ML_MAX_EVENT_QUEUE_CAPACITY + 1;
    assert_eq!(
        unsafe { ml_core_create(&oversized_queue, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut oversized_payload = config(data_dir.path());
    oversized_payload.max_event_payload_bytes = ML_MAX_EVENT_PAYLOAD_BYTES + 1;
    assert_eq!(
        unsafe { ml_core_create(&oversized_payload, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );
}

#[test]
fn larger_v2_config_prefix_is_forward_compatible() {
    #[repr(C)]
    struct ExtendedConfig {
        prefix: ml_config_v2,
        future_value: u64,
    }

    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let mut extended = ExtendedConfig {
        prefix: config(data_dir.path()),
        future_value: 0xfeed_face_cafe_beef,
    };
    extended.prefix.struct_size = size_of::<ExtendedConfig>() as u32;
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&extended.prefix, &mut core) },
        ML_STATUS_OK
    );
    assert!(!core.is_null());
    unsafe { ml_core_destroy(core) };
}

#[test]
fn events_can_only_be_released_by_their_owning_core() {
    let first_data_dir = tempfile::tempdir().expect("create first ABI state directory");
    let second_data_dir = tempfile::tempdir().expect("create second ABI state directory");
    let mut first = ptr::null_mut();
    let mut second = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(first_data_dir.path()), &mut first) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { ml_core_create(&config(second_data_dir.path()), &mut second) },
        ML_STATUS_OK
    );

    let mut first_event = poll_until_event(first);
    let mut second_event = poll_until_event(second);
    assert_ne!(first_event.sequence, second_event.sequence);

    unsafe { ml_core_release_event(second, &mut first_event) };
    assert_ne!(first_event.sequence, 0);
    unsafe { ml_core_release_event(first, &mut first_event) };
    assert_eq!(first_event.sequence, 0);
    unsafe { ml_core_release_event(first, &mut first_event) };
    unsafe { ml_core_release_event(second, &mut second_event) };
    unsafe { ml_core_release_event(second, ptr::null_mut()) };

    unsafe { ml_core_destroy(first) };
    unsafe { ml_core_destroy(second) };
}

#[test]
fn lifecycle_handles_are_unique_across_destroy_cycles() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let mut previous = ptr::null_mut();
    for _ in 0..64 {
        let mut core = ptr::null_mut();
        assert_eq!(
            unsafe { ml_core_create(&config(data_dir.path()), &mut core) },
            ML_STATUS_OK
        );
        assert!(!core.is_null());
        assert_ne!(core, previous);
        unsafe { ml_core_destroy(core) };
        previous = core;
    }
}

#[test]
fn immediate_destroy_releases_the_database_for_reopen() {
    let data_dir = tempfile::tempdir().expect("create ABI state directory");
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(data_dir.path()), &mut core) },
        ML_STATUS_OK
    );

    let core_address = core.addr();
    let (destroyed_sender, destroyed_receiver) = mpsc::channel();
    let destroyer = std::thread::spawn(move || {
        unsafe { ml_core_destroy(ptr::without_provenance_mut(core_address)) };
        destroyed_sender
            .send(())
            .expect("report completed core destruction");
    });
    destroyed_receiver
        .recv_timeout(Duration::from_secs(2))
        .expect("core destruction completes promptly");
    destroyer.join().expect("core destruction does not panic");

    let mut replacement = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(data_dir.path()), &mut replacement) },
        ML_STATUS_OK
    );
    let mut ready = poll_until_event(replacement);
    ready_revision(&ready);
    unsafe { ml_core_release_event(replacement, &mut ready) };
    unsafe { ml_core_destroy(replacement) };
}
