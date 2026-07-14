use std::io;
use std::num::{NonZeroU64, NonZeroUsize};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TrySendError};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::library::{CoursePage, LibraryDatabase, LibraryError};

#[derive(Debug)]
pub(crate) enum DomainRequest {
    OpenSnapshot {
        path: PathBuf,
    },
    CoursePage {
        expected_revision: u64,
        offset: u64,
        limit: u32,
    },
    #[cfg(test)]
    LongQuery {
        entered: mpsc::Sender<()>,
    },
}

#[derive(Debug)]
pub(crate) enum DomainResponse {
    LibraryOpened {
        revision: u64,
        library_path: Option<String>,
    },
    CoursePage(CoursePage),
}

#[derive(Debug)]
pub(crate) enum DomainError {
    Library(LibraryError),
    LibraryNotOpen,
    RevisionExhausted,
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SubmitError {
    Full,
    Closed,
}

struct DomainCommand {
    request_id: NonZeroU64,
    request: DomainRequest,
}

pub(crate) struct DomainCoordinator {
    command_sender: Option<SyncSender<DomainCommand>>,
    outcome_receiver: Option<Receiver<DomainOutcome>>,
    stopping: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl DomainCoordinator {
    pub(crate) fn start(capacity: NonZeroUsize) -> io::Result<Self> {
        let runtime = tokio::runtime::Builder::new_current_thread().build()?;
        let (command_sender, command_receiver) = mpsc::sync_channel(capacity.get());
        let (outcome_sender, outcome_receiver) = mpsc::sync_channel(capacity.get());
        let stopping = Arc::new(AtomicBool::new(false));
        let worker_stopping = Arc::clone(&stopping);
        let worker = thread::Builder::new()
            .name("melearner-domain".to_string())
            .spawn(move || {
                run_worker(runtime, command_receiver, outcome_sender, worker_stopping);
            })?;
        Ok(Self {
            command_sender: Some(command_sender),
            outcome_receiver: Some(outcome_receiver),
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

    pub(crate) fn recv_timeout(
        &self,
        timeout: Duration,
    ) -> Result<DomainOutcome, RecvTimeoutError> {
        self.outcome_receiver
            .as_ref()
            .ok_or(RecvTimeoutError::Disconnected)?
            .recv_timeout(timeout)
    }
}

impl Drop for DomainCoordinator {
    fn drop(&mut self) {
        self.stopping.store(true, Ordering::Release);
        self.command_sender.take();
        self.outcome_receiver.take();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

struct DomainState {
    library: Option<LibraryDatabase>,
    revision: u64,
    stopping: Arc<AtomicBool>,
}

impl DomainState {
    fn new(stopping: Arc<AtomicBool>) -> Self {
        Self {
            library: None,
            revision: 0,
            stopping,
        }
    }

    async fn execute(&mut self, request: DomainRequest) -> Result<DomainResponse, DomainError> {
        match request {
            DomainRequest::OpenSnapshot { path } => self.open_snapshot(path).await,
            DomainRequest::CoursePage {
                expected_revision,
                offset,
                limit,
            } => {
                let library = self.library.as_mut().ok_or(DomainError::LibraryNotOpen)?;
                Ok(DomainResponse::CoursePage(
                    library
                        .course_page(expected_revision, offset, limit)
                        .await?,
                ))
            }
            #[cfg(test)]
            DomainRequest::LongQuery { entered } => {
                let library = self.library.as_mut().ok_or(DomainError::LibraryNotOpen)?;
                library.run_until_interrupted(entered).await?;
                Err(DomainError::LibraryNotOpen)
            }
        }
    }

    async fn open_snapshot(&mut self, path: PathBuf) -> Result<DomainResponse, DomainError> {
        let revision = self
            .revision
            .checked_add(1)
            .and_then(NonZeroU64::new)
            .ok_or(DomainError::RevisionExhausted)?;
        let candidate = LibraryDatabase::open_snapshot_read_only_interruptible(
            &path,
            revision,
            Arc::clone(&self.stopping),
        )
        .await?;
        let library_path = candidate.library_path().map(str::to_owned);
        let previous = self.library.replace(candidate);
        self.revision = revision.get();
        drop(previous);
        Ok(DomainResponse::LibraryOpened {
            revision: revision.get(),
            library_path,
        })
    }
}

fn run_worker(
    runtime: tokio::runtime::Runtime,
    command_receiver: Receiver<DomainCommand>,
    outcome_sender: SyncSender<DomainOutcome>,
    stopping: Arc<AtomicBool>,
) {
    let mut state = DomainState::new(Arc::clone(&stopping));
    while let Ok(command) = command_receiver.recv() {
        if stopping.load(Ordering::Acquire) {
            break;
        }
        let result = runtime.block_on(state.execute(command.request));
        if outcome_sender
            .send(DomainOutcome {
                request_id: command.request_id,
                result,
            })
            .is_err()
        {
            break;
        }
    }
    if let Some(library) = state.library.take() {
        let _ = runtime.block_on(library.close());
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::num::{NonZeroU64, NonZeroUsize};
    use std::path::{Path, PathBuf};
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    use super::{DomainCoordinator, DomainError, DomainRequest, DomainResponse, SubmitError};
    use crate::library::LibraryError;

    fn request_id(value: u64) -> NonZeroU64 {
        NonZeroU64::new(value).expect("nonzero test request id")
    }

    fn copied_fixture() -> (tempfile::TempDir, PathBuf) {
        let temp = tempfile::tempdir().expect("create coordinator fixture tempdir");
        let copied = temp.path().join("database-v16.sqlite");
        fs::copy(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../fixtures/parity/database-v16.sqlite"),
            &copied,
        )
        .expect("copy v16 database fixture");
        (temp, copied)
    }

    fn coordinator(capacity: usize) -> DomainCoordinator {
        DomainCoordinator::start(NonZeroUsize::new(capacity).expect("nonzero test capacity"))
            .expect("start domain coordinator")
    }

    fn assert_no_sidecars(path: &Path) {
        for suffix in ["-wal", "-shm", "-journal"] {
            assert!(!PathBuf::from(format!("{}{suffix}", path.display())).exists());
        }
    }

    #[test]
    fn requests_run_in_fifo_order_with_caller_owned_ids() {
        let (_temp, path) = copied_fixture();
        let coordinator = coordinator(4);
        coordinator
            .try_submit(
                request_id(11),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit page before open");
        coordinator
            .try_submit(
                request_id(12),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit snapshot open");
        coordinator
            .try_submit(
                request_id(13),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit page after open");

        let before_open = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive page-before-open outcome");
        assert_eq!(before_open.request_id, request_id(11));
        assert!(matches!(
            before_open.result,
            Err(DomainError::LibraryNotOpen)
        ));

        let opened = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive open outcome");
        assert_eq!(opened.request_id, request_id(12));
        assert!(matches!(
            opened.result,
            Ok(DomainResponse::LibraryOpened {
                revision: 1,
                library_path: Some(ref library_path)
            }) if library_path == "/fixtures/library"
        ));

        let page = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive page-after-open outcome");
        assert_eq!(page.request_id, request_id(13));
        assert!(matches!(
            page.result,
            Ok(DomainResponse::CoursePage(ref page))
                if page.revision == 1 && page.total == 3 && page.rows.len() == 1
        ));
        drop(coordinator);
        assert_no_sidecars(&path);
    }

    #[test]
    fn replacement_advances_once_and_stales_later_fifo_pages() {
        let (_temp, path) = copied_fixture();
        let coordinator = coordinator(4);
        coordinator
            .try_submit(
                request_id(21),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit first open");
        coordinator
            .try_submit(
                request_id(22),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit page before replacement");
        coordinator
            .try_submit(
                request_id(23),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit replacement open");
        coordinator
            .try_submit(
                request_id(24),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit stale page");

        let first = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive first open");
        assert!(matches!(
            first.result,
            Ok(DomainResponse::LibraryOpened { revision: 1, .. })
        ));
        let before_replacement = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive page before replacement");
        assert!(matches!(
            before_replacement.result,
            Ok(DomainResponse::CoursePage(ref page)) if page.revision == 1
        ));
        let second = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive replacement open");
        assert!(matches!(
            second.result,
            Ok(DomainResponse::LibraryOpened { revision: 2, .. })
        ));
        let stale = coordinator
            .recv_timeout(Duration::from_secs(2))
            .expect("receive stale page");
        assert!(matches!(
            stale.result,
            Err(DomainError::Library(LibraryError::StaleRevision {
                expected: 1,
                actual: 2
            }))
        ));
    }

    #[test]
    fn failed_replacement_preserves_the_open_snapshot_and_revision() {
        let (_temp, path) = copied_fixture();
        let missing = path.with_file_name("missing.sqlite");
        let coordinator = coordinator(5);
        coordinator
            .try_submit(
                request_id(31),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit first open");
        coordinator
            .try_submit(
                request_id(32),
                DomainRequest::OpenSnapshot { path: missing },
            )
            .expect("submit failed replacement");
        coordinator
            .try_submit(
                request_id(33),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit page after failed replacement");
        coordinator
            .try_submit(
                request_id(34),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit successful replacement");

        assert!(matches!(
            coordinator
                .recv_timeout(Duration::from_secs(2))
                .expect("receive first open")
                .result,
            Ok(DomainResponse::LibraryOpened { revision: 1, .. })
        ));
        assert!(matches!(
            coordinator
                .recv_timeout(Duration::from_secs(2))
                .expect("receive failed replacement")
                .result,
            Err(DomainError::Library(LibraryError::Database(_)))
        ));
        assert!(matches!(
            coordinator
                .recv_timeout(Duration::from_secs(2))
                .expect("receive preserved page")
                .result,
            Ok(DomainResponse::CoursePage(ref page)) if page.revision == 1
        ));
        assert!(matches!(
            coordinator
                .recv_timeout(Duration::from_secs(2))
                .expect("receive successful replacement")
                .result,
            Ok(DomainResponse::LibraryOpened { revision: 2, .. })
        ));
    }

    #[test]
    fn bounded_submission_reports_full_and_drop_unblocks_the_worker() {
        let coordinator = coordinator(1);
        coordinator
            .try_submit(
                request_id(41),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            )
            .expect("submit first result");

        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        let mut next_request_id = 42;
        for accepted in 0..2 {
            loop {
                let result = coordinator.try_submit(
                    request_id(next_request_id),
                    DomainRequest::CoursePage {
                        expected_revision: 1,
                        offset: 0,
                        limit: 1,
                    },
                );
                next_request_id += 1;
                match result {
                    Ok(()) => break,
                    Err(SubmitError::Full) => {
                        assert!(
                            std::time::Instant::now() < deadline,
                            "worker did not open command slot {accepted}"
                        );
                        thread::yield_now();
                    }
                    Err(SubmitError::Closed) => panic!("coordinator closed during submission"),
                }
            }
        }
        assert_eq!(
            coordinator.try_submit(
                request_id(next_request_id),
                DomainRequest::CoursePage {
                    expected_revision: 1,
                    offset: 0,
                    limit: 1,
                },
            ),
            Err(SubmitError::Full),
            "third queued command proves the worker is blocked on the full outcome queue"
        );

        let (dropped, dropped_receiver) = mpsc::channel();
        thread::spawn(move || {
            drop(coordinator);
            let _ = dropped.send(());
        });
        dropped_receiver
            .recv_timeout(Duration::from_secs(2))
            .expect("coordinator drop must not hang under backpressure");
    }

    #[test]
    fn drop_is_bounded_while_a_worker_operation_is_blocked() {
        let (_temp, path) = copied_fixture();
        let coordinator = coordinator(2);
        coordinator
            .try_submit(
                request_id(51),
                DomainRequest::OpenSnapshot { path: path.clone() },
            )
            .expect("submit snapshot open");
        assert!(matches!(
            coordinator
                .recv_timeout(Duration::from_secs(2))
                .expect("receive snapshot open")
                .result,
            Ok(DomainResponse::LibraryOpened { revision: 1, .. })
        ));

        let (entered, entered_receiver) = mpsc::channel();
        coordinator
            .try_submit(request_id(52), DomainRequest::LongQuery { entered })
            .expect("submit blocked operation");
        entered_receiver
            .recv_timeout(Duration::from_secs(2))
            .expect("worker must enter blocked operation");

        let (dropped, dropped_receiver) = mpsc::channel();
        thread::spawn(move || {
            drop(coordinator);
            let _ = dropped.send(());
        });
        dropped_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("coordinator drop must not wait for blocked worker I/O");
        assert_no_sidecars(&path);
    }
}
