use std::io;
use std::num::{NonZeroU64, NonZeroUsize};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};
use std::thread::{self, JoinHandle};

use crate::library::{CoursePage, LessonPage, LibraryDatabase, LibraryError};

#[derive(Debug)]
pub(crate) enum DomainRequest {
    CoursePage {
        expected_revision: u64,
        offset: u64,
        limit: u32,
    },
    LessonPage {
        expected_revision: u64,
        course_id: String,
        section_id: Option<String>,
        offset: u64,
        limit: u32,
    },
    #[cfg(test)]
    LongQuery { entered: mpsc::Sender<()> },
    #[cfg(test)]
    Panic,
}

#[derive(Debug)]
pub(crate) enum DomainResponse {
    CoursePage(CoursePage),
    LessonPage(LessonPage),
}

#[derive(Debug)]
pub(crate) enum DomainError {
    Library(LibraryError),
    WorkerPanicked,
}

impl From<LibraryError> for DomainError {
    fn from(error: LibraryError) -> Self {
        Self::Library(error)
    }
}

#[derive(Debug)]
pub(crate) struct DomainOutcome {
    pub(crate) request_id: NonZeroU64,
    pub(crate) result: Result<DomainResponse, DomainError>,
}

#[derive(Debug)]
pub(crate) enum DomainEvent {
    Ready { revision: u64 },
    Completed(DomainOutcome),
    Fatal(DomainError),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SubmitError {
    Full,
    Closed,
}

pub(crate) type DomainEventSink = Arc<dyn Fn(DomainEvent) -> bool + Send + Sync>;

struct DomainCommand {
    request_id: NonZeroU64,
    request: DomainRequest,
}

pub(crate) struct DomainWorker {
    command_sender: Option<SyncSender<DomainCommand>>,
    stopping: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl DomainWorker {
    pub(crate) fn start(
        data_dir: PathBuf,
        revision: NonZeroU64,
        capacity: NonZeroUsize,
        event_sink: DomainEventSink,
    ) -> io::Result<Self> {
        let runtime = tokio::runtime::Builder::new_current_thread().build()?;
        let (command_sender, command_receiver) = mpsc::sync_channel(capacity.get());
        let stopping = Arc::new(AtomicBool::new(false));
        let worker_stopping = Arc::clone(&stopping);
        let panic_sink = Arc::clone(&event_sink);
        let worker = thread::Builder::new()
            .name("melearner-domain".to_string())
            .spawn(move || {
                if catch_unwind(AssertUnwindSafe(|| {
                    run_worker(
                        runtime,
                        data_dir,
                        revision,
                        command_receiver,
                        event_sink,
                        worker_stopping,
                    );
                }))
                .is_err()
                {
                    panic_sink(DomainEvent::Fatal(DomainError::WorkerPanicked));
                }
            })?;
        Ok(Self {
            command_sender: Some(command_sender),
            stopping,
            worker: Some(worker),
        })
    }

    pub(crate) fn try_submit(
        &self,
        request_id: NonZeroU64,
        request: DomainRequest,
    ) -> Result<(), SubmitError> {
        let sender = self.command_sender.as_ref().ok_or(SubmitError::Closed)?;
        match sender.try_send(DomainCommand {
            request_id,
            request,
        }) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) => Err(SubmitError::Full),
            Err(TrySendError::Disconnected(_)) => Err(SubmitError::Closed),
        }
    }
}

impl Drop for DomainWorker {
    fn drop(&mut self) {
        self.stopping.store(true, Ordering::Release);
        self.command_sender.take();
        if let Some(worker) = self.worker.take()
            && worker.thread().id() != thread::current().id()
        {
            let _ = worker.join();
        }
    }
}

struct DomainState {
    library: LibraryDatabase,
}

impl DomainState {
    async fn open(
        data_dir: PathBuf,
        revision: NonZeroU64,
        stopping: Arc<AtomicBool>,
    ) -> Result<Self, DomainError> {
        let library = LibraryDatabase::open_current(&data_dir, revision, stopping).await?;
        Ok(Self { library })
    }

    async fn execute(&mut self, request: DomainRequest) -> Result<DomainResponse, DomainError> {
        match request {
            DomainRequest::CoursePage {
                expected_revision,
                offset,
                limit,
            } => Ok(DomainResponse::CoursePage(
                self.library
                    .course_page(expected_revision, offset, limit)
                    .await?,
            )),
            DomainRequest::LessonPage {
                expected_revision,
                course_id,
                section_id,
                offset,
                limit,
            } => Ok(DomainResponse::LessonPage(
                self.library
                    .lesson_page(
                        expected_revision,
                        &course_id,
                        section_id.as_deref(),
                        offset,
                        limit,
                    )
                    .await?,
            )),
            #[cfg(test)]
            DomainRequest::LongQuery { entered } => {
                self.library.run_until_interrupted(entered).await?;
                unreachable!("interruptible query returned without shutdown")
            }
            #[cfg(test)]
            DomainRequest::Panic => panic!("forced domain worker panic"),
        }
    }
}

fn run_worker(
    runtime: tokio::runtime::Runtime,
    data_dir: PathBuf,
    revision: NonZeroU64,
    command_receiver: Receiver<DomainCommand>,
    event_sink: DomainEventSink,
    stopping: Arc<AtomicBool>,
) {
    let mut state =
        match runtime.block_on(DomainState::open(data_dir, revision, Arc::clone(&stopping))) {
            Ok(state) => state,
            Err(error) => {
                event_sink(DomainEvent::Fatal(error));
                return;
            }
        };
    if !event_sink(DomainEvent::Ready {
        revision: state.library.revision(),
    }) {
        let _ = runtime.block_on(state.library.close());
        return;
    }
    while let Ok(command) = command_receiver.recv() {
        if stopping.load(Ordering::Acquire) {
            break;
        }
        let result = runtime.block_on(state.execute(command.request));
        if !event_sink(DomainEvent::Completed(DomainOutcome {
            request_id: command.request_id,
            result,
        })) {
            break;
        }
    }
    let _ = runtime.block_on(state.library.close());
}

#[cfg(test)]
mod tests {
    use std::num::{NonZeroU64, NonZeroUsize};
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, mpsc};
    use std::thread;
    use std::time::Duration;

    use sqlx::sqlite::SqliteConnectOptions;
    use sqlx::{Connection, SqliteConnection};

    use super::{
        DomainError, DomainEvent, DomainEventSink, DomainOutcome, DomainRequest, DomainResponse,
        DomainWorker, SubmitError,
    };
    use crate::library::{LibraryError, NATIVE_DATABASE_FILENAME};

    const CURRENT_SEED: &str = include_str!("../../../fixtures/parity/database-current.sql");

    fn request_id(value: u64) -> NonZeroU64 {
        NonZeroU64::new(value).expect("nonzero test request id")
    }

    struct WorkerHarness {
        worker: DomainWorker,
        event_receiver: mpsc::Receiver<DomainEvent>,
    }

    impl WorkerHarness {
        fn try_submit(
            &self,
            request_id: NonZeroU64,
            request: DomainRequest,
        ) -> Result<(), SubmitError> {
            self.worker.try_submit(request_id, request)
        }

        fn recv_timeout(&self, timeout: Duration) -> Result<DomainEvent, mpsc::RecvTimeoutError> {
            self.event_receiver.recv_timeout(timeout)
        }
    }

    fn start(data_dir: &Path, capacity: usize) -> WorkerHarness {
        let (event_sender, event_receiver) = mpsc::channel();
        let event_sink: DomainEventSink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = DomainWorker::start(
            data_dir.to_path_buf(),
            NonZeroU64::new(1).expect("nonzero test revision"),
            NonZeroUsize::new(capacity).expect("nonzero test capacity"),
            event_sink,
        )
        .expect("start native core");
        WorkerHarness {
            worker,
            event_receiver,
        }
    }

    fn receive_ready(core: &WorkerHarness) -> u64 {
        match core
            .recv_timeout(Duration::from_secs(2))
            .expect("receive core ready event")
        {
            DomainEvent::Ready { revision } => revision,
            other => panic!("expected core ready event, received {other:?}"),
        }
    }

    fn native_database_path(data_dir: &Path) -> PathBuf {
        data_dir.join(NATIVE_DATABASE_FILENAME)
    }

    fn seed_current_database(path: &Path) {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build seed runtime")
            .block_on(async {
                let options = SqliteConnectOptions::new()
                    .filename(path)
                    .foreign_keys(true)
                    .busy_timeout(Duration::from_secs(10));
                let mut connection = SqliteConnection::connect_with(&options)
                    .await
                    .expect("open current native database for seeding");
                sqlx::raw_sql(CURRENT_SEED)
                    .execute(&mut connection)
                    .await
                    .expect("seed current native database");
                connection
                    .close()
                    .await
                    .expect("close seeded native database");
            });
    }

    fn current_tables(path: &Path) -> Vec<String> {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build table inspection runtime")
            .block_on(async {
                let options = SqliteConnectOptions::new().filename(path).read_only(true);
                let mut connection = SqliteConnection::connect_with(&options)
                    .await
                    .expect("open native database for table inspection");
                let tables = sqlx::query_scalar::<_, String>(
                    "SELECT name
                     FROM sqlite_schema
                     WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
                     ORDER BY name",
                )
                .fetch_all(&mut connection)
                .await
                .expect("read native database tables");
                connection
                    .close()
                    .await
                    .expect("close inspected native database");
                tables
            })
    }

    #[test]
    fn fresh_native_core_uses_only_its_current_database() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let previous_database = data_dir.path().join("melearner.db");
        std::fs::write(&previous_database, b"previous database sentinel")
            .expect("write previous database sentinel");

        let core = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&core), 1);
        core.try_submit(
            request_id(1),
            DomainRequest::CoursePage {
                expected_revision: 1,
                offset: 0,
                limit: 20,
            },
        )
        .expect("submit empty Library page");
        assert!(matches!(
            core.recv_timeout(Duration::from_secs(2))
                .expect("receive empty Library page"),
            DomainEvent::Completed(DomainOutcome {
                request_id: outcome_request_id,
                result: Ok(DomainResponse::CoursePage(page)),
            }) if outcome_request_id == request_id(1)
                && page.revision == 1
                && page.total == 0
                && page.rows.is_empty()
        ));
        drop(core);

        let native_database = native_database_path(data_dir.path());
        assert!(native_database.is_file());
        assert_eq!(
            current_tables(&native_database),
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
            std::fs::read(previous_database).expect("read previous database sentinel"),
            b"previous database sentinel"
        );
    }

    #[test]
    fn restart_reopens_the_same_current_database() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let core = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&core), 1);
        drop(core);
        seed_current_database(&native_database_path(data_dir.path()));

        let restarted = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&restarted), 1);
        restarted
            .try_submit(
                request_id(2),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 20,
                },
            )
            .expect("submit restarted Library page");
        assert!(matches!(
            restarted
                .recv_timeout(Duration::from_secs(2))
                .expect("receive restarted Library page"),
            DomainEvent::Completed(DomainOutcome {
                request_id: outcome_request_id,
                result: Ok(DomainResponse::CoursePage(page)),
            }) if outcome_request_id == request_id(2)
                && page.revision == 1
                && page.total == 3
                && page.rows.len() == 3
        ));
    }

    #[test]
    fn noncurrent_native_database_fails_without_repair() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let database_path = native_database_path(data_dir.path());
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build obsolete database runtime")
            .block_on(async {
                let options = SqliteConnectOptions::new()
                    .filename(&database_path)
                    .create_if_missing(true);
                let mut connection = SqliteConnection::connect_with(&options)
                    .await
                    .expect("create noncurrent native database");
                sqlx::query("CREATE TABLE obsolete_data (value TEXT NOT NULL)")
                    .execute(&mut connection)
                    .await
                    .expect("create obsolete table");
                connection
                    .close()
                    .await
                    .expect("close noncurrent native database");
            });
        let original_database = std::fs::read(&database_path).expect("read noncurrent database");

        let core = start(data_dir.path(), 1);
        assert!(matches!(
            core.recv_timeout(Duration::from_secs(2))
                .expect("receive fatal startup event"),
            DomainEvent::Fatal(DomainError::Library(LibraryError::Database(message)))
                if message == "database schema is not current"
        ));
        drop(core);
        assert!(
            std::fs::read(&database_path).expect("reread noncurrent database") == original_database,
            "noncurrent database bytes changed during rejection"
        );
        for suffix in ["-wal", "-shm", "-journal"] {
            assert!(
                !data_dir
                    .path()
                    .join(format!("{NATIVE_DATABASE_FILENAME}{suffix}"))
                    .exists()
            );
        }
        assert_eq!(current_tables(&database_path), ["obsolete_data"]);
    }

    #[test]
    fn same_state_directory_rejects_a_second_live_core() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let first = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&first), 1);

        let second = start(data_dir.path(), 4);
        assert!(matches!(
            second
                .recv_timeout(Duration::from_secs(2))
                .expect("receive ownership failure"),
            DomainEvent::Fatal(DomainError::Library(LibraryError::Database(message)))
                if message == "native database is already open"
        ));
        drop(second);
        assert!(native_database_path(data_dir.path()).is_file());

        first
            .try_submit(
                request_id(3),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 20,
                },
            )
            .expect("submit page to first core");
        assert!(matches!(
            first
                .recv_timeout(Duration::from_secs(2))
                .expect("receive page from first core"),
            DomainEvent::Completed(DomainOutcome {
                request_id: outcome_request_id,
                result: Ok(DomainResponse::CoursePage(page)),
            }) if outcome_request_id == request_id(3) && page.revision == 1
        ));
        drop(first);

        let reopened = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&reopened), 1);
    }

    #[test]
    fn requests_complete_in_fifo_order_and_reject_stale_pages() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let core = start(data_dir.path(), 4);
        assert_eq!(receive_ready(&core), 1);

        core.try_submit(
            request_id(11),
            DomainRequest::CoursePage {
                expected_revision: 2,
                offset: 0,
                limit: 20,
            },
        )
        .expect("submit stale page");
        core.try_submit(
            request_id(12),
            DomainRequest::CoursePage {
                expected_revision: 1,
                offset: 0,
                limit: 20,
            },
        )
        .expect("submit current page");

        assert!(matches!(
            core.recv_timeout(Duration::from_secs(2))
                .expect("receive stale page"),
            DomainEvent::Completed(DomainOutcome {
                request_id: outcome_request_id,
                result: Err(DomainError::Library(LibraryError::StaleRevision {
                    expected: 2,
                    actual: 1,
                })),
            }) if outcome_request_id == request_id(11)
        ));
        assert!(matches!(
            core.recv_timeout(Duration::from_secs(2))
                .expect("receive current page"),
            DomainEvent::Completed(DomainOutcome {
                request_id: outcome_request_id,
                result: Ok(DomainResponse::CoursePage(page)),
            }) if outcome_request_id == request_id(12) && page.revision == 1
        ));
    }

    #[test]
    fn bounded_submission_reports_full_and_drop_unblocks_the_worker() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let core = start(data_dir.path(), 1);
        assert_eq!(receive_ready(&core), 1);

        core.try_submit(
            request_id(21),
            DomainRequest::CoursePage {
                expected_revision: 1,
                offset: 0,
                limit: 1,
            },
        )
        .expect("submit first request");
        let deadline = std::time::Instant::now() + Duration::from_secs(2);
        let mut next_id = 22;
        loop {
            match core.try_submit(
                request_id(next_id),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            ) {
                Ok(()) => next_id += 1,
                Err(SubmitError::Full) => break,
                Err(SubmitError::Closed) => panic!("native core closed during submission"),
            }
            assert!(
                std::time::Instant::now() < deadline,
                "bounded command queue never filled"
            );
            thread::yield_now();
        }

        let (dropped, dropped_receiver) = mpsc::channel();
        thread::spawn(move || {
            drop(core);
            let _ = dropped.send(());
        });
        dropped_receiver
            .recv_timeout(Duration::from_secs(2))
            .expect("native core drop must unblock backpressure");
    }

    #[test]
    fn drop_interrupts_an_active_database_operation() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let core = start(data_dir.path(), 2);
        assert_eq!(receive_ready(&core), 1);
        let (entered, entered_receiver) = mpsc::channel();
        core.try_submit(request_id(31), DomainRequest::LongQuery { entered })
            .expect("submit long database operation");
        entered_receiver
            .recv_timeout(Duration::from_secs(2))
            .expect("database operation must start");

        let (dropped, dropped_receiver) = mpsc::channel();
        thread::spawn(move || {
            drop(core);
            let _ = dropped.send(());
        });
        dropped_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("native core drop must interrupt database work");
    }

    #[test]
    fn worker_panic_becomes_one_fatal_event() {
        let data_dir = tempfile::tempdir().expect("create native data directory");
        let core = start(data_dir.path(), 2);
        assert_eq!(receive_ready(&core), 1);
        core.try_submit(request_id(41), DomainRequest::Panic)
            .expect("submit panicking request");

        assert!(matches!(
            core.recv_timeout(Duration::from_secs(2))
                .expect("receive worker fatal event"),
            DomainEvent::Fatal(DomainError::WorkerPanicked)
        ));
        assert!(matches!(
            core.recv_timeout(Duration::from_millis(20)),
            Err(mpsc::RecvTimeoutError::Timeout | mpsc::RecvTimeoutError::Disconnected)
        ));
        assert_eq!(
            core.try_submit(
                request_id(42),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            ),
            Err(SubmitError::Closed)
        );
    }
}
