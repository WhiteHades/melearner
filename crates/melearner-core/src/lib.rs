#![allow(non_camel_case_types)]

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::mem::size_of;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};

pub const ML_ABI_VERSION: u32 = 1;
pub const ML_MAX_EVENT_QUEUE_CAPACITY: u32 = 65_536;
pub const ML_MAX_EVENT_PAYLOAD_BYTES: u32 = 16 * 1024 * 1024;

pub type ml_status_t = u32;
pub const ML_STATUS_OK: ml_status_t = 0;
pub const ML_STATUS_INVALID_ARGUMENT: ml_status_t = 1;
pub const ML_STATUS_ABI_MISMATCH: ml_status_t = 2;
pub const ML_STATUS_INVALID_HANDLE: ml_status_t = 3;
pub const ML_STATUS_EMPTY: ml_status_t = 4;
pub const ML_STATUS_BUSY: ml_status_t = 5;
pub const ML_STATUS_PANIC: ml_status_t = 6;
pub const ML_STATUS_CANCELLED: ml_status_t = 7;
pub const ML_STATUS_FAILED: ml_status_t = 8;
pub const ML_STATUS_NOT_FOUND: ml_status_t = 9;

pub type ml_event_kind_t = u32;
pub const ML_EVENT_CORE_READY: ml_event_kind_t = 1;
pub const ML_EVENT_REQUEST_CANCELLED: ml_event_kind_t = 2;
pub const ML_EVENT_FATAL: ml_event_kind_t = 3;

pub type ml_wake_fn = Option<unsafe extern "C" fn(context: *mut c_void)>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_config_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub event_queue_capacity: u32,
    pub max_event_payload_bytes: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_core_limits_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub event_queue_capacity: u32,
    pub max_event_payload_bytes: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_event_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub sequence: u64,
    pub request_id: u64,
    pub kind: ml_event_kind_t,
    pub status: ml_status_t,
    pub payload_schema_version: u32,
    pub reserved: u32,
    pub payload: *const u8,
    pub payload_len: usize,
}

pub struct ml_core_t {
    _private: (),
}

struct OwnedEvent {
    sequence: u64,
    request_id: u64,
    kind: ml_event_kind_t,
    status: ml_status_t,
    payload_schema_version: u32,
    payload: Vec<u8>,
}

impl OwnedEvent {
    fn new(
        request_id: u64,
        kind: ml_event_kind_t,
        status: ml_status_t,
        payload_schema_version: u32,
        payload: Vec<u8>,
    ) -> Self {
        Self {
            sequence: next_event_sequence(),
            request_id,
            kind,
            status,
            payload_schema_version,
            payload,
        }
    }
}

struct WakeState {
    active: bool,
    calls: usize,
}

struct WakeRegistration {
    callback: unsafe extern "C" fn(context: *mut c_void),
    context: WakeContext,
    state: Mutex<WakeState>,
    idle: Condvar,
}

struct WakeContext(*mut c_void);

// SAFETY: `ml_core_set_waker` requires the callback and context to be safe to
// invoke from any thread until the registration is cleared or the core dies.
unsafe impl Send for WakeContext {}
unsafe impl Sync for WakeContext {}

impl WakeRegistration {
    fn new(callback: unsafe extern "C" fn(context: *mut c_void), context: *mut c_void) -> Self {
        Self {
            callback,
            context: WakeContext(context),
            state: Mutex::new(WakeState {
                active: true,
                calls: 0,
            }),
            idle: Condvar::new(),
        }
    }

    fn begin(self: &Arc<Self>) -> Option<WakeCall> {
        let mut state = lock(&self.state);
        if !state.active {
            return None;
        }
        state.calls += 1;
        drop(state);
        Some(WakeCall {
            registration: Arc::clone(self),
        })
    }

    fn retire(&self) {
        let mut state = lock(&self.state);
        state.active = false;
        let registration = ptr::from_ref(self).addr();
        let calls_here = ACTIVE_WAKERS.with(|active| {
            active
                .borrow()
                .iter()
                .filter(|&&active| active == registration)
                .count()
        });
        while state.calls > calls_here {
            state = self
                .idle
                .wait(state)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
        }
    }
}

struct WakeCall {
    registration: Arc<WakeRegistration>,
}

thread_local! {
    static ACTIVE_WAKERS: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) };
}

struct ActiveWake {
    registration: usize,
}

impl ActiveWake {
    fn enter(registration: &Arc<WakeRegistration>) -> Self {
        let registration = Arc::as_ptr(registration).addr();
        ACTIVE_WAKERS.with(|active| active.borrow_mut().push(registration));
        Self { registration }
    }
}

impl Drop for ActiveWake {
    fn drop(&mut self) {
        ACTIVE_WAKERS.with(|active| {
            let popped = active.borrow_mut().pop();
            debug_assert_eq!(popped, Some(self.registration));
        });
    }
}

impl WakeCall {
    fn invoke(self) {
        let _active = ActiveWake::enter(&self.registration);
        unsafe { (self.registration.callback)(self.registration.context.0) };
    }
}

impl Drop for WakeCall {
    fn drop(&mut self) {
        let mut state = lock(&self.registration.state);
        state.calls -= 1;
        self.registration.idle.notify_all();
    }
}

struct PendingRequest {
    #[cfg(feature = "abi-test-hooks")]
    payload: Vec<u8>,
}

struct CoreState {
    event_queue_capacity: usize,
    max_event_payload_bytes: usize,
    queued: VecDeque<OwnedEvent>,
    in_flight: HashMap<u64, OwnedEvent>,
    pending_requests: HashMap<u64, PendingRequest>,
    #[cfg(feature = "abi-test-hooks")]
    next_request_id: u64,
    failed: bool,
    fatal_pending: bool,
    fatal_emitted: bool,
    destroyed: bool,
    waker: Option<Arc<WakeRegistration>>,
}

impl CoreState {
    fn new(config: &ml_config_v1) -> Self {
        Self {
            event_queue_capacity: config.event_queue_capacity as usize,
            max_event_payload_bytes: config.max_event_payload_bytes as usize,
            queued: VecDeque::with_capacity(config.event_queue_capacity as usize),
            in_flight: HashMap::new(),
            pending_requests: HashMap::new(),
            #[cfg(feature = "abi-test-hooks")]
            next_request_id: 1,
            failed: false,
            fatal_pending: false,
            fatal_emitted: false,
            destroyed: false,
            waker: None,
        }
    }

    fn outstanding(&self) -> usize {
        self.queued.len() + self.in_flight.len() + self.pending_requests.len()
    }

    fn enqueue_unreserved(&mut self, event: OwnedEvent) -> Result<Option<WakeCall>, ml_status_t> {
        if event.payload.len() > self.max_event_payload_bytes {
            return Err(ML_STATUS_INVALID_ARGUMENT);
        }
        if self.outstanding() >= self.event_queue_capacity {
            return Err(ML_STATUS_BUSY);
        }
        Ok(self.push_event(event))
    }

    fn complete_reserved(&mut self, request_id: u64, event: OwnedEvent) -> Action {
        if self.pending_requests.remove(&request_id).is_none() {
            return Action::status(ML_STATUS_NOT_FOUND);
        }
        debug_assert!(event.payload.len() <= self.max_event_payload_bytes);
        debug_assert!(self.outstanding() < self.event_queue_capacity);
        Action::with_wake(ML_STATUS_OK, self.push_event(event))
    }

    fn push_event(&mut self, event: OwnedEvent) -> Option<WakeCall> {
        let was_empty = self.queued.is_empty();
        self.queued.push_back(event);
        if was_empty {
            self.waker.as_ref().and_then(|waker| waker.begin())
        } else {
            None
        }
    }

    #[cfg(feature = "abi-test-hooks")]
    fn accept_test_request(&mut self, payload: &[u8]) -> Result<u64, ml_status_t> {
        if payload.len() > self.max_event_payload_bytes {
            return Err(ML_STATUS_INVALID_ARGUMENT);
        }
        if self.outstanding() >= self.event_queue_capacity {
            return Err(ML_STATUS_BUSY);
        }
        let request_id = self.next_request_id();
        self.pending_requests.insert(
            request_id,
            PendingRequest {
                payload: payload.to_vec(),
            },
        );
        Ok(request_id)
    }

    #[cfg(feature = "abi-test-hooks")]
    fn next_request_id(&mut self) -> u64 {
        loop {
            let request_id = self.next_request_id;
            self.next_request_id = self.next_request_id.wrapping_add(1).max(1);
            if request_id != 0 && !self.pending_requests.contains_key(&request_id) {
                return request_id;
            }
        }
    }

    fn materialize_fatal(&mut self) -> Option<WakeCall> {
        if !self.fatal_pending || self.outstanding() >= self.event_queue_capacity {
            return None;
        }
        self.fatal_pending = false;
        self.fatal_emitted = true;
        self.push_event(OwnedEvent::new(
            0,
            ML_EVENT_FATAL,
            ML_STATUS_FAILED,
            1,
            b"0".to_vec(),
        ))
    }
}

struct Core {
    state: Mutex<CoreState>,
}

struct Registry {
    next_handle: usize,
    cores: HashMap<usize, Arc<Core>>,
}

impl Registry {
    fn new() -> Self {
        Self {
            next_handle: 1,
            cores: HashMap::new(),
        }
    }

    fn insert(&mut self, core: Arc<Core>) -> usize {
        loop {
            let handle = self.next_handle;
            self.next_handle = self.next_handle.wrapping_add(1).max(1);
            if handle != 0 && !self.cores.contains_key(&handle) {
                self.cores.insert(handle, core);
                return handle;
            }
        }
    }
}

struct Action {
    status: ml_status_t,
    wake: Option<WakeCall>,
    retired_waker: Option<Arc<WakeRegistration>>,
}

impl Action {
    fn status(status: ml_status_t) -> Self {
        Self {
            status,
            wake: None,
            retired_waker: None,
        }
    }

    fn with_wake(status: ml_status_t, wake: Option<WakeCall>) -> Self {
        Self {
            status,
            wake,
            retired_waker: None,
        }
    }

    fn finish(self) -> ml_status_t {
        if let Some(waker) = self.retired_waker {
            waker.retire();
        }
        if let Some(wake) = self.wake {
            wake.invoke();
        }
        self.status
    }
}

fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::new()))
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn handle_id(core: *mut ml_core_t) -> Option<usize> {
    let id = core.addr();
    (id != 0).then_some(id)
}

fn resolve_core(core: *mut ml_core_t) -> Option<Arc<Core>> {
    let handle = handle_id(core)?;
    lock(registry()).cores.get(&handle).cloned()
}

fn ffi_status(action: impl FnOnce() -> ml_status_t) -> ml_status_t {
    catch_unwind(AssertUnwindSafe(action)).unwrap_or(ML_STATUS_PANIC)
}

fn ffi_core_status(
    core: *mut ml_core_t,
    allow_failed: bool,
    action: impl FnOnce(&mut CoreState) -> Action,
) -> ml_status_t {
    let Some(core) = resolve_core(core) else {
        return ML_STATUS_INVALID_HANDLE;
    };
    match catch_unwind(AssertUnwindSafe(|| {
        let mut state = lock(&core.state);
        if state.destroyed {
            return Action::status(ML_STATUS_INVALID_HANDLE);
        }
        if state.failed && !allow_failed {
            return Action::status(ML_STATUS_FAILED);
        }
        action(&mut state)
    })) {
        Ok(result) => result.finish(),
        Err(_) => {
            mark_core_failed(&core);
            ML_STATUS_PANIC
        }
    }
}

fn mark_core_failed(core: &Arc<Core>) {
    let wake = catch_unwind(AssertUnwindSafe(|| {
        let mut state = lock(&core.state);
        if state.destroyed || state.failed {
            return None;
        }
        state.failed = true;
        state.pending_requests.clear();
        if !state.fatal_emitted {
            state.fatal_pending = true;
        }
        state.materialize_fatal()
    }))
    .ok()
    .flatten();
    if let Some(wake) = wake {
        wake.invoke();
    }
}

fn next_event_sequence() -> u64 {
    static NEXT_SEQUENCE: AtomicU64 = AtomicU64::new(1);
    loop {
        let sequence = NEXT_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        if sequence != 0 {
            return sequence;
        }
    }
}

fn valid_config(config: &ml_config_v1) -> ml_status_t {
    if config.struct_size < size_of::<ml_config_v1>() as u32 || config.abi_version != ML_ABI_VERSION
    {
        return ML_STATUS_ABI_MISMATCH;
    }
    if config.event_queue_capacity == 0
        || config.event_queue_capacity > ML_MAX_EVENT_QUEUE_CAPACITY
        || config.max_event_payload_bytes == 0
        || config.max_event_payload_bytes > ML_MAX_EVENT_PAYLOAD_BYTES
    {
        return ML_STATUS_INVALID_ARGUMENT;
    }
    ML_STATUS_OK
}

fn valid_output(struct_size: u32, abi_version: u32, expected_size: usize) -> ml_status_t {
    if struct_size < expected_size as u32 || abi_version != ML_ABI_VERSION {
        ML_STATUS_ABI_MISMATCH
    } else {
        ML_STATUS_OK
    }
}

fn clear_event(event: &mut ml_event_v1) {
    let struct_size = event.struct_size;
    let abi_version = event.abi_version;
    *event = ml_event_v1 {
        struct_size,
        abi_version,
        sequence: 0,
        request_id: 0,
        kind: 0,
        status: 0,
        payload_schema_version: 0,
        reserved: 0,
        payload: ptr::null(),
        payload_len: 0,
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn ml_abi_version() -> u32 {
    catch_unwind(|| ML_ABI_VERSION).unwrap_or(0)
}

#[unsafe(no_mangle)]
/// Creates a core and writes its opaque handle to `out_core`.
///
/// # Safety
///
/// `config` must point to a readable `ml_config_v1`. `out_core` must point to
/// writable storage for one handle. Both pointers are borrowed only for this call.
pub unsafe extern "C" fn ml_core_create(
    config: *const ml_config_v1,
    out_core: *mut *mut ml_core_t,
) -> ml_status_t {
    ffi_status(|| {
        if out_core.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_core = ptr::null_mut() };
        if config.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let config = unsafe { &*config };
        let status = valid_config(config);
        if status != ML_STATUS_OK {
            return status;
        }

        let mut state = CoreState::new(config);
        let event = OwnedEvent::new(0, ML_EVENT_CORE_READY, ML_STATUS_OK, 1, b"1".to_vec());
        if state.enqueue_unreserved(event).is_err() {
            return ML_STATUS_BUSY;
        }
        let core = Arc::new(Core {
            state: Mutex::new(state),
        });
        let handle = lock(registry()).insert(core);
        unsafe { *out_core = ptr::without_provenance_mut(handle) };
        ML_STATUS_OK
    })
}

#[unsafe(no_mangle)]
/// Destroys a core. Null, stale, and already-destroyed handles are ignored.
///
/// # Safety
///
/// `core` must be null or an opaque handle previously returned by
/// `ml_core_create`. Any event payload borrows end during this call.
pub unsafe extern "C" fn ml_core_destroy(core: *mut ml_core_t) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Some(handle) = handle_id(core) else {
            return;
        };
        let Some(core) = lock(registry()).cores.remove(&handle) else {
            return;
        };
        let retired_waker = {
            let mut state = lock(&core.state);
            state.destroyed = true;
            state.queued.clear();
            state.in_flight.clear();
            state.pending_requests.clear();
            state.waker.take()
        };
        if let Some(waker) = retired_waker {
            waker.retire();
        }
    }));
}

#[unsafe(no_mangle)]
/// Replaces the thread-safe empty-to-nonempty event waker.
///
/// Passing a null callback and null context clears the current waker. Clearing
/// or replacing a waker waits for its active calls to finish. Registration does
/// not wake for events that were already queued, so callers must drain once.
/// When called by the active callback itself, retirement completes when that
/// callback returns.
///
/// # Safety
///
/// The callback and context must be safe to invoke from any thread until the
/// registration is cleared or the core is destroyed. The callback must not unwind.
pub unsafe extern "C" fn ml_core_set_waker(
    core: *mut ml_core_t,
    callback: ml_wake_fn,
    context: *mut c_void,
) -> ml_status_t {
    if callback.is_none() && !context.is_null() {
        return ML_STATUS_INVALID_ARGUMENT;
    }
    ffi_core_status(core, true, |state| {
        let retired_waker = state.waker.take();
        state.waker = callback.map(|callback| Arc::new(WakeRegistration::new(callback, context)));
        Action {
            status: ML_STATUS_OK,
            wake: None,
            retired_waker,
        }
    })
}

#[unsafe(no_mangle)]
/// Returns the configured transport bounds for a core.
///
/// # Safety
///
/// `out_limits` must point to a writable `ml_core_limits_v1` whose versioned
/// prefix is initialized by the caller.
pub unsafe extern "C" fn ml_core_get_limits_v1(
    core: *mut ml_core_t,
    out_limits: *mut ml_core_limits_v1,
) -> ml_status_t {
    ffi_core_status(core, true, |state| {
        if out_limits.is_null() {
            return Action::status(ML_STATUS_INVALID_ARGUMENT);
        }
        let out_limits = unsafe { &mut *out_limits };
        let status = valid_output(
            out_limits.struct_size,
            out_limits.abi_version,
            size_of::<ml_core_limits_v1>(),
        );
        if status != ML_STATUS_OK {
            return Action::status(status);
        }
        out_limits.event_queue_capacity = state.event_queue_capacity as u32;
        out_limits.max_event_payload_bytes = state.max_event_payload_bytes as u32;
        Action::status(ML_STATUS_OK)
    })
}

#[unsafe(no_mangle)]
/// Polls one event and transfers its payload borrow to the caller.
///
/// # Safety
///
/// `core` must be an opaque handle returned by `ml_core_create`. `out_event`
/// must point to writable `ml_event_v1` storage with an initialized versioned
/// prefix. A successful event must be returned with `ml_core_release_event`.
pub unsafe extern "C" fn ml_core_poll_event(
    core: *mut ml_core_t,
    out_event: *mut ml_event_v1,
) -> ml_status_t {
    ffi_core_status(core, true, |state| {
        if out_event.is_null() {
            return Action::status(ML_STATUS_INVALID_ARGUMENT);
        }
        let out_event = unsafe { &mut *out_event };
        let status = valid_output(
            out_event.struct_size,
            out_event.abi_version,
            size_of::<ml_event_v1>(),
        );
        if status != ML_STATUS_OK {
            return Action::status(status);
        }
        clear_event(out_event);

        let Some(event) = state.queued.pop_front() else {
            return Action::status(ML_STATUS_EMPTY);
        };
        let sequence = event.sequence;
        state.in_flight.insert(sequence, event);
        let event = state
            .in_flight
            .get(&sequence)
            .expect("inserted event is present");
        out_event.sequence = event.sequence;
        out_event.request_id = event.request_id;
        out_event.kind = event.kind;
        out_event.status = event.status;
        out_event.payload_schema_version = event.payload_schema_version;
        out_event.payload = if event.payload.is_empty() {
            ptr::null()
        } else {
            event.payload.as_ptr()
        };
        out_event.payload_len = event.payload.len();
        Action::status(ML_STATUS_OK)
    })
}

#[unsafe(no_mangle)]
/// Releases one event previously returned by `ml_core_poll_event`.
///
/// # Safety
///
/// `core` must be the handle that produced `event`. `event` must remain writable
/// for this call and must not have been released already.
pub unsafe extern "C" fn ml_core_release_event(core: *mut ml_core_t, event: *mut ml_event_v1) {
    let Some(core) = resolve_core(core) else {
        return;
    };
    let result = catch_unwind(AssertUnwindSafe(|| {
        if event.is_null() {
            return None;
        }
        let event = unsafe { &mut *event };
        let mut state = lock(&core.state);
        if state.destroyed
            || event.sequence == 0
            || state.in_flight.remove(&event.sequence).is_none()
        {
            return None;
        }
        clear_event(event);
        state.materialize_fatal()
    }));
    match result {
        Ok(Some(wake)) => wake.invoke(),
        Ok(None) => {}
        Err(_) => mark_core_failed(&core),
    }
}

#[unsafe(no_mangle)]
/// Cancels an active asynchronous request.
pub extern "C" fn ml_core_cancel(core: *mut ml_core_t, request_id: u64) -> ml_status_t {
    if request_id == 0 {
        return ML_STATUS_INVALID_ARGUMENT;
    }
    ffi_core_status(core, false, |state| {
        if !state.pending_requests.contains_key(&request_id) {
            return Action::status(ML_STATUS_NOT_FOUND);
        }
        let event = OwnedEvent::new(
            request_id,
            ML_EVENT_REQUEST_CANCELLED,
            ML_STATUS_CANCELLED,
            1,
            Vec::new(),
        );
        state.complete_reserved(request_id, event)
    })
}

#[cfg(feature = "abi-test-hooks")]
#[unsafe(no_mangle)]
/// Accepts one deterministic held UTF-8 request for C ABI transport tests.
///
/// # Safety
///
/// `payload` must be readable for `payload_len` bytes during this call, and
/// `out_request_id` must point to writable `u64` storage.
pub unsafe extern "C" fn ml_core_test_submit(
    core: *mut ml_core_t,
    payload: *const u8,
    payload_len: usize,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_core_status(core, false, |state| {
        if out_request_id.is_null() {
            return Action::status(ML_STATUS_INVALID_ARGUMENT);
        }
        unsafe { *out_request_id = 0 };
        if payload_len > state.max_event_payload_bytes || (payload.is_null() && payload_len != 0) {
            return Action::status(ML_STATUS_INVALID_ARGUMENT);
        }
        let payload = if payload_len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(payload, payload_len) }
        };
        if std::str::from_utf8(payload).is_err() {
            return Action::status(ML_STATUS_INVALID_ARGUMENT);
        }
        match state.accept_test_request(payload) {
            Ok(request_id) => {
                unsafe { *out_request_id = request_id };
                Action::status(ML_STATUS_OK)
            }
            Err(status) => Action::status(status),
        }
    })
}

#[cfg(feature = "abi-test-hooks")]
#[unsafe(no_mangle)]
/// Completes one deterministic held request for C ABI transport tests.
pub extern "C" fn ml_core_test_complete(core: *mut ml_core_t, request_id: u64) -> ml_status_t {
    ffi_core_status(core, false, |state| {
        let Some(request) = state.pending_requests.get(&request_id) else {
            return Action::status(ML_STATUS_NOT_FOUND);
        };
        let event = OwnedEvent::new(
            request_id,
            u32::MAX,
            ML_STATUS_OK,
            1,
            request.payload.clone(),
        );
        state.complete_reserved(request_id, event)
    })
}

#[cfg(feature = "abi-test-hooks")]
#[unsafe(no_mangle)]
/// Forces the per-core panic barrier for C ABI containment tests.
pub extern "C" fn ml_core_test_panic(core: *mut ml_core_t) -> ml_status_t {
    ffi_core_status(core, false, |_state| panic!("forced ABI test panic"))
}

#[cfg(feature = "abi-test-hooks")]
#[unsafe(no_mangle)]
/// Enqueues a nonterminal event for C ABI waker tests.
pub extern "C" fn ml_core_test_emit(core: *mut ml_core_t) -> ml_status_t {
    ffi_core_status(core, false, |state| {
        let event = OwnedEvent::new(0, u32::MAX - 1, ML_STATUS_OK, 1, Vec::new());
        match state.enqueue_unreserved(event) {
            Ok(wake) => Action::with_wake(ML_STATUS_OK, wake),
            Err(status) => Action::status(status),
        }
    })
}
