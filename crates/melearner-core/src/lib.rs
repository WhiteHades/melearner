#![allow(non_camel_case_types)]

mod coordinator;
mod library;
pub mod migrations;
pub mod scanner;
pub mod schema;

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::io::{self, Write};
use std::mem::size_of;
use std::num::{NonZeroU64, NonZeroUsize};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicU8, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock, Weak};

use coordinator::{
    DomainError, DomainEvent, DomainEventSink, DomainRequest, DomainResponse, DomainWorker,
    SubmitError,
};
use library::{ActivityPageInput, LibraryError, ProgressInput, SearchPageInput};

pub const ML_ABI_VERSION: u32 = 2;
pub const ML_MAX_EVENT_QUEUE_CAPACITY: u32 = 65_536;
pub const ML_MAX_EVENT_PAYLOAD_BYTES: u32 = 16 * 1024 * 1024;
pub const ML_MIN_EVENT_PAYLOAD_BYTES: u32 = 20;
pub const ML_MAX_SEARCH_QUERY_BYTES: u32 = 64 * 1024;

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
pub const ML_STATUS_STALE: ml_status_t = 10;

pub type ml_event_kind_t = u32;
pub const ML_EVENT_CORE_READY: ml_event_kind_t = 1;
pub const ML_EVENT_REQUEST_CANCELLED: ml_event_kind_t = 2;
pub const ML_EVENT_FATAL: ml_event_kind_t = 3;
pub const ML_EVENT_LIBRARY_COURSE_PAGE: ml_event_kind_t = 4;
pub const ML_EVENT_LIBRARY_LESSON_PAGE: ml_event_kind_t = 5;
pub const ML_EVENT_LIBRARY_SCAN: ml_event_kind_t = 6;
pub const ML_EVENT_PROGRESS_UPDATED: ml_event_kind_t = 7;
pub const ML_EVENT_ACTIVITY_DAY_PAGE: ml_event_kind_t = 8;
pub const ML_EVENT_SEARCH_INDEX_READY: ml_event_kind_t = 9;
pub const ML_EVENT_SEARCH_PAGE: ml_event_kind_t = 10;

pub type ml_wake_fn = Option<unsafe extern "C" fn(context: *mut c_void)>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_config_v2 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub event_queue_capacity: u32,
    pub max_event_payload_bytes: u32,
    pub state_dir: *const u8,
    pub state_dir_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_library_course_page_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub offset: u64,
    pub limit: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_library_lesson_page_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub offset: u64,
    pub limit: u32,
    pub reserved: u32,
    pub course_id: *const u8,
    pub course_id_len: usize,
    pub section_id: *const u8,
    pub section_id_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_library_scan_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub root_path: *const u8,
    pub root_path_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_progress_put_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub watched_time: u64,
    pub last_position: f64,
    pub completed: u32,
    pub reserved: u32,
    pub lesson_id: *const u8,
    pub lesson_id_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_activity_day_page_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub offset: u64,
    pub lookback_days: u32,
    pub limit: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_search_rebuild_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_revision: u64,
    pub reserved: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ml_search_query_request_v1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub expected_index_revision: u64,
    pub query_id: u64,
    pub offset: u64,
    pub limit: u32,
    pub reserved: u32,
    pub query: *const u8,
    pub query_len: usize,
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

enum PendingRequest {
    Domain {
        event_kind: ml_event_kind_t,
        mutation_control: Option<Arc<MutationControl>>,
    },
    #[cfg(feature = "abi-test-hooks")]
    Test { payload: Vec<u8> },
}

const MUTATION_ACTIVE: u8 = 0;
const MUTATION_CANCELLED: u8 = 1;
const MUTATION_COMMITTING: u8 = 2;

#[derive(Debug)]
pub(crate) struct MutationControl {
    state: AtomicU8,
}

impl MutationControl {
    pub(crate) fn new() -> Self {
        Self {
            state: AtomicU8::new(MUTATION_ACTIVE),
        }
    }

    pub(crate) fn is_cancelled(&self) -> bool {
        self.state.load(Ordering::Acquire) == MUTATION_CANCELLED
    }

    pub(crate) fn begin_commit(&self) -> bool {
        self.state
            .compare_exchange(
                MUTATION_ACTIVE,
                MUTATION_COMMITTING,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
    }

    fn cancel(&self) -> bool {
        self.state
            .compare_exchange(
                MUTATION_ACTIVE,
                MUTATION_CANCELLED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
    }
}

struct CoreState {
    event_queue_capacity: usize,
    max_event_payload_bytes: usize,
    queued: VecDeque<OwnedEvent>,
    in_flight: HashMap<u64, OwnedEvent>,
    pending_requests: HashMap<u64, PendingRequest>,
    next_request_id: u64,
    starting: bool,
    failed: bool,
    fatal_pending: bool,
    fatal_emitted: bool,
    destroyed: bool,
    waker: Option<Arc<WakeRegistration>>,
}

impl CoreState {
    fn new(config: &ml_config_v2) -> Self {
        Self {
            event_queue_capacity: config.event_queue_capacity as usize,
            max_event_payload_bytes: config.max_event_payload_bytes as usize,
            queued: VecDeque::with_capacity(config.event_queue_capacity as usize),
            in_flight: HashMap::new(),
            pending_requests: HashMap::new(),
            next_request_id: 1,
            starting: true,
            failed: false,
            fatal_pending: false,
            fatal_emitted: false,
            destroyed: false,
            waker: None,
        }
    }

    fn outstanding(&self) -> usize {
        self.queued.len()
            + self.in_flight.len()
            + self.pending_requests.len()
            + usize::from(self.starting)
    }

    #[cfg(feature = "abi-test-hooks")]
    fn enqueue_unreserved(&mut self, event: OwnedEvent) -> Result<Option<WakeCall>, ml_status_t> {
        if self.starting {
            return Err(ML_STATUS_BUSY);
        }
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
        if self.starting || self.outstanding() >= self.event_queue_capacity {
            return Err(ML_STATUS_BUSY);
        }
        let request_id = self.next_request_id();
        self.pending_requests.insert(
            request_id,
            PendingRequest::Test {
                payload: payload.to_vec(),
            },
        );
        Ok(request_id)
    }

    fn reserve_domain_request(
        &mut self,
        event_kind: ml_event_kind_t,
        mutation_control: Option<Arc<MutationControl>>,
    ) -> Result<u64, ml_status_t> {
        if self.starting || self.outstanding() >= self.event_queue_capacity {
            return Err(ML_STATUS_BUSY);
        }
        let request_id = self.next_request_id();
        self.pending_requests.insert(
            request_id,
            PendingRequest::Domain {
                event_kind,
                mutation_control,
            },
        );
        Ok(request_id)
    }

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

    fn cancel_pending_mutations(&self) {
        for pending in self.pending_requests.values() {
            if let PendingRequest::Domain {
                mutation_control: Some(control),
                ..
            } = pending
            {
                let _ = control.cancel();
            }
        }
    }

    fn fail(&mut self) -> Option<WakeCall> {
        if self.destroyed || self.failed {
            return None;
        }
        self.starting = false;
        self.failed = true;
        self.cancel_pending_mutations();
        self.pending_requests.clear();
        if !self.fatal_emitted {
            self.fatal_pending = true;
        }
        self.materialize_fatal()
    }
}

struct NativeCore {
    state: Arc<Mutex<CoreState>>,
    domain: Mutex<Option<DomainWorker>>,
}

struct Registry {
    next_handle: usize,
    cores: HashMap<usize, Arc<NativeCore>>,
}

impl Registry {
    fn new() -> Self {
        Self {
            next_handle: 1,
            cores: HashMap::new(),
        }
    }

    fn insert(&mut self, core: Arc<NativeCore>) -> usize {
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

fn resolve_core(core: *mut ml_core_t) -> Option<Arc<NativeCore>> {
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

unsafe fn submit_domain_request(
    core: *mut ml_core_t,
    event_kind: ml_event_kind_t,
    mut request: DomainRequest,
    mutation_control: Option<Arc<MutationControl>>,
    out_request_id: *mut u64,
) -> ml_status_t {
    let Some(core) = resolve_core(core) else {
        return ML_STATUS_INVALID_HANDLE;
    };
    match catch_unwind(AssertUnwindSafe(|| {
        let mut state = lock(&core.state);
        if state.destroyed {
            return (ML_STATUS_INVALID_HANDLE, None);
        }
        if state.failed {
            return (ML_STATUS_FAILED, None);
        }
        match &mut request {
            DomainRequest::Scan {
                max_payload_bytes, ..
            }
            | DomainRequest::PutProgress {
                max_payload_bytes, ..
            }
            | DomainRequest::RebuildSearch {
                max_payload_bytes, ..
            } => *max_payload_bytes = state.max_event_payload_bytes,
            _ => {}
        }
        let request_id = match state.reserve_domain_request(event_kind, mutation_control) {
            Ok(request_id) => request_id,
            Err(status) => return (status, None),
        };
        let domain = lock(&core.domain);
        let submitted = match domain.as_ref() {
            Some(domain) => domain.try_submit(
                NonZeroU64::new(request_id).expect("generated request ID is nonzero"),
                request,
            ),
            None => Err(SubmitError::Closed),
        };
        match submitted {
            Ok(()) => {
                unsafe { *out_request_id = request_id };
                (ML_STATUS_OK, None)
            }
            Err(SubmitError::Full) => {
                state.pending_requests.remove(&request_id);
                (ML_STATUS_BUSY, None)
            }
            Err(SubmitError::Closed) => {
                state.pending_requests.remove(&request_id);
                (ML_STATUS_FAILED, state.fail())
            }
        }
    })) {
        Ok((status, wake)) => {
            if let Some(wake) = wake {
                wake.invoke();
            }
            status
        }
        Err(_) => {
            mark_core_failed(&core);
            ML_STATUS_PANIC
        }
    }
}

fn mark_core_failed(core: &Arc<NativeCore>) {
    let wake = catch_unwind(AssertUnwindSafe(|| lock(&core.state).fail()))
        .ok()
        .flatten();
    if let Some(wake) = wake {
        wake.invoke();
    }
}

fn publish_domain_event(state: &Weak<Mutex<CoreState>>, event: DomainEvent) -> bool {
    let event = match event {
        DomainEvent::Completed(outcome) => return publish_domain_completion(state, outcome),
        event => event,
    };
    let Some(state) = state.upgrade() else {
        return false;
    };
    let wake = {
        let mut state = lock(&state);
        if state.destroyed {
            return false;
        }
        match event {
            DomainEvent::Ready { revision } => {
                if !state.starting || state.failed {
                    return false;
                }
                let payload = revision.to_string().into_bytes();
                if payload.len() > state.max_event_payload_bytes {
                    state.fail()
                } else {
                    state.starting = false;
                    state.push_event(OwnedEvent::new(
                        0,
                        ML_EVENT_CORE_READY,
                        ML_STATUS_OK,
                        1,
                        payload,
                    ))
                }
            }
            DomainEvent::Fatal(error) => {
                drop(error);
                state.fail()
            }
            DomainEvent::Completed(_) => unreachable!("completion handled before startup event"),
        }
    };
    if let Some(wake) = wake {
        wake.invoke();
    }
    true
}

fn publish_domain_completion(
    state: &Weak<Mutex<CoreState>>,
    outcome: coordinator::DomainOutcome,
) -> bool {
    let Some(state) = state.upgrade() else {
        return false;
    };
    let max_payload_bytes = lock(&state).max_event_payload_bytes;
    let (status, payload_schema_version, payload) =
        encode_domain_result(outcome.result, max_payload_bytes);
    let action = {
        let mut state = lock(&state);
        if state.destroyed || state.failed {
            return false;
        }
        let Some(PendingRequest::Domain { event_kind, .. }) =
            state.pending_requests.get(&outcome.request_id.get())
        else {
            return true;
        };
        let event_kind = *event_kind;
        debug_assert!(payload.len() <= state.max_event_payload_bytes);
        let event = OwnedEvent::new(
            outcome.request_id.get(),
            event_kind,
            status,
            payload_schema_version,
            payload,
        );
        state.complete_reserved(outcome.request_id.get(), event)
    };
    action.finish();
    true
}

fn encode_domain_result(
    result: Result<DomainResponse, DomainError>,
    max_payload_bytes: usize,
) -> (ml_status_t, u32, Vec<u8>) {
    match result {
        Ok(DomainResponse::CoursePage(page)) => encode_json(ML_STATUS_OK, &page, max_payload_bytes),
        Ok(DomainResponse::LessonPage(page)) => encode_json(ML_STATUS_OK, &page, max_payload_bytes),
        Ok(DomainResponse::Scan(scan)) => encode_json(ML_STATUS_OK, &scan, max_payload_bytes),
        Ok(DomainResponse::Progress(progress)) => {
            encode_json(ML_STATUS_OK, &progress, max_payload_bytes)
        }
        Ok(DomainResponse::ActivityDayPage(page)) => {
            encode_json(ML_STATUS_OK, &page, max_payload_bytes)
        }
        Ok(DomainResponse::SearchIndexReady(ready)) => {
            encode_json(ML_STATUS_OK, &ready, max_payload_bytes)
        }
        Ok(DomainResponse::SearchPage(page)) => encode_json(ML_STATUS_OK, &page, max_payload_bytes),
        Err(DomainError::Library(LibraryError::InvalidPageSize { limit })) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({
                "error": "invalidPageSize",
                "limit": limit,
            }),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::InvalidOffset { offset })) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({
                "error": "invalidOffset",
                "offset": offset,
            }),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::InvalidActivityLookback { days })) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({
                "error": "invalidActivityLookback",
                "days": days,
            }),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::InvalidProgress)) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({"error": "invalidProgress"}),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::InvalidSearchQuery)) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({"error": "invalidSearchQuery"}),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::LessonNotFound)) => encode_json(
            ML_STATUS_NOT_FOUND,
            &serde_json::json!({"error": "lessonNotFound"}),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::StaleSearchIndex { expected, actual })) => {
            encode_json(
                ML_STATUS_STALE,
                &serde_json::json!({
                    "error": "staleSearchIndex",
                    "expected": expected,
                    "actual": actual,
                }),
                max_payload_bytes,
            )
        }
        Err(DomainError::Library(LibraryError::StaleRevision { expected, actual })) => encode_json(
            ML_STATUS_STALE,
            &serde_json::json!({
                "error": "staleRevision",
                "expected": expected,
                "actual": actual,
            }),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::Cancelled)) => (ML_STATUS_CANCELLED, 0, Vec::new()),
        Err(DomainError::Library(LibraryError::InvalidScan(_))) => encode_json(
            ML_STATUS_INVALID_ARGUMENT,
            &serde_json::json!({"error": "invalidScan"}),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::RevisionExhausted)) => encode_json(
            ML_STATUS_FAILED,
            &serde_json::json!({"error": "revisionExhausted"}),
            max_payload_bytes,
        ),
        Err(DomainError::Library(LibraryError::ResponseTooLarge { .. })) => {
            (ML_STATUS_FAILED, 0, Vec::new())
        }
        Err(DomainError::Library(LibraryError::Database(_))) => encode_json(
            ML_STATUS_FAILED,
            &serde_json::json!({"error": "database"}),
            max_payload_bytes,
        ),
        Err(DomainError::WorkerPanicked) => encode_json(
            ML_STATUS_FAILED,
            &serde_json::json!({"error": "workerPanicked"}),
            max_payload_bytes,
        ),
    }
}

fn encode_json(
    status: ml_status_t,
    value: &impl serde::Serialize,
    max_payload_bytes: usize,
) -> (ml_status_t, u32, Vec<u8>) {
    let mut writer = LimitedWriter::new(max_payload_bytes);
    match serde_json::to_writer(&mut writer, value) {
        Ok(()) => (status, 1, writer.into_inner()),
        Err(_) => (ML_STATUS_FAILED, 0, Vec::new()),
    }
}

struct LimitedWriter {
    bytes: Vec<u8>,
    limit: usize,
}

impl LimitedWriter {
    fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
        }
    }

    fn into_inner(self) -> Vec<u8> {
        self.bytes
    }
}

impl Write for LimitedWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > self.limit.saturating_sub(self.bytes.len()) {
            return Err(io::Error::other("event payload limit exceeded"));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
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

fn take_next_library_revision(next_revision: &AtomicU64) -> Option<NonZeroU64> {
    let revision = next_revision
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |revision| {
            if revision == 0 {
                None
            } else {
                Some(revision.checked_add(1).unwrap_or(0))
            }
        })
        .ok()?;
    NonZeroU64::new(revision)
}

fn next_library_revision() -> Option<NonZeroU64> {
    static NEXT_REVISION: AtomicU64 = AtomicU64::new(1);
    take_next_library_revision(&NEXT_REVISION)
}

#[cfg(test)]
#[test]
fn library_revision_allocator_exhausts_instead_of_reusing_values() {
    let next_revision = AtomicU64::new(u64::MAX);
    assert_eq!(
        take_next_library_revision(&next_revision).map(NonZeroU64::get),
        Some(u64::MAX)
    );
    assert_eq!(take_next_library_revision(&next_revision), None);
}

#[cfg(test)]
#[test]
fn mutation_cancellation_and_commit_gate_are_mutually_exclusive() {
    let cancelled = MutationControl::new();
    assert!(cancelled.cancel());
    assert!(cancelled.is_cancelled());
    assert!(!cancelled.begin_commit());
    assert!(!cancelled.cancel());

    let committing = MutationControl::new();
    assert!(committing.begin_commit());
    assert!(!committing.is_cancelled());
    assert!(!committing.cancel());
    assert!(!committing.begin_commit());
}

#[cfg(test)]
#[test]
fn required_ffi_strings_are_copied_before_the_caller_mutates_them() {
    let mut bytes = b"/library/original".to_vec();
    let copied = unsafe { copy_required_string(bytes.as_ptr(), bytes.len()) }
        .expect("copy valid FFI string");

    bytes.fill(b'x');

    assert_eq!(copied, "/library/original");
}

#[cfg(test)]
#[test]
fn ready_payload_respects_the_minimum_bound_at_the_largest_revision() {
    let state_dir = b"state";
    let config = ml_config_v2 {
        struct_size: size_of::<ml_config_v2>() as u32,
        abi_version: ML_ABI_VERSION,
        event_queue_capacity: 1,
        max_event_payload_bytes: ML_MIN_EVENT_PAYLOAD_BYTES,
        state_dir: state_dir.as_ptr(),
        state_dir_len: state_dir.len(),
    };
    let state = Arc::new(Mutex::new(CoreState::new(&config)));

    assert!(publish_domain_event(
        &Arc::downgrade(&state),
        DomainEvent::Ready { revision: u64::MAX },
    ));

    let state = lock(&state);
    let ready = state.queued.front().expect("ready event");
    assert_eq!(ready.kind, ML_EVENT_CORE_READY);
    assert_eq!(ready.payload, u64::MAX.to_string().as_bytes());
    assert!(ready.payload.len() <= state.max_event_payload_bytes);
}

fn valid_config(config: &ml_config_v2) -> ml_status_t {
    if config.struct_size < size_of::<ml_config_v2>() as u32 || config.abi_version != ML_ABI_VERSION
    {
        return ML_STATUS_ABI_MISMATCH;
    }
    if config.event_queue_capacity == 0
        || config.event_queue_capacity > ML_MAX_EVENT_QUEUE_CAPACITY
        || config.max_event_payload_bytes < ML_MIN_EVENT_PAYLOAD_BYTES
        || config.max_event_payload_bytes > ML_MAX_EVENT_PAYLOAD_BYTES
        || config.state_dir.is_null()
        || config.state_dir_len == 0
        || config.state_dir_len > ML_MAX_EVENT_PAYLOAD_BYTES as usize
    {
        return ML_STATUS_INVALID_ARGUMENT;
    }
    ML_STATUS_OK
}

unsafe fn copy_state_dir(config: &ml_config_v2) -> Result<PathBuf, ml_status_t> {
    let bytes = unsafe { std::slice::from_raw_parts(config.state_dir, config.state_dir_len) };
    let value = std::str::from_utf8(bytes).map_err(|_| ML_STATUS_INVALID_ARGUMENT)?;
    if value.is_empty() || value.contains('\0') {
        return Err(ML_STATUS_INVALID_ARGUMENT);
    }
    Ok(PathBuf::from(value))
}

unsafe fn copy_required_string(value: *const u8, len: usize) -> Result<String, ml_status_t> {
    if value.is_null() || len == 0 || len > ML_MAX_EVENT_PAYLOAD_BYTES as usize {
        return Err(ML_STATUS_INVALID_ARGUMENT);
    }
    let bytes = unsafe { std::slice::from_raw_parts(value, len) };
    let value = std::str::from_utf8(bytes).map_err(|_| ML_STATUS_INVALID_ARGUMENT)?;
    if value.contains('\0') {
        return Err(ML_STATUS_INVALID_ARGUMENT);
    }
    Ok(value.to_owned())
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
/// `config` must point to a readable `ml_config_v2`. Its state-directory bytes
/// must remain readable for this call. `out_core` must point to
/// writable storage for one handle. Both pointers are borrowed only for this call.
pub unsafe extern "C" fn ml_core_create(
    config: *const ml_config_v2,
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
        let state_dir = match unsafe { copy_state_dir(config) } {
            Ok(state_dir) => state_dir,
            Err(status) => return status,
        };

        let state = Arc::new(Mutex::new(CoreState::new(config)));
        let weak_state = Arc::downgrade(&state);
        let event_sink: DomainEventSink =
            Arc::new(move |event| publish_domain_event(&weak_state, event));
        let capacity = NonZeroUsize::new(config.event_queue_capacity as usize)
            .expect("validated event capacity is nonzero");
        let Some(revision) = next_library_revision() else {
            return ML_STATUS_FAILED;
        };
        let domain = match DomainWorker::start(state_dir, revision, capacity, event_sink) {
            Ok(domain) => domain,
            Err(_) => return ML_STATUS_FAILED,
        };
        let core = Arc::new(NativeCore {
            state,
            domain: Mutex::new(Some(domain)),
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
            state.starting = false;
            state.queued.clear();
            state.in_flight.clear();
            state.cancel_pending_mutations();
            state.pending_requests.clear();
            state.waker.take()
        };
        drop(lock(&core.domain).take());
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
        match state.pending_requests.get(&request_id) {
            Some(PendingRequest::Domain {
                mutation_control: Some(control),
                ..
            }) if !control.cancel() => return Action::status(ML_STATUS_NOT_FOUND),
            Some(_) => {}
            None => return Action::status(ML_STATUS_NOT_FOUND),
        }
        let event = OwnedEvent::new(
            request_id,
            ML_EVENT_REQUEST_CANCELLED,
            ML_STATUS_CANCELLED,
            0,
            Vec::new(),
        );
        state.complete_reserved(request_id, event)
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous Library course-page request.
///
/// # Safety
///
/// `request` must point to a readable `ml_library_course_page_request_v1`, and
/// `out_request_id` must point to writable `u64` storage. Both pointers are
/// borrowed only for this call.
pub unsafe extern "C" fn ml_library_course_page_v1(
    core: *mut ml_core_t,
    request: *const ml_library_course_page_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_library_course_page_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_LIBRARY_COURSE_PAGE,
                DomainRequest::CoursePage {
                    expected_revision: request.expected_revision,
                    offset: request.offset,
                    limit: request.limit,
                },
                None,
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous Library lesson-page request.
///
/// A null `section_id` with zero length selects all Sections in the Course.
///
/// # Safety
///
/// `request` must point to a readable `ml_library_lesson_page_request_v1`.
/// Its ID byte ranges must remain readable for this call. `out_request_id`
/// must point to writable `u64` storage. All inputs are copied before return.
pub unsafe extern "C" fn ml_library_lesson_page_v1(
    core: *mut ml_core_t,
    request: *const ml_library_lesson_page_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_library_lesson_page_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let course_id =
            match unsafe { copy_required_string(request.course_id, request.course_id_len) } {
                Ok(course_id) => course_id,
                Err(status) => return status,
            };
        let section_id = if request.section_id.is_null() && request.section_id_len == 0 {
            None
        } else {
            match unsafe { copy_required_string(request.section_id, request.section_id_len) } {
                Ok(section_id) => Some(section_id),
                Err(status) => return status,
            }
        };
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_LIBRARY_LESSON_PAGE,
                DomainRequest::LessonPage {
                    expected_revision: request.expected_revision,
                    course_id,
                    section_id,
                    offset: request.offset,
                    limit: request.limit,
                },
                None,
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous Library scan and reconciliation request.
///
/// # Safety
///
/// `request` must point to a readable `ml_library_scan_request_v1`. Its root
/// path bytes must remain readable for this call. `out_request_id` must point
/// to writable `u64` storage. All inputs are copied before return.
pub unsafe extern "C" fn ml_library_scan_v1(
    core: *mut ml_core_t,
    request: *const ml_library_scan_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_library_scan_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        let root_path =
            match unsafe { copy_required_string(request.root_path, request.root_path_len) } {
                Ok(root_path) => root_path,
                Err(status) => return status,
            };
        let control = Arc::new(MutationControl::new());
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_LIBRARY_SCAN,
                DomainRequest::Scan {
                    expected_revision: request.expected_revision,
                    root_path,
                    max_payload_bytes: 0,
                    control: Arc::clone(&control),
                },
                Some(control),
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous Lesson Progress update.
///
/// # Safety
///
/// `request` must point to a readable `ml_progress_put_request_v1`. Its Lesson
/// ID bytes must remain readable for this call. `out_request_id` must point to
/// writable `u64` storage. All inputs are copied before return.
pub unsafe extern "C" fn ml_progress_put_v1(
    core: *mut ml_core_t,
    request: *const ml_progress_put_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_progress_put_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 || request.completed > 1 {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let lesson_id =
            match unsafe { copy_required_string(request.lesson_id, request.lesson_id_len) } {
                Ok(lesson_id) => lesson_id,
                Err(status) => return status,
            };
        let control = Arc::new(MutationControl::new());
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_PROGRESS_UPDATED,
                DomainRequest::PutProgress {
                    input: ProgressInput {
                        expected_revision: request.expected_revision,
                        lesson_id,
                        watched_time: request.watched_time,
                        last_position: request.last_position,
                        completed: request.completed == 1,
                    },
                    max_payload_bytes: 0,
                    control: Arc::clone(&control),
                },
                Some(control),
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous Learning activity day-page request.
///
/// # Safety
///
/// `request` must point to a readable `ml_activity_day_page_request_v1`, and
/// `out_request_id` must point to writable `u64` storage. Both pointers are
/// borrowed only for this call.
pub unsafe extern "C" fn ml_activity_day_page_v1(
    core: *mut ml_core_t,
    request: *const ml_activity_day_page_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_activity_day_page_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_ACTIVITY_DAY_PAGE,
                DomainRequest::ActivityDayPage {
                    input: ActivityPageInput {
                        expected_revision: request.expected_revision,
                        lookback_days: request.lookback_days,
                        offset: request.offset,
                        limit: request.limit,
                    },
                },
                None,
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Rebuilds the in-memory Lesson search index from the current Library.
///
/// # Safety
///
/// `request` must point to a readable `ml_search_rebuild_request_v1`, and
/// `out_request_id` must point to writable `u64` storage. Both pointers are
/// borrowed only for this call.
pub unsafe extern "C" fn ml_search_rebuild_v1(
    core: *mut ml_core_t,
    request: *const ml_search_rebuild_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_search_rebuild_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let control = Arc::new(MutationControl::new());
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_SEARCH_INDEX_READY,
                DomainRequest::RebuildSearch {
                    expected_revision: request.expected_revision,
                    max_payload_bytes: 0,
                    control: Arc::clone(&control),
                },
                Some(control),
                out_request_id,
            )
        }
    })
}

#[unsafe(no_mangle)]
/// Submits one asynchronous paged Lesson search query.
///
/// # Safety
///
/// `request` must point to a readable `ml_search_query_request_v1`. Its query
/// bytes must remain readable for this call. `out_request_id` must point to
/// writable `u64` storage. The query is copied before return.
pub unsafe extern "C" fn ml_search_query_v1(
    core: *mut ml_core_t,
    request: *const ml_search_query_request_v1,
    out_request_id: *mut u64,
) -> ml_status_t {
    ffi_status(|| {
        if out_request_id.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        unsafe { *out_request_id = 0 };
        if request.is_null() {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let request = unsafe { *request };
        let status = valid_output(
            request.struct_size,
            request.abi_version,
            size_of::<ml_search_query_request_v1>(),
        );
        if status != ML_STATUS_OK {
            return status;
        }
        if request.reserved != 0 || request.query_len > ML_MAX_SEARCH_QUERY_BYTES as usize {
            return ML_STATUS_INVALID_ARGUMENT;
        }
        let query = match unsafe { copy_required_string(request.query, request.query_len) } {
            Ok(query) => query,
            Err(status) => return status,
        };
        let control = Arc::new(MutationControl::new());
        unsafe {
            submit_domain_request(
                core,
                ML_EVENT_SEARCH_PAGE,
                DomainRequest::SearchPage {
                    input: SearchPageInput {
                        expected_index_revision: request.expected_index_revision,
                        query_id: request.query_id,
                        query,
                        offset: request.offset,
                        limit: request.limit,
                    },
                    control: Arc::clone(&control),
                },
                Some(control),
                out_request_id,
            )
        }
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
        let payload = match state.pending_requests.get(&request_id) {
            Some(PendingRequest::Test { payload }) => payload.clone(),
            _ => return Action::status(ML_STATUS_NOT_FOUND),
        };
        let event = OwnedEvent::new(request_id, u32::MAX, ML_STATUS_OK, 1, payload);
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
