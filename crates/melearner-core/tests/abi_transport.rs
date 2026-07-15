#![cfg(feature = "abi-test-hooks")]

use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Barrier, mpsc};
use std::thread;
use std::time::{Duration, Instant};

use melearner_core::*;

fn config(state_dir: &Path, capacity: u32, max_payload: u32) -> ml_config_v2 {
    let state_dir = state_dir
        .to_str()
        .expect("temporary state directory is UTF-8")
        .as_bytes();
    ml_config_v2 {
        struct_size: size_of::<ml_config_v2>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: capacity,
        max_event_payload_bytes: max_payload,
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

fn create_empty(capacity: u32, max_payload: u32) -> (tempfile::TempDir, *mut ml_core_t) {
    let state_dir = tempfile::tempdir().expect("create transport state directory");
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(state_dir.path(), capacity, max_payload), &mut core) },
        ML_STATUS_OK
    );
    let mut ready = poll(core);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    unsafe { ml_core_release_event(core, &mut ready) };
    (state_dir, core)
}

fn poll(core: *mut ml_core_t) -> ml_event_v1 {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let mut next = event();
        match unsafe { ml_core_poll_event(core, &mut next) } {
            ML_STATUS_OK => return next,
            ML_STATUS_EMPTY if Instant::now() < deadline => thread::yield_now(),
            status => panic!("event did not arrive: status {status}"),
        }
    }
}

#[test]
fn borrowed_input_is_copied_and_outstanding_events_apply_backpressure() {
    let (_state_dir, core) = create_empty(2, ML_MIN_EVENT_PAYLOAD_BYTES);
    let mut malformed_request = u64::MAX;
    assert_eq!(
        unsafe { ml_core_test_submit(core, ptr::null(), 1, &mut malformed_request) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(malformed_request, 0);
    malformed_request = u64::MAX;
    assert_eq!(
        unsafe { ml_core_test_submit(core, [0xff].as_ptr(), 1, &mut malformed_request) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(malformed_request, 0);
    assert_eq!(
        unsafe { ml_core_test_submit(core, ptr::null(), 0, ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );
    let mut first_input = b"first".to_vec();
    let mut first_request = 0;
    assert_eq!(
        unsafe {
            ml_core_test_submit(
                core,
                first_input.as_ptr(),
                first_input.len(),
                &mut first_request,
            )
        },
        ML_STATUS_OK
    );
    first_input.fill(b'x');

    let mut second_request = 0;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"two".as_ptr(), 3, &mut second_request) },
        ML_STATUS_OK
    );
    let mut rejected_request = u64::MAX;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"no".as_ptr(), 2, &mut rejected_request) },
        ML_STATUS_BUSY
    );
    assert_eq!(rejected_request, 0);

    assert_eq!(ml_core_test_complete(core, first_request), ML_STATUS_OK);
    let mut first_event = poll(core);
    assert_eq!(first_event.request_id, first_request);
    assert_eq!(first_event.status, ML_STATUS_OK);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(first_event.payload, first_event.payload_len) },
        b"first"
    );

    assert_eq!(
        unsafe { ml_core_test_submit(core, b"no".as_ptr(), 2, &mut rejected_request) },
        ML_STATUS_BUSY
    );
    assert_eq!(ml_core_cancel(core, second_request), ML_STATUS_OK);
    assert_eq!(
        ml_core_test_complete(core, second_request),
        ML_STATUS_NOT_FOUND
    );

    unsafe { ml_core_release_event(core, &mut first_event) };
    assert!(first_event.payload.is_null());
    assert_eq!(first_event.payload_len, 0);

    let mut third_request = 0;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"three".as_ptr(), 5, &mut third_request) },
        ML_STATUS_OK
    );
    let mut cancelled = poll(core);
    assert_eq!(cancelled.request_id, second_request);
    assert_eq!(cancelled.kind, ML_EVENT_REQUEST_CANCELLED);
    assert_eq!(cancelled.status, ML_STATUS_CANCELLED);
    assert_eq!(cancelled.payload_schema_version, 0);
    assert!(cancelled.payload.is_null());
    assert_eq!(cancelled.payload_len, 0);
    unsafe { ml_core_release_event(core, &mut cancelled) };

    assert_eq!(ml_core_cancel(core, third_request), ML_STATUS_OK);
    assert_eq!(ml_core_cancel(core, third_request), ML_STATUS_NOT_FOUND);
    let mut third_cancelled = poll(core);
    unsafe { ml_core_release_event(core, &mut third_cancelled) };

    let mut oversized_request = 0;
    assert_eq!(
        unsafe {
            ml_core_test_submit(
                core,
                [b'x'; ML_MIN_EVENT_PAYLOAD_BYTES as usize + 1].as_ptr(),
                ML_MIN_EVENT_PAYLOAD_BYTES as usize + 1,
                &mut oversized_request,
            )
        },
        ML_STATUS_INVALID_ARGUMENT
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn cancellation_and_completion_race_to_one_terminal_event() {
    let (_state_dir, core) = create_empty(2, ML_MIN_EVENT_PAYLOAD_BYTES);
    let mut request_id = 0;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"race".as_ptr(), 4, &mut request_id) },
        ML_STATUS_OK
    );

    let barrier = Arc::new(Barrier::new(3));
    let cancel_barrier = Arc::clone(&barrier);
    let complete_barrier = Arc::clone(&barrier);
    let handle = core.addr();
    let cancel = thread::spawn(move || {
        cancel_barrier.wait();
        ml_core_cancel(ptr::without_provenance_mut(handle), request_id)
    });
    let handle = core.addr();
    let complete = thread::spawn(move || {
        complete_barrier.wait();
        ml_core_test_complete(ptr::without_provenance_mut(handle), request_id)
    });
    barrier.wait();

    let statuses = [
        cancel.join().expect("cancel thread"),
        complete.join().expect("complete thread"),
    ];
    assert_eq!(
        statuses
            .iter()
            .filter(|&&status| status == ML_STATUS_OK)
            .count(),
        1
    );
    assert_eq!(
        statuses
            .iter()
            .filter(|&&status| status == ML_STATUS_NOT_FOUND)
            .count(),
        1
    );

    let mut terminal = poll(core);
    assert_eq!(terminal.request_id, request_id);
    unsafe { ml_core_release_event(core, &mut terminal) };
    let mut empty = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut empty) },
        ML_STATUS_EMPTY
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn multiple_in_flight_payloads_remain_valid_until_each_release() {
    let (_state_dir, core) = create_empty(3, ML_MIN_EVENT_PAYLOAD_BYTES);
    let mut first_request = 0;
    let mut second_request = 0;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"first".as_ptr(), 5, &mut first_request) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"second".as_ptr(), 6, &mut second_request) },
        ML_STATUS_OK
    );
    assert_eq!(ml_core_test_complete(core, first_request), ML_STATUS_OK);
    assert_eq!(ml_core_test_complete(core, second_request), ML_STATUS_OK);

    let mut first = poll(core);
    let mut second = poll(core);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(first.payload, first.payload_len) },
        b"first"
    );
    assert_eq!(
        unsafe { std::slice::from_raw_parts(second.payload, second.payload_len) },
        b"second"
    );

    let mut third_request = 0;
    assert_eq!(
        unsafe { ml_core_test_submit(core, b"third".as_ptr(), 5, &mut third_request) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { std::slice::from_raw_parts(first.payload, first.payload_len) },
        b"first"
    );
    assert_eq!(
        unsafe { std::slice::from_raw_parts(second.payload, second.payload_len) },
        b"second"
    );

    unsafe { ml_core_release_event(core, &mut first) };
    unsafe { ml_core_release_event(core, &mut second) };
    assert_eq!(ml_core_cancel(core, third_request), ML_STATUS_OK);
    let mut cancelled = poll(core);
    unsafe { ml_core_release_event(core, &mut cancelled) };
    unsafe { ml_core_destroy(core) };
}

#[test]
fn panic_marks_the_core_failed_and_emits_one_fatal_event_when_space_is_released() {
    let state_dir = tempfile::tempdir().expect("create panic transport state directory");
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe {
            ml_core_create(
                &config(state_dir.path(), 1, ML_MIN_EVENT_PAYLOAD_BYTES),
                &mut core,
            )
        },
        ML_STATUS_OK
    );

    let mut ready = poll(core);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    assert_eq!(ml_core_test_panic(core), ML_STATUS_PANIC);
    assert_eq!(ml_core_cancel(core, 1), ML_STATUS_FAILED);
    assert_eq!(ml_core_test_panic(core), ML_STATUS_FAILED);

    let mut empty = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut empty) },
        ML_STATUS_EMPTY
    );
    unsafe { ml_core_release_event(core, &mut ready) };

    let mut fatal = poll(core);
    assert_eq!(fatal.kind, ML_EVENT_FATAL);
    assert_eq!(fatal.status, ML_STATUS_FAILED);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(fatal.payload, fatal.payload_len) },
        b"0"
    );
    unsafe { ml_core_release_event(core, &mut fatal) };
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut empty) },
        ML_STATUS_EMPTY
    );

    let mut limits = ml_core_limits_v1 {
        struct_size: size_of::<ml_core_limits_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 0,
        max_event_payload_bytes: 0,
    };
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut limits) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { ml_core_set_waker(core, None, ptr::null_mut()) },
        ML_STATUS_OK
    );
    unsafe { ml_core_destroy(core) };
    unsafe { ml_core_destroy(core) };
}

struct ReentrantProbe {
    core: AtomicUsize,
    calls: AtomicUsize,
    mode: AtomicU32,
    status: AtomicU32,
}

unsafe extern "C" fn reentrant_wake(context: *mut c_void) {
    let probe = unsafe { &*(context.cast::<ReentrantProbe>()) };
    let core = ptr::without_provenance_mut::<ml_core_t>(probe.core.load(Ordering::Acquire));
    let status = match probe.mode.load(Ordering::Acquire) {
        0 => {
            let mut limits = ml_core_limits_v1 {
                struct_size: size_of::<ml_core_limits_v1>() as u32,
                abi_version: ML_ABI_VERSION,
                event_queue_capacity: 0,
                max_event_payload_bytes: 0,
            };
            unsafe { ml_core_get_limits_v1(core, &mut limits) }
        }
        1 => unsafe { ml_core_set_waker(core, None, ptr::null_mut()) },
        2 => {
            unsafe { ml_core_destroy(core) };
            ML_STATUS_OK
        }
        _ => ML_STATUS_INVALID_ARGUMENT,
    };
    probe.status.store(status, Ordering::Release);
    probe.calls.fetch_add(1, Ordering::AcqRel);
}

#[test]
fn concurrent_empty_to_nonempty_transition_wakes_once_outside_core_locks() {
    let (_state_dir, core) = create_empty(32, ML_MIN_EVENT_PAYLOAD_BYTES);
    let probe = Box::new(ReentrantProbe {
        core: AtomicUsize::new(core.addr()),
        calls: AtomicUsize::new(0),
        mode: AtomicU32::new(0),
        status: AtomicU32::new(u32::MAX),
    });
    let context = (&*probe as *const ReentrantProbe).cast_mut().cast();
    assert_eq!(
        unsafe { ml_core_set_waker(core, Some(reentrant_wake), context) },
        ML_STATUS_OK
    );

    let barrier = Arc::new(Barrier::new(17));
    let mut threads = Vec::new();
    for _ in 0..16 {
        let barrier = Arc::clone(&barrier);
        let handle = core.addr();
        threads.push(thread::spawn(move || {
            barrier.wait();
            ml_core_test_emit(ptr::without_provenance_mut(handle))
        }));
    }
    barrier.wait();
    for thread in threads {
        assert_eq!(thread.join().expect("emit thread"), ML_STATUS_OK);
    }
    assert_eq!(probe.calls.load(Ordering::Acquire), 1);
    assert_eq!(probe.status.load(Ordering::Acquire), ML_STATUS_OK);

    let mut previous_sequence = 0;
    for _ in 0..16 {
        let mut emitted = poll(core);
        assert!(emitted.sequence > previous_sequence);
        previous_sequence = emitted.sequence;
        unsafe { ml_core_release_event(core, &mut emitted) };
    }
    assert_eq!(ml_core_test_emit(core), ML_STATUS_OK);
    assert_eq!(probe.calls.load(Ordering::Acquire), 2);
    let mut emitted = poll(core);
    unsafe { ml_core_release_event(core, &mut emitted) };

    assert_eq!(
        unsafe { ml_core_set_waker(core, None, ptr::null_mut()) },
        ML_STATUS_OK
    );
    unsafe { ml_core_destroy(core) };
}

#[test]
fn active_callback_can_clear_itself_or_destroy_its_core() {
    let (_clear_state_dir, core) = create_empty(4, ML_MIN_EVENT_PAYLOAD_BYTES);
    let clear_probe = Box::new(ReentrantProbe {
        core: AtomicUsize::new(core.addr()),
        calls: AtomicUsize::new(0),
        mode: AtomicU32::new(1),
        status: AtomicU32::new(u32::MAX),
    });
    let context = (&*clear_probe as *const ReentrantProbe).cast_mut().cast();
    assert_eq!(
        unsafe { ml_core_set_waker(core, Some(reentrant_wake), context) },
        ML_STATUS_OK
    );
    assert_eq!(ml_core_test_emit(core), ML_STATUS_OK);
    assert_eq!(clear_probe.calls.load(Ordering::Acquire), 1);
    assert_eq!(clear_probe.status.load(Ordering::Acquire), ML_STATUS_OK);
    assert_eq!(ml_core_test_emit(core), ML_STATUS_OK);
    assert_eq!(clear_probe.calls.load(Ordering::Acquire), 1);
    for _ in 0..2 {
        let mut emitted = poll(core);
        unsafe { ml_core_release_event(core, &mut emitted) };
    }
    unsafe { ml_core_destroy(core) };

    let (_destroy_state_dir, core) = create_empty(4, ML_MIN_EVENT_PAYLOAD_BYTES);
    let destroy_probe = Box::new(ReentrantProbe {
        core: AtomicUsize::new(core.addr()),
        calls: AtomicUsize::new(0),
        mode: AtomicU32::new(2),
        status: AtomicU32::new(u32::MAX),
    });
    let context = (&*destroy_probe as *const ReentrantProbe).cast_mut().cast();
    assert_eq!(
        unsafe { ml_core_set_waker(core, Some(reentrant_wake), context) },
        ML_STATUS_OK
    );
    assert_eq!(ml_core_test_emit(core), ML_STATUS_OK);
    assert_eq!(destroy_probe.calls.load(Ordering::Acquire), 1);
    assert_eq!(destroy_probe.status.load(Ordering::Acquire), ML_STATUS_OK);
    let mut limits = ml_core_limits_v1 {
        struct_size: size_of::<ml_core_limits_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 0,
        max_event_payload_bytes: 0,
    };
    assert_eq!(
        unsafe { ml_core_get_limits_v1(core, &mut limits) },
        ML_STATUS_INVALID_HANDLE
    );
}

struct BlockingProbe {
    entered: Barrier,
    release: Barrier,
    called: AtomicBool,
}

unsafe extern "C" fn blocking_wake(context: *mut c_void) {
    let probe = unsafe { &*(context.cast::<BlockingProbe>()) };
    probe.called.store(true, Ordering::Release);
    probe.entered.wait();
    probe.release.wait();
}

#[test]
fn clearing_a_waker_waits_for_an_active_callback() {
    let (_state_dir, core) = create_empty(4, ML_MIN_EVENT_PAYLOAD_BYTES);
    let probe = Box::new(BlockingProbe {
        entered: Barrier::new(2),
        release: Barrier::new(2),
        called: AtomicBool::new(false),
    });
    let context = (&*probe as *const BlockingProbe).cast_mut().cast();
    assert_eq!(
        unsafe { ml_core_set_waker(core, Some(blocking_wake), context) },
        ML_STATUS_OK
    );

    let handle = core.addr();
    let emitter = thread::spawn(move || ml_core_test_emit(ptr::without_provenance_mut(handle)));
    probe.entered.wait();
    assert!(probe.called.load(Ordering::Acquire));

    let (tx, rx) = mpsc::channel();
    let handle = core.addr();
    let clearer = thread::spawn(move || {
        let status = unsafe {
            ml_core_set_waker(ptr::without_provenance_mut(handle), None, ptr::null_mut())
        };
        tx.send(status).expect("clear status receiver");
    });
    assert!(rx.recv_timeout(Duration::from_millis(50)).is_err());
    probe.release.wait();
    assert_eq!(rx.recv().expect("clear status"), ML_STATUS_OK);
    assert_eq!(emitter.join().expect("emitter"), ML_STATUS_OK);
    clearer.join().expect("clearer");

    let mut emitted = poll(core);
    unsafe { ml_core_release_event(core, &mut emitted) };
    unsafe { ml_core_destroy(core) };
}
