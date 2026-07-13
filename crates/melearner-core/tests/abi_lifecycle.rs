use std::mem::{align_of, offset_of, size_of};
use std::ptr;

use melearner_core::*;

fn config() -> ml_config_v1 {
    ml_config_v1 {
        struct_size: size_of::<ml_config_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 4,
        max_event_payload_bytes: 1024,
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

fn limits() -> ml_core_limits_v1 {
    ml_core_limits_v1 {
        struct_size: size_of::<ml_core_limits_v1>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 0,
        max_event_payload_bytes: 0,
    }
}

#[test]
fn public_v1_layout_is_pinned() {
    assert_eq!(size_of::<ml_config_v1>(), 16);
    assert_eq!(align_of::<ml_config_v1>(), 4);
    assert_eq!(offset_of!(ml_config_v1, struct_size), 0);
    assert_eq!(offset_of!(ml_config_v1, abi_version), 4);
    assert_eq!(offset_of!(ml_config_v1, event_queue_capacity), 8);
    assert_eq!(offset_of!(ml_config_v1, max_event_payload_bytes), 12);

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

    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(), &mut core) },
        ML_STATUS_OK
    );
    assert!(!core.is_null());

    let mut ready = event();
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut ready) },
        ML_STATUS_OK
    );
    assert!(ready.sequence > 0);
    assert_eq!(ready.request_id, 0);
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    assert_eq!(ready.status, ML_STATUS_OK);
    assert_eq!(ready.payload_schema_version, 1);
    assert!(!ready.payload.is_null());
    assert_eq!(ready.payload_len, 1);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(ready.payload, 1) },
        b"1"
    );

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
fn public_abi_returns_limits_and_accepts_prefix_only_event_initialization() {
    let mut core = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(), &mut core) },
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
    assert_eq!(
        unsafe { ml_core_poll_event(core, &mut ready) },
        ML_STATUS_OK
    );
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
    let mut core = ptr::dangling_mut();
    assert_eq!(
        unsafe { ml_core_create(ptr::null(), &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );
    assert!(core.is_null());
    assert_eq!(
        unsafe { ml_core_create(&config(), ptr::null_mut()) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut too_small = config();
    too_small.struct_size -= 1;
    assert_eq!(
        unsafe { ml_core_create(&too_small, &mut core) },
        ML_STATUS_ABI_MISMATCH
    );
    assert!(core.is_null());

    let mut wrong_version = config();
    wrong_version.abi_version += 1;
    assert_eq!(
        unsafe { ml_core_create(&wrong_version, &mut core) },
        ML_STATUS_ABI_MISMATCH
    );
    assert!(core.is_null());

    let mut zero_queue = config();
    zero_queue.event_queue_capacity = 0;
    assert_eq!(
        unsafe { ml_core_create(&zero_queue, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut zero_payload = config();
    zero_payload.max_event_payload_bytes = 0;
    assert_eq!(
        unsafe { ml_core_create(&zero_payload, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut oversized_queue = config();
    oversized_queue.event_queue_capacity = ML_MAX_EVENT_QUEUE_CAPACITY + 1;
    assert_eq!(
        unsafe { ml_core_create(&oversized_queue, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );

    let mut oversized_payload = config();
    oversized_payload.max_event_payload_bytes = ML_MAX_EVENT_PAYLOAD_BYTES + 1;
    assert_eq!(
        unsafe { ml_core_create(&oversized_payload, &mut core) },
        ML_STATUS_INVALID_ARGUMENT
    );
}

#[test]
fn larger_v1_config_prefix_is_forward_compatible() {
    #[repr(C)]
    struct ExtendedConfig {
        prefix: ml_config_v1,
        future_value: u64,
    }

    let mut extended = ExtendedConfig {
        prefix: config(),
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
    let mut first = ptr::null_mut();
    let mut second = ptr::null_mut();
    assert_eq!(
        unsafe { ml_core_create(&config(), &mut first) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { ml_core_create(&config(), &mut second) },
        ML_STATUS_OK
    );

    let mut first_event = event();
    let mut second_event = event();
    assert_eq!(
        unsafe { ml_core_poll_event(first, &mut first_event) },
        ML_STATUS_OK
    );
    assert_eq!(
        unsafe { ml_core_poll_event(second, &mut second_event) },
        ML_STATUS_OK
    );
    assert!(first_event.sequence < second_event.sequence);

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
    let mut previous = ptr::null_mut();
    for _ in 0..64 {
        let mut core = ptr::null_mut();
        assert_eq!(
            unsafe { ml_core_create(&config(), &mut core) },
            ML_STATUS_OK
        );
        assert!(!core.is_null());
        assert_ne!(core, previous);
        unsafe { ml_core_destroy(core) };
        previous = core;
    }
}
