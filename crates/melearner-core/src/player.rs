use libmpv2::Format;
use libmpv2::events::{Event, PropertyData, mpv_event_id};
use libmpv2::mpv_end_file_reason;
use libmpv2_sys as mpv_sys;
use std::ffi::{CStr, CString, OsString};
use std::fmt;
use std::io;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::num::{NonZeroU64, NonZeroUsize};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError, SyncSender, TrySendError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

static SCREENSHOT_COUNTER: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) enum PlayerError {
    ApprovedRootsEmpty,
    ApprovedRootUnavailable {
        path: PathBuf,
        detail: String,
    },
    ApprovedRootNotDirectory {
        path: PathBuf,
    },
    InvalidLocalPath {
        path: PathBuf,
    },
    MediaMissing {
        path: PathBuf,
    },
    MediaUnavailable {
        path: PathBuf,
        detail: String,
    },
    MediaNotFile {
        path: PathBuf,
    },
    MediaOutsideApprovedRoots {
        path: PathBuf,
    },
    MediaPlaybackFailed {
        path: PathBuf,
        detail: String,
    },
    InvalidResumePosition,
    InvalidSeekTarget,
    InvalidVolume,
    InvalidPlaybackRate,
    InvalidTrackId,
    TrackUnavailable {
        kind: &'static str,
        id: i64,
    },
    InvalidChapterId,
    ChapterUnavailable {
        id: i64,
    },
    ScreenshotDirectoryUnavailable,
    ScreenshotDirectoryCreate {
        path: PathBuf,
        detail: String,
    },
    ScreenshotClock {
        detail: String,
    },
    NoMediaLoaded,
    EngineInitialization {
        detail: String,
    },
    EngineCommand {
        operation: &'static str,
        detail: String,
    },
    EngineEvent {
        detail: String,
    },
    EngineEventQueueOverflow,
    WorkerStart {
        detail: String,
    },
}

impl fmt::Display for PlayerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ApprovedRootsEmpty => formatter.write_str("approved roots must not be empty"),
            Self::ApprovedRootUnavailable { path, detail } => {
                write!(
                    formatter,
                    "approved root is unavailable: {}: {detail}",
                    path.display()
                )
            }
            Self::ApprovedRootNotDirectory { path } => {
                write!(
                    formatter,
                    "approved root is not a directory: {}",
                    path.display()
                )
            }
            Self::InvalidLocalPath { path } => {
                write!(
                    formatter,
                    "media path is not a local path: {}",
                    path.display()
                )
            }
            Self::MediaMissing { path } => {
                write!(formatter, "media is missing: {}", path.display())
            }
            Self::MediaUnavailable { path, detail } => {
                write!(
                    formatter,
                    "media is unavailable: {}: {detail}",
                    path.display()
                )
            }
            Self::MediaNotFile { path } => {
                write!(formatter, "media path is not a file: {}", path.display())
            }
            Self::MediaOutsideApprovedRoots { path } => {
                write!(
                    formatter,
                    "media is outside the approved roots: {}",
                    path.display()
                )
            }
            Self::MediaPlaybackFailed { path, detail } => {
                write!(
                    formatter,
                    "media playback failed: {}: {detail}",
                    path.display()
                )
            }
            Self::InvalidResumePosition => {
                formatter.write_str("resume position must be finite and non-negative")
            }
            Self::InvalidSeekTarget => formatter.write_str("seek target must be finite"),
            Self::InvalidVolume => formatter.write_str("volume must be finite"),
            Self::InvalidPlaybackRate => {
                formatter.write_str("playback rate must be between 0.25 and 4.0")
            }
            Self::InvalidTrackId => formatter.write_str("track id must not be negative"),
            Self::TrackUnavailable { kind, id } => {
                write!(formatter, "{kind} track id is not available: {id}")
            }
            Self::InvalidChapterId => formatter.write_str("chapter id must not be negative"),
            Self::ChapterUnavailable { id } => {
                write!(formatter, "chapter id is not available: {id}")
            }
            Self::ScreenshotDirectoryUnavailable => {
                formatter.write_str("screenshot directory is unavailable")
            }
            Self::ScreenshotDirectoryCreate { path, detail } => {
                write!(
                    formatter,
                    "screenshot directory could not be created: {}: {detail}",
                    path.display()
                )
            }
            Self::ScreenshotClock { detail } => {
                write!(formatter, "screenshot timestamp is unavailable: {detail}")
            }
            Self::NoMediaLoaded => formatter.write_str("no media is loaded"),
            Self::EngineInitialization { detail } => {
                write!(formatter, "embedded libmpv could not initialize: {detail}")
            }
            Self::EngineCommand { operation, detail } => {
                write!(formatter, "embedded libmpv could not {operation}: {detail}")
            }
            Self::EngineEvent { detail } => {
                write!(formatter, "embedded libmpv event failed: {detail}")
            }
            Self::EngineEventQueueOverflow => {
                formatter.write_str("embedded libmpv event queue overflowed")
            }
            Self::WorkerStart { detail } => {
                write!(formatter, "Player worker could not start: {detail}")
            }
        }
    }
}

impl std::error::Error for PlayerError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct ApprovedRoots {
    canonical: Vec<PathBuf>,
}

impl ApprovedRoots {
    pub(crate) fn new<I, P>(roots: I) -> Result<Self, PlayerError>
    where
        I: IntoIterator<Item = P>,
        P: AsRef<Path>,
    {
        let canonical = roots
            .into_iter()
            .map(|root| {
                let root = root.as_ref();
                let canonical =
                    root.canonicalize()
                        .map_err(|error| PlayerError::ApprovedRootUnavailable {
                            path: root.to_path_buf(),
                            detail: error.to_string(),
                        })?;
                if !canonical.is_dir() {
                    return Err(PlayerError::ApprovedRootNotDirectory { path: canonical });
                }
                Ok(canonical)
            })
            .collect::<Result<Vec<_>, _>>()?;
        if canonical.is_empty() {
            return Err(PlayerError::ApprovedRootsEmpty);
        }
        Ok(Self { canonical })
    }

    pub(crate) fn resolve_media(&self, path: impl AsRef<Path>) -> Result<PathBuf, PlayerError> {
        let path = path.as_ref();
        if path.as_os_str().is_empty() || looks_like_url_or_scheme(path) {
            return Err(PlayerError::InvalidLocalPath {
                path: path.to_path_buf(),
            });
        }
        let canonical = path.canonicalize().map_err(|error| {
            if error.kind() == io::ErrorKind::NotFound {
                PlayerError::MediaMissing {
                    path: path.to_path_buf(),
                }
            } else {
                PlayerError::MediaUnavailable {
                    path: path.to_path_buf(),
                    detail: error.to_string(),
                }
            }
        })?;
        if !canonical.is_file() {
            return Err(PlayerError::MediaNotFile { path: canonical });
        }
        if !self
            .canonical
            .iter()
            .any(|root| canonical.starts_with(root))
        {
            return Err(PlayerError::MediaOutsideApprovedRoots { path: canonical });
        }
        Ok(canonical)
    }
}

fn looks_like_url_or_scheme(path: &Path) -> bool {
    path.to_str()
        .is_some_and(|path| path.contains("://") || path.starts_with("file:"))
}

fn screenshot_root_from(
    mut environment: impl FnMut(&str) -> Option<OsString>,
    app_data_fallback: Option<&Path>,
) -> Result<PathBuf, PlayerError> {
    if let Some(path) = environment("MELEARNER_SCREENSHOT_DIR")
        && !path.to_string_lossy().trim().is_empty()
    {
        return Ok(PathBuf::from(path));
    }

    #[cfg(target_os = "windows")]
    if let Some(home) = environment("USERPROFILE") {
        return Ok(PathBuf::from(home).join("Pictures").join("melearner"));
    }

    if let Some(home) = environment("HOME") {
        return Ok(PathBuf::from(home).join("Pictures").join("melearner"));
    }

    app_data_fallback
        .map(|root| root.join("screenshots"))
        .ok_or(PlayerError::ScreenshotDirectoryUnavailable)
}

fn screenshot_file_stem(path: Option<&Path>) -> String {
    let source = path
        .and_then(Path::file_stem)
        .and_then(|stem| stem.to_str())
        .unwrap_or("capture");
    let mut output = String::new();
    for character in source.chars().take(64) {
        if character.is_ascii_alphanumeric() || character == '-' || character == '_' {
            output.push(character);
        } else {
            output.push('_');
        }
    }
    if output.is_empty() {
        "capture".to_string()
    } else {
        output
    }
}

fn screenshot_filename(path: Option<&Path>, timestamp: u128, counter: u64) -> String {
    format!("{}-{timestamp}-{counter}.png", screenshot_file_stem(path))
}

fn screenshot_output_path(
    media_path: Option<&Path>,
    app_data_fallback: Option<&Path>,
) -> Result<PathBuf, PlayerError> {
    let root = screenshot_root_from(|name| std::env::var_os(name), app_data_fallback)?;
    screenshot_output_path_in_root(media_path, &root)
}

fn screenshot_output_path_in_root(
    media_path: Option<&Path>,
    root: &Path,
) -> Result<PathBuf, PlayerError> {
    std::fs::create_dir_all(root).map_err(|error| PlayerError::ScreenshotDirectoryCreate {
        path: root.to_path_buf(),
        detail: error.to_string(),
    })?;
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| PlayerError::ScreenshotClock {
            detail: error.to_string(),
        })?
        .as_nanos();
    let counter = SCREENSHOT_COUNTER.fetch_add(1, Ordering::Relaxed);
    Ok(root.join(screenshot_filename(media_path, timestamp, counter)))
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PlayerState {
    pub(crate) path: Option<PathBuf>,
    pub(crate) paused: bool,
    pub(crate) buffering: bool,
    pub(crate) current_time: f64,
    pub(crate) duration: f64,
    pub(crate) volume: f64,
    pub(crate) muted: bool,
    pub(crate) rate: f64,
    pub(crate) width: Option<i64>,
    pub(crate) height: Option<i64>,
    pub(crate) audio_tracks: Vec<PlayerTrack>,
    pub(crate) subtitle_tracks: Vec<PlayerTrack>,
    pub(crate) selected_audio_track_id: Option<i64>,
    pub(crate) selected_subtitle_track_id: Option<i64>,
    pub(crate) chapters: Vec<PlayerChapter>,
    pub(crate) current_chapter_id: Option<i64>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct PlayerTrack {
    pub(crate) id: i64,
    pub(crate) title: Option<String>,
    pub(crate) language: Option<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PlayerChapter {
    pub(crate) id: i64,
    pub(crate) title: Option<String>,
    pub(crate) start_time: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PlayerSubtitleLoadRequest {
    pub(crate) path: PathBuf,
    pub(crate) label: Option<String>,
    pub(crate) language: Option<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PlayerLoadRequest {
    pub(crate) path: PathBuf,
    pub(crate) approved_roots: Vec<PathBuf>,
    pub(crate) subtitles: Vec<PlayerSubtitleLoadRequest>,
    pub(crate) start_time: Option<f64>,
    pub(crate) autoplay: bool,
}

impl PlayerLoadRequest {
    fn validate(mut self) -> Result<Self, PlayerError> {
        if self
            .start_time
            .is_some_and(|position| !position.is_finite() || position < 0.0)
        {
            return Err(PlayerError::InvalidResumePosition);
        }
        let approved = ApprovedRoots::new(&self.approved_roots)?;
        self.path = approved.resolve_media(&self.path)?;
        for subtitle in &mut self.subtitles {
            subtitle.path = approved.resolve_media(&subtitle.path)?;
        }
        self.approved_roots = approved.canonical;
        Ok(self)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum PlayerCommand {
    Load(PlayerLoadRequest),
    State,
    Play,
    Pause,
    Seek { seconds: f64, mode: PlayerSeekMode },
    SetVolume(f64),
    SetMuted(bool),
    SetRate(f64),
    SelectAudioTrack(Option<i64>),
    SelectSubtitleTrack(Option<i64>),
    SelectChapter(i64),
    StepFrame,
    Screenshot,
    Destroy,
}

impl PlayerCommand {
    fn validate(self) -> Result<Self, PlayerError> {
        match &self {
            Self::Load(request) => return request.clone().validate().map(Self::Load),
            Self::Seek { seconds, .. } if !seconds.is_finite() => {
                return Err(PlayerError::InvalidSeekTarget);
            }
            Self::SetVolume(volume) if !volume.is_finite() => {
                return Err(PlayerError::InvalidVolume);
            }
            Self::SetRate(rate) if !rate.is_finite() || !(0.25..=4.0).contains(rate) => {
                return Err(PlayerError::InvalidPlaybackRate);
            }
            Self::SelectAudioTrack(Some(id)) | Self::SelectSubtitleTrack(Some(id)) if *id < 0 => {
                return Err(PlayerError::InvalidTrackId);
            }
            Self::SelectChapter(id) if *id < 0 => {
                return Err(PlayerError::InvalidChapterId);
            }
            _ => {}
        }
        Ok(self)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PlayerSeekMode {
    Absolute,
    Relative,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum PlayerCommandResult {
    State(PlayerState),
    ScreenshotSaved { state: PlayerState, path: PathBuf },
}

impl PlayerCommandResult {
    fn into_state(self) -> PlayerState {
        match self {
            Self::State(state) | Self::ScreenshotSaved { state, .. } => state,
        }
    }
}

impl From<PlayerState> for PlayerCommandResult {
    fn from(state: PlayerState) -> Self {
        Self::State(state)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum PlayerEvent {
    CommandCompleted {
        request_id: NonZeroU64,
        result: Result<PlayerCommandResult, PlayerError>,
    },
    FileLoaded {
        state: PlayerState,
    },
    Position {
        state: PlayerState,
    },
    EndFile {
        state: PlayerState,
    },
    Error {
        error: PlayerError,
    },
    Shutdown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PlayerSubmitError {
    Full,
    Closed,
}

pub(crate) trait PlayerBackend: Send + 'static {
    fn execute(&mut self, command: PlayerCommand) -> Result<PlayerCommandResult, PlayerError>;

    fn poll_event(&mut self) -> Option<PlayerEvent> {
        None
    }

    fn poll_lifecycle_event(&mut self) -> Option<PlayerEvent> {
        None
    }

    fn poll_position_event(&mut self) -> Option<PlayerEvent> {
        None
    }
}

pub(crate) struct LibmpvBackend {
    mpv: libmpv2::Mpv,
    path: Option<PathBuf>,
    pending_load: Option<PendingLoad>,
    last_position_event: Instant,
    eof_reached: bool,
    screenshot_root_override: Option<PathBuf>,
    screenshot_app_data_fallback: Option<PathBuf>,
}

struct PendingLoad {
    subtitles: Vec<PlayerSubtitleLoadRequest>,
    start_time: Option<f64>,
    autoplay: bool,
}

#[derive(Default)]
struct TrackState {
    audio_tracks: Vec<PlayerTrack>,
    subtitle_tracks: Vec<PlayerTrack>,
    selected_audio_track_id: Option<i64>,
    selected_subtitle_track_id: Option<i64>,
}

struct MpvNode {
    raw: mpv_sys::mpv_node,
}

#[derive(Clone, Copy)]
struct MpvNodeRef<'a> {
    raw: *const mpv_sys::mpv_node,
    _owner: PhantomData<&'a MpvNode>,
}

impl Drop for MpvNode {
    fn drop(&mut self) {
        // SAFETY: `raw` was initialized by `mpv_get_property` with
        // `MPV_FORMAT_NODE` and remains owned by this wrapper.
        unsafe {
            mpv_sys::mpv_free_node_contents(&mut self.raw);
        }
    }
}

impl MpvNode {
    fn get(mpv: &libmpv2::Mpv, property: &str) -> Option<Self> {
        let property = CString::new(property).ok()?;
        let mut raw = MaybeUninit::<mpv_sys::mpv_node>::zeroed();
        // SAFETY: the property is a valid C string, the context is live, and
        // `raw` points to enough storage for the requested node.
        let result = unsafe {
            mpv_sys::mpv_get_property(
                mpv.ctx.as_ptr(),
                property.as_ptr(),
                mpv_sys::mpv_format_MPV_FORMAT_NODE,
                raw.as_mut_ptr().cast(),
            )
        };
        if result < 0 {
            return None;
        }
        // SAFETY: non-negative `mpv_get_property` initialized the node.
        Some(Self {
            raw: unsafe { raw.assume_init() },
        })
    }

    fn as_ref(&self) -> MpvNodeRef<'_> {
        MpvNodeRef {
            raw: &self.raw,
            _owner: PhantomData,
        }
    }
}

impl<'a> MpvNodeRef<'a> {
    fn format(self) -> mpv_sys::mpv_format {
        // SAFETY: every node reference is derived from a live `MpvNode`.
        unsafe { (*self.raw).format }
    }

    fn to_bool(self) -> Option<bool> {
        (self.format() == mpv_sys::mpv_format_MPV_FORMAT_FLAG)
            // SAFETY: the checked format selects the flag union member.
            .then(|| unsafe { (*self.raw).u.flag != 0 })
    }

    fn to_i64(self) -> Option<i64> {
        (self.format() == mpv_sys::mpv_format_MPV_FORMAT_INT64)
            // SAFETY: the checked format selects the int64 union member.
            .then(|| unsafe { (*self.raw).u.int64 })
    }

    fn to_f64(self) -> Option<f64> {
        (self.format() == mpv_sys::mpv_format_MPV_FORMAT_DOUBLE)
            // SAFETY: the checked format selects the double union member.
            .then(|| finite_or(unsafe { (*self.raw).u.double_ }, 0.0))
    }

    fn to_string(self) -> Option<String> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_STRING {
            return None;
        }
        // SAFETY: the checked format selects the string union member.
        let raw = unsafe { (*self.raw).u.string };
        if raw.is_null() {
            return None;
        }
        // SAFETY: libmpv guarantees a NUL-terminated string for this node.
        unsafe { CStr::from_ptr(raw) }
            .to_str()
            .ok()
            .map(str::to_owned)
    }

    fn array_items(self) -> Vec<Self> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_NODE_ARRAY {
            return Vec::new();
        }
        // SAFETY: the checked format selects the list union member.
        let list = unsafe { (*self.raw).u.list };
        if list.is_null() {
            return Vec::new();
        }
        // SAFETY: libmpv owns the list for the lifetime represented by `'a`.
        let list = unsafe { &*list };
        if list.num <= 0 || list.values.is_null() {
            return Vec::new();
        }
        (0..list.num)
            .map(|index| Self {
                // SAFETY: `index` is bounded by the list's reported length.
                raw: unsafe { list.values.offset(index as isize) },
                _owner: PhantomData,
            })
            .collect()
    }

    fn map_entries(self) -> Vec<(String, Self)> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_NODE_MAP {
            return Vec::new();
        }
        // SAFETY: the checked format selects the list union member.
        let list = unsafe { (*self.raw).u.list };
        if list.is_null() {
            return Vec::new();
        }
        // SAFETY: libmpv owns the list for the lifetime represented by `'a`.
        let list = unsafe { &*list };
        if list.num <= 0 || list.values.is_null() || list.keys.is_null() {
            return Vec::new();
        }
        (0..list.num)
            .filter_map(|index| {
                // SAFETY: `index` is bounded by the list's reported length.
                let key = unsafe { *list.keys.offset(index as isize) };
                if key.is_null() {
                    return None;
                }
                // SAFETY: libmpv map keys are NUL-terminated strings.
                let key = unsafe { CStr::from_ptr(key) }.to_str().ok()?.to_owned();
                Some((
                    key,
                    Self {
                        // SAFETY: `index` is bounded by the list's reported length.
                        raw: unsafe { list.values.offset(index as isize) },
                        _owner: PhantomData,
                    },
                ))
            })
            .collect()
    }
}

impl LibmpvBackend {
    pub(crate) fn new() -> Result<Self, PlayerError> {
        Self::new_with_screenshot_policy(None, None)
    }

    pub(crate) fn new_with_app_data(app_data_fallback: PathBuf) -> Result<Self, PlayerError> {
        Self::new_with_screenshot_policy(None, Some(app_data_fallback))
    }

    #[cfg(test)]
    fn new_with_screenshot_root(root: PathBuf) -> Result<Self, PlayerError> {
        Self::new_with_screenshot_policy(Some(root), None)
    }

    fn new_with_screenshot_policy(
        screenshot_root_override: Option<PathBuf>,
        screenshot_app_data_fallback: Option<PathBuf>,
    ) -> Result<Self, PlayerError> {
        configure_libmpv_numeric_locale();
        let mpv = libmpv2::Mpv::with_initializer(|init| {
            init.set_option("config", false)?;
            init.set_option("load-scripts", false)?;
            init.set_option("ytdl", false)?;
            init.set_option("terminal", false)?;
            init.set_option("idle", true)?;
            init.set_option("keep-open", true)?;
            init.set_option("pause", true)?;
            init.set_option("hwdec", "auto-safe")?;
            init.set_option("vo", "null")?;
            #[cfg(test)]
            init.set_option("ao", "null")?;
            Ok(())
        })
        .map_err(|error| PlayerError::EngineInitialization {
            detail: error.to_string(),
        })?;
        mpv.disable_deprecated_events()
            .and_then(|_| mpv.enable_event(mpv_event_id::FileLoaded))
            .and_then(|_| mpv.enable_event(mpv_event_id::EndFile))
            .and_then(|_| mpv.enable_event(mpv_event_id::QueueOverflow))
            .and_then(|_| mpv.enable_event(mpv_event_id::Shutdown))
            .and_then(|_| mpv.enable_event(mpv_event_id::PropertyChange))
            .and_then(|_| mpv.observe_property("eof-reached", Format::Flag, 1))
            .map_err(|error| PlayerError::EngineInitialization {
                detail: error.to_string(),
            })?;
        Ok(Self {
            mpv,
            path: None,
            pending_load: None,
            last_position_event: Instant::now(),
            eof_reached: false,
            screenshot_root_override,
            screenshot_app_data_fallback,
        })
    }

    fn state(&self) -> PlayerState {
        let volume_percent = finite_or(self.mpv.get_property("volume").unwrap_or(100.0), 100.0);
        let tracks = self.track_state();
        PlayerState {
            path: self.path.clone(),
            paused: self.mpv.get_property("pause").unwrap_or(true),
            buffering: self.mpv.get_property("paused-for-cache").unwrap_or(false),
            current_time: finite_or(self.mpv.get_property("time-pos").unwrap_or(0.0), 0.0),
            duration: finite_or(self.mpv.get_property("duration").unwrap_or(0.0), 0.0),
            volume: (volume_percent / 100.0).clamp(0.0, 1.0),
            muted: self.mpv.get_property("mute").unwrap_or(false),
            rate: finite_or(self.mpv.get_property("speed").unwrap_or(1.0), 1.0),
            width: self.mpv.get_property("width").ok(),
            height: self.mpv.get_property("height").ok(),
            audio_tracks: tracks.audio_tracks,
            subtitle_tracks: tracks.subtitle_tracks,
            selected_audio_track_id: tracks.selected_audio_track_id,
            selected_subtitle_track_id: tracks.selected_subtitle_track_id,
            chapters: self.chapters(),
            current_chapter_id: self
                .mpv
                .get_property("chapter")
                .ok()
                .filter(|chapter: &i64| *chapter >= 0),
        }
    }

    fn track_state(&self) -> TrackState {
        MpvNode::get(&self.mpv, "track-list")
            .map(|tracks| parse_track_list(tracks.as_ref()))
            .unwrap_or_default()
    }

    fn chapters(&self) -> Vec<PlayerChapter> {
        MpvNode::get(&self.mpv, "chapter-list")
            .map(|chapters| parse_chapter_list(chapters.as_ref()))
            .unwrap_or_default()
    }

    fn select_track(
        &mut self,
        property: &'static str,
        kind: &'static str,
        id: Option<i64>,
    ) -> Result<PlayerState, PlayerError> {
        self.require_loaded()?;
        if let Some(id) = id {
            let tracks = self.track_state();
            let available = match property {
                "aid" => &tracks.audio_tracks,
                "sid" => &tracks.subtitle_tracks,
                _ => unreachable!("Player track property is fixed"),
            };
            if !available.iter().any(|track| track.id == id) {
                return Err(PlayerError::TrackUnavailable { kind, id });
            }
            self.mpv
                .set_property(property, id)
                .map_err(|error| PlayerError::EngineCommand {
                    operation: "select track",
                    detail: error.to_string(),
                })?;
        } else {
            self.mpv
                .set_property(property, "no")
                .map_err(|error| PlayerError::EngineCommand {
                    operation: "disable track",
                    detail: error.to_string(),
                })?;
        }
        Ok(self.state())
    }

    fn require_loaded(&self) -> Result<(), PlayerError> {
        if self.path.is_none() {
            Err(PlayerError::NoMediaLoaded)
        } else {
            Ok(())
        }
    }

    fn finish_pending_load(&mut self) -> Result<PlayerState, PlayerError> {
        let pending = self.pending_load.take();
        if let Some(pending) = pending {
            for subtitle in pending.subtitles {
                let path = subtitle.path.to_string_lossy();
                let result = match (&subtitle.label, &subtitle.language) {
                    (Some(label), Some(language)) => self
                        .mpv
                        .command("sub-add", &[&path, "auto", label, language]),
                    (Some(label), None) => self.mpv.command("sub-add", &[&path, "auto", label]),
                    (None, Some(language)) => {
                        self.mpv.command("sub-add", &[&path, "auto", "", language])
                    }
                    (None, None) => self.mpv.command("sub-add", &[&path, "auto"]),
                };
                result.map_err(|error| PlayerError::EngineCommand {
                    operation: "add Subtitle track",
                    detail: error.to_string(),
                })?;
            }
            if let Some(start_time) = pending.start_time.filter(|position| *position > 0.0) {
                self.mpv
                    .command("seek", &[&start_time.to_string(), "absolute"])
                    .map_err(|error| PlayerError::EngineCommand {
                        operation: "seek to resume position",
                        detail: error.to_string(),
                    })?;
            }
            self.mpv
                .set_property("pause", !pending.autoplay)
                .map_err(|error| PlayerError::EngineCommand {
                    operation: "set initial playback state",
                    detail: error.to_string(),
                })?;
        }
        Ok(self.state())
    }

    fn event_error(&mut self, detail: String) -> PlayerError {
        if self.pending_load.take().is_some() {
            if let Some(path) = self.path.take() {
                PlayerError::MediaPlaybackFailed { path, detail }
            } else {
                PlayerError::EngineEvent { detail }
            }
        } else {
            PlayerError::EngineEvent { detail }
        }
    }

    fn screenshot_output_path(&self) -> Result<PathBuf, PlayerError> {
        if let Some(root) = &self.screenshot_root_override {
            screenshot_output_path_in_root(self.path.as_deref(), root)
        } else {
            screenshot_output_path(
                self.path.as_deref(),
                self.screenshot_app_data_fallback.as_deref(),
            )
        }
    }
}

impl PlayerBackend for LibmpvBackend {
    fn execute(&mut self, command: PlayerCommand) -> Result<PlayerCommandResult, PlayerError> {
        match command {
            PlayerCommand::State => Ok(self.state().into()),
            PlayerCommand::Load(request) => {
                let path = request.path.to_string_lossy();
                self.mpv
                    .command("loadfile", &[&path, "replace"])
                    .map_err(|error| PlayerError::EngineCommand {
                        operation: "load Lesson",
                        detail: error.to_string(),
                    })?;
                self.path = Some(request.path);
                self.pending_load = Some(PendingLoad {
                    subtitles: request.subtitles,
                    start_time: request.start_time,
                    autoplay: request.autoplay,
                });
                Ok(self.state().into())
            }
            PlayerCommand::Play => {
                self.require_loaded()?;
                self.mpv.set_property("pause", false).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "play",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::Pause => {
                self.require_loaded()?;
                self.mpv.set_property("pause", true).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "pause",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::Seek { seconds, mode } => {
                self.require_loaded()?;
                let seconds = seconds.to_string();
                let mode = match mode {
                    PlayerSeekMode::Absolute => "absolute",
                    PlayerSeekMode::Relative => "relative",
                };
                self.mpv
                    .command("seek", &[&seconds, mode])
                    .map_err(|error| PlayerError::EngineCommand {
                        operation: "seek",
                        detail: error.to_string(),
                    })?;
                Ok(self.state().into())
            }
            PlayerCommand::SetVolume(volume) => {
                self.require_loaded()?;
                self.mpv
                    .set_property("volume", volume.clamp(0.0, 1.0) * 100.0)
                    .map_err(|error| PlayerError::EngineCommand {
                        operation: "set volume",
                        detail: error.to_string(),
                    })?;
                Ok(self.state().into())
            }
            PlayerCommand::SetMuted(muted) => {
                self.require_loaded()?;
                self.mpv.set_property("mute", muted).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "set mute",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::SetRate(rate) => {
                self.require_loaded()?;
                self.mpv.set_property("speed", rate).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "set playback rate",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::SelectAudioTrack(id) => {
                self.select_track("aid", "audio", id).map(Into::into)
            }
            PlayerCommand::SelectSubtitleTrack(id) => {
                self.select_track("sid", "subtitle", id).map(Into::into)
            }
            PlayerCommand::SelectChapter(id) => {
                self.require_loaded()?;
                if !self.chapters().iter().any(|chapter| chapter.id == id) {
                    return Err(PlayerError::ChapterUnavailable { id });
                }
                self.mpv.set_property("chapter", id).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "select chapter",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::StepFrame => {
                self.require_loaded()?;
                self.mpv.command("frame-step", &[]).map_err(|error| {
                    PlayerError::EngineCommand {
                        operation: "step frame",
                        detail: error.to_string(),
                    }
                })?;
                Ok(self.state().into())
            }
            PlayerCommand::Screenshot => {
                self.require_loaded()?;
                let path = self.screenshot_output_path()?;
                let output = path.to_string_lossy();
                self.mpv
                    .command("screenshot-to-file", &[&output, "video"])
                    .map_err(|error| PlayerError::EngineCommand {
                        operation: "take screenshot",
                        detail: error.to_string(),
                    })?;
                Ok(PlayerCommandResult::ScreenshotSaved {
                    state: self.state(),
                    path,
                })
            }
            PlayerCommand::Destroy => {
                self.path = None;
                self.pending_load = None;
                self.eof_reached = false;
                Ok(self.state().into())
            }
        }
    }

    fn poll_event(&mut self) -> Option<PlayerEvent> {
        match self.mpv.wait_event(0.0)? {
            Ok(Event::FileLoaded) => Some(match self.finish_pending_load() {
                Ok(state) => {
                    self.eof_reached = false;
                    PlayerEvent::FileLoaded { state }
                }
                Err(error) => PlayerEvent::Error { error },
            }),
            Ok(Event::EndFile(reason)) if reason == mpv_end_file_reason::Eof => {
                self.eof_reached = true;
                Some(PlayerEvent::EndFile {
                    state: self.state(),
                })
            }
            Ok(Event::EndFile(reason)) if reason == mpv_end_file_reason::Quit => {
                Some(PlayerEvent::Shutdown)
            }
            Ok(Event::PropertyChange {
                name: "eof-reached",
                change: PropertyData::Flag(true),
                ..
            }) if !self.eof_reached => {
                self.eof_reached = true;
                Some(PlayerEvent::EndFile {
                    state: self.state(),
                })
            }
            Ok(Event::PropertyChange {
                name: "eof-reached",
                change: PropertyData::Flag(false),
                ..
            }) => {
                self.eof_reached = false;
                None
            }
            Ok(Event::QueueOverflow) => Some(PlayerEvent::Error {
                error: PlayerError::EngineEventQueueOverflow,
            }),
            Ok(Event::Shutdown) => Some(PlayerEvent::Shutdown),
            Err(error) => {
                let error = self.event_error(error.to_string());
                Some(PlayerEvent::Error { error })
            }
            _ => None,
        }
    }

    fn poll_position_event(&mut self) -> Option<PlayerEvent> {
        if self.path.is_none() || self.last_position_event.elapsed() < Duration::from_millis(500) {
            return None;
        }
        self.last_position_event = Instant::now();
        Some(PlayerEvent::Position {
            state: self.state(),
        })
    }

    fn poll_lifecycle_event(&mut self) -> Option<PlayerEvent> {
        if self.path.is_some()
            && !self.eof_reached
            && self
                .mpv
                .get_property::<bool>("eof-reached")
                .unwrap_or(false)
        {
            self.eof_reached = true;
            return Some(PlayerEvent::EndFile {
                state: self.state(),
            });
        }
        None
    }
}

fn configure_libmpv_numeric_locale() {
    let locale = c"C";
    // SAFETY: `locale` is a valid static C string and `setlocale` borrows it
    // only for this call. libmpv requires the process numeric locale to be C.
    unsafe {
        libc::setlocale(libc::LC_NUMERIC, locale.as_ptr());
    }
}

fn finite_or(value: f64, fallback: f64) -> f64 {
    if value.is_finite() { value } else { fallback }
}

fn node_field<'a>(node: MpvNodeRef<'a>, field: &str) -> Option<MpvNodeRef<'a>> {
    node.map_entries()
        .into_iter()
        .find_map(|(key, value)| (key == field).then_some(value))
}

fn node_string_field(node: MpvNodeRef<'_>, field: &str) -> Option<String> {
    let value = node_field(node, field)?;
    value
        .to_string()
        .or_else(|| value.to_i64().map(|number| number.to_string()))
}

fn node_f64_field(node: MpvNodeRef<'_>, field: &str) -> Option<f64> {
    let value = node_field(node, field)?;
    value
        .to_f64()
        .or_else(|| value.to_i64().map(|number| number as f64))
}

fn blank_to_none(value: Option<String>) -> Option<String> {
    value.and_then(|value| {
        let trimmed = value.trim();
        (!trimmed.is_empty()).then(|| trimmed.to_owned())
    })
}

fn parse_track_list(node: MpvNodeRef<'_>) -> TrackState {
    let mut state = TrackState::default();
    for track in node.array_items() {
        let Some(kind) = node_string_field(track, "type") else {
            continue;
        };
        if kind != "audio" && kind != "sub" {
            continue;
        }
        let Some(id) = node_field(track, "id").and_then(MpvNodeRef::to_i64) else {
            continue;
        };
        let selected = node_field(track, "selected")
            .and_then(MpvNodeRef::to_bool)
            .unwrap_or(false);
        let player_track = PlayerTrack {
            id,
            title: blank_to_none(node_string_field(track, "title")),
            language: blank_to_none(node_string_field(track, "lang")),
        };
        if kind == "audio" {
            if selected {
                state.selected_audio_track_id = Some(id);
            }
            state.audio_tracks.push(player_track);
        } else {
            if selected {
                state.selected_subtitle_track_id = Some(id);
            }
            state.subtitle_tracks.push(player_track);
        }
    }
    state
}

fn parse_chapter_list(node: MpvNodeRef<'_>) -> Vec<PlayerChapter> {
    node.array_items()
        .into_iter()
        .enumerate()
        .map(|(index, chapter)| PlayerChapter {
            id: i64::try_from(index).expect("libmpv chapter count fits i64"),
            title: blank_to_none(node_string_field(chapter, "title")),
            start_time: node_f64_field(chapter, "time").unwrap_or(0.0),
        })
        .collect()
}

pub(crate) type PlayerEventSink = Arc<dyn Fn(PlayerEvent) -> bool + Send + Sync>;

struct QueuedCommand {
    request_id: NonZeroU64,
    command: PlayerCommand,
}

pub(crate) struct PlayerWorker {
    sender: Option<SyncSender<QueuedCommand>>,
    worker: Option<JoinHandle<()>>,
}

impl PlayerWorker {
    pub(crate) fn start(
        capacity: NonZeroUsize,
        event_sink: PlayerEventSink,
    ) -> Result<Self, PlayerError> {
        Self::start_with_backend(capacity, LibmpvBackend::new()?, event_sink)
    }

    pub(crate) fn start_with_app_data(
        capacity: NonZeroUsize,
        app_data_fallback: PathBuf,
        event_sink: PlayerEventSink,
    ) -> Result<Self, PlayerError> {
        Self::start_with_backend(
            capacity,
            LibmpvBackend::new_with_app_data(app_data_fallback)?,
            event_sink,
        )
    }

    pub(crate) fn start_with_backend<B>(
        capacity: NonZeroUsize,
        mut backend: B,
        event_sink: PlayerEventSink,
    ) -> Result<Self, PlayerError>
    where
        B: PlayerBackend,
    {
        let (sender, receiver) = mpsc::sync_channel::<QueuedCommand>(capacity.get());
        let worker = thread::Builder::new()
            .name("melearner-player".to_string())
            .spawn(move || {
                'worker: loop {
                    match receiver.recv_timeout(Duration::from_millis(20)) {
                        Ok(queued) => {
                            let destroy = matches!(&queued.command, PlayerCommand::Destroy);
                            let result = queued
                                .command
                                .validate()
                                .and_then(|command| backend.execute(command));
                            let event = PlayerEvent::CommandCompleted {
                                request_id: queued.request_id,
                                result,
                            };
                            if !event_sink(event) {
                                break;
                            }
                            if destroy {
                                let _ = event_sink(PlayerEvent::Shutdown);
                                break;
                            }
                        }
                        Err(RecvTimeoutError::Timeout) => {}
                        Err(RecvTimeoutError::Disconnected) => break,
                    }
                    while let Some(event) = backend.poll_event() {
                        let shutdown = matches!(event, PlayerEvent::Shutdown);
                        if !event_sink(event) || shutdown {
                            break 'worker;
                        }
                    }
                    if let Some(event) = backend.poll_lifecycle_event()
                        && !event_sink(event)
                    {
                        break;
                    }
                    if let Some(event) = backend.poll_position_event()
                        && !event_sink(event)
                    {
                        break;
                    }
                }
            })
            .map_err(|error| PlayerError::WorkerStart {
                detail: error.to_string(),
            })?;
        Ok(Self {
            sender: Some(sender),
            worker: Some(worker),
        })
    }

    pub(crate) fn try_submit(
        &self,
        request_id: NonZeroU64,
        command: PlayerCommand,
    ) -> Result<(), PlayerSubmitError> {
        let sender = self.sender.as_ref().ok_or(PlayerSubmitError::Closed)?;
        match sender.try_send(QueuedCommand {
            request_id,
            command,
        }) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) => Err(PlayerSubmitError::Full),
            Err(TrySendError::Disconnected(_)) => Err(PlayerSubmitError::Closed),
        }
    }
}

impl Drop for PlayerWorker {
    fn drop(&mut self) {
        self.sender.take();
        if let Some(worker) = self.worker.take()
            && worker.thread().id() != thread::current().id()
        {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        ApprovedRoots, LibmpvBackend, PlayerBackend, PlayerCommand, PlayerCommandResult,
        PlayerError, PlayerEvent, PlayerLoadRequest, PlayerSeekMode, PlayerState,
        PlayerSubmitError, PlayerSubtitleLoadRequest, PlayerWorker,
    };
    use std::fs;
    use std::mem::size_of;
    use std::num::{NonZeroU64, NonZeroUsize};
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Mutex, MutexGuard, mpsc};
    use std::time::{Duration, Instant};

    struct StateBackend;

    static LIBMPV_TEST_LOCK: Mutex<()> = Mutex::new(());

    struct BlockingBackend {
        started: mpsc::Sender<()>,
        release: mpsc::Receiver<()>,
    }

    fn lock_libmpv() -> MutexGuard<'static, ()> {
        LIBMPV_TEST_LOCK
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn write_wav_lesson(path: &Path) {
        let sample_rate = 8_000_u32;
        let samples = [0_i16; 800];
        let data_len = u32::try_from(samples.len() * size_of::<i16>()).expect("WAV data length");
        let mut bytes = Vec::with_capacity(44 + data_len as usize);
        bytes.extend_from_slice(b"RIFF");
        bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt ");
        bytes.extend_from_slice(&16_u32.to_le_bytes());
        bytes.extend_from_slice(&1_u16.to_le_bytes());
        bytes.extend_from_slice(&1_u16.to_le_bytes());
        bytes.extend_from_slice(&sample_rate.to_le_bytes());
        bytes.extend_from_slice(&(sample_rate * 2).to_le_bytes());
        bytes.extend_from_slice(&2_u16.to_le_bytes());
        bytes.extend_from_slice(&16_u16.to_le_bytes());
        bytes.extend_from_slice(b"data");
        bytes.extend_from_slice(&data_len.to_le_bytes());
        for sample in samples {
            bytes.extend_from_slice(&sample.to_le_bytes());
        }
        fs::write(path, bytes).expect("write WAV Lesson");
    }

    fn parity_media(relative: &str) -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/parity/media")
            .join(relative)
            .canonicalize()
            .expect("canonical parity media fixture")
    }

    #[test]
    fn screenshot_path_policy_sanitizes_and_uses_each_root_source() {
        let media = PathBuf::from("13 - Shifting Operations!.mkv");
        assert_eq!(
            super::screenshot_file_stem(Some(&media)),
            "13_-_Shifting_Operations_"
        );

        let override_root = super::screenshot_root_from(
            |name| (name == "MELEARNER_SCREENSHOT_DIR").then(|| " /captures ".into()),
            Some(Path::new("/app-data")),
        )
        .expect("override screenshot root");
        assert_eq!(override_root, PathBuf::from(" /captures "));

        let home_root = super::screenshot_root_from(
            |name| (name == "HOME").then(|| "/home/learner".into()),
            Some(Path::new("/app-data")),
        )
        .expect("home screenshot root");
        assert_eq!(home_root, PathBuf::from("/home/learner/Pictures/melearner"));

        let fallback_root = super::screenshot_root_from(|_| None, Some(Path::new("/app-data")))
            .expect("app-data screenshot root");
        assert_eq!(fallback_root, PathBuf::from("/app-data/screenshots"));

        assert_eq!(
            super::screenshot_filename(Some(&media), 42, 7),
            "13_-_Shifting_Operations_-42-7.png"
        );
    }

    #[test]
    fn player_worker_saves_a_nonempty_png_from_the_frozen_media_fixture() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("Systems 日本語/01 H264 AAC.mp4");
        let screenshot_root = tempfile::tempdir().expect("create screenshot root");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let backend = LibmpvBackend::new_with_screenshot_root(screenshot_root.path().to_path_buf())
            .expect("initialize embedded libmpv");
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(2).expect("nonzero capacity"),
            backend,
            sink,
        )
        .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(90).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");
        loop {
            if matches!(
                event_receiver
                    .recv_timeout(Duration::from_secs(2))
                    .expect("receive load event"),
                PlayerEvent::FileLoaded { .. }
            ) {
                break;
            }
        }

        let request_id = NonZeroU64::new(91).expect("nonzero request");
        worker
            .try_submit(request_id, PlayerCommand::Screenshot)
            .expect("submit screenshot");
        let path = loop {
            if let PlayerEvent::CommandCompleted {
                request_id: completed_id,
                result,
            } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive screenshot result")
                && completed_id == request_id
            {
                match result.expect("take screenshot") {
                    PlayerCommandResult::ScreenshotSaved { path, .. } => break path,
                    PlayerCommandResult::State(_) => panic!("screenshot path is missing"),
                }
            }
        };

        assert_eq!(path.parent(), Some(screenshot_root.path()));
        let bytes = fs::read(&path).expect("read saved screenshot");
        assert!(bytes.len() > 8);
        assert_eq!(&bytes[..8], b"\x89PNG\r\n\x1a\n");
    }

    impl PlayerBackend for StateBackend {
        fn execute(&mut self, command: PlayerCommand) -> Result<PlayerCommandResult, PlayerError> {
            let _ = command;
            Ok(PlayerState {
                path: None,
                paused: true,
                buffering: false,
                current_time: 0.0,
                duration: 0.0,
                volume: 1.0,
                muted: false,
                rate: 1.0,
                width: None,
                height: None,
                audio_tracks: Vec::new(),
                subtitle_tracks: Vec::new(),
                selected_audio_track_id: None,
                selected_subtitle_track_id: None,
                chapters: Vec::new(),
                current_chapter_id: None,
            }
            .into())
        }
    }

    impl PlayerBackend for BlockingBackend {
        fn execute(&mut self, command: PlayerCommand) -> Result<PlayerCommandResult, PlayerError> {
            self.started.send(()).expect("report command start");
            self.release.recv().expect("release blocked command");
            StateBackend.execute(command)
        }
    }

    #[test]
    fn approved_roots_resolve_a_local_lesson() {
        let root = tempfile::tempdir().expect("create approved root");
        let lesson = root.path().join("lesson.mp4");
        fs::write(&lesson, b"media").expect("write Lesson");

        let approved = ApprovedRoots::new([root.path()]).expect("approve root");

        assert_eq!(
            approved.resolve_media(&lesson).expect("resolve Lesson"),
            lesson.canonicalize().expect("canonical Lesson")
        );
    }

    #[test]
    fn approved_roots_reject_a_missing_lesson_with_a_typed_error() {
        let root = tempfile::tempdir().expect("create approved root");
        let missing = root.path().join("missing.mp4");
        let approved = ApprovedRoots::new([root.path()]).expect("approve root");

        assert_eq!(
            approved.resolve_media(&missing),
            Err(PlayerError::MediaMissing { path: missing })
        );
    }

    #[test]
    fn approved_roots_reject_a_lesson_outside_the_library() {
        let root = tempfile::tempdir().expect("create approved root");
        let outside = tempfile::tempdir().expect("create outside directory");
        let lesson = outside.path().join("lesson.mp4");
        fs::write(&lesson, b"media").expect("write Lesson");
        let canonical_lesson = lesson.canonicalize().expect("canonical Lesson");
        let approved = ApprovedRoots::new([root.path()]).expect("approve root");

        assert_eq!(
            approved.resolve_media(&lesson),
            Err(PlayerError::MediaOutsideApprovedRoots {
                path: canonical_lesson
            })
        );
    }

    #[test]
    fn approved_roots_reject_urls_before_touching_the_filesystem() {
        let root = tempfile::tempdir().expect("create approved root");
        let approved = ApprovedRoots::new([root.path()]).expect("approve root");
        let url = PathBuf::from("https://example.com/lesson.mp4");

        assert_eq!(
            approved.resolve_media(&url),
            Err(PlayerError::InvalidLocalPath { path: url })
        );
    }

    #[test]
    fn player_worker_reports_state_through_its_event_sink() {
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(2).expect("nonzero capacity"),
            StateBackend,
            sink,
        )
        .expect("start Player worker");

        worker
            .try_submit(
                NonZeroU64::new(41).expect("nonzero request"),
                PlayerCommand::State,
            )
            .expect("submit state request");

        assert_eq!(
            event_receiver.recv().expect("receive state"),
            PlayerEvent::CommandCompleted {
                request_id: NonZeroU64::new(41).expect("nonzero request"),
                result: Ok(PlayerCommandResult::State(PlayerState {
                    path: None,
                    paused: true,
                    buffering: false,
                    current_time: 0.0,
                    duration: 0.0,
                    volume: 1.0,
                    muted: false,
                    rate: 1.0,
                    width: None,
                    height: None,
                    audio_tracks: Vec::new(),
                    subtitle_tracks: Vec::new(),
                    selected_audio_track_id: None,
                    selected_subtitle_track_id: None,
                    chapters: Vec::new(),
                    current_chapter_id: None,
                })),
            }
        );
    }

    #[test]
    fn player_worker_rejects_a_missing_lesson_before_loading_it() {
        let root = tempfile::tempdir().expect("create approved root");
        let missing = root.path().join("missing.mp4");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(2).expect("nonzero capacity"),
            StateBackend,
            sink,
        )
        .expect("start Player worker");
        let request_id = NonZeroU64::new(42).expect("nonzero request");

        worker
            .try_submit(
                request_id,
                PlayerCommand::Load(PlayerLoadRequest {
                    path: missing.clone(),
                    approved_roots: vec![root.path().to_path_buf()],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");

        assert_eq!(
            event_receiver.recv().expect("receive load result"),
            PlayerEvent::CommandCompleted {
                request_id,
                result: Err(PlayerError::MediaMissing { path: missing }),
            }
        );
    }

    #[test]
    fn player_worker_rejects_invalid_numeric_controls_before_libmpv() {
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(4).expect("nonzero capacity"),
            StateBackend,
            sink,
        )
        .expect("start Player worker");
        let cases = [
            (
                PlayerCommand::Seek {
                    seconds: f64::NAN,
                    mode: PlayerSeekMode::Absolute,
                },
                PlayerError::InvalidSeekTarget,
            ),
            (
                PlayerCommand::SetVolume(f64::INFINITY),
                PlayerError::InvalidVolume,
            ),
            (
                PlayerCommand::SetRate(4.01),
                PlayerError::InvalidPlaybackRate,
            ),
        ];

        for (index, (command, expected_error)) in cases.into_iter().enumerate() {
            let request_id =
                NonZeroU64::new(u64::try_from(index + 1).expect("request id")).expect("nonzero");
            worker
                .try_submit(request_id, command)
                .expect("submit invalid control");
            assert_eq!(
                event_receiver.recv().expect("receive control result"),
                PlayerEvent::CommandCompleted {
                    request_id,
                    result: Err(expected_error),
                }
            );
        }
    }

    #[test]
    fn embedded_libmpv_starts_idle_without_a_loaded_lesson() {
        let _libmpv = lock_libmpv();
        let mut backend = LibmpvBackend::new().expect("initialize embedded libmpv");

        let state = backend
            .execute(PlayerCommand::State)
            .expect("read Player state")
            .into_state();

        assert_eq!(state.current_time, 0.0);
        assert_eq!(state.duration, 0.0);
        assert_eq!(state.volume, 1.0);
        assert!(!state.muted);
        assert_eq!(state.rate, 1.0);
        assert_eq!(state.width, None);
        assert_eq!(state.height, None);
    }

    #[test]
    fn player_worker_owns_the_embedded_libmpv_backend() {
        let _libmpv = lock_libmpv();
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");
        let request_id = NonZeroU64::new(43).expect("nonzero request");

        worker
            .try_submit(request_id, PlayerCommand::State)
            .expect("submit state request");

        assert_eq!(
            event_receiver.recv().expect("receive state"),
            PlayerEvent::CommandCompleted {
                request_id,
                result: Ok(PlayerCommandResult::State(PlayerState {
                    path: None,
                    paused: true,
                    buffering: false,
                    current_time: 0.0,
                    duration: 0.0,
                    volume: 1.0,
                    muted: false,
                    rate: 1.0,
                    width: None,
                    height: None,
                    audio_tracks: Vec::new(),
                    subtitle_tracks: Vec::new(),
                    selected_audio_track_id: None,
                    selected_subtitle_track_id: None,
                    chapters: Vec::new(),
                    current_chapter_id: None,
                })),
            }
        );
    }

    #[test]
    fn player_worker_emits_file_loaded_from_libmpv() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("Systems 日本語/01 H264 AAC.mp4");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(44).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson.clone(),
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");

        let loaded = loop {
            let event = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Player event");
            if let PlayerEvent::FileLoaded { state } = event {
                break state;
            }
        };
        assert_eq!(loaded.path, Some(lesson));
        assert!(loaded.paused);
        assert!(loaded.duration > 0.0);
        assert!(loaded.width.is_some_and(|width| width > 0));
        assert!(loaded.height.is_some_and(|height| height > 0));
    }

    #[test]
    fn embedded_libmpv_loads_the_frozen_hevc_main_10_fixture() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("03 HEVC Main 10.mkv");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(45).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit HEVC load request");

        let loaded = loop {
            if let PlayerEvent::FileLoaded { state } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive HEVC Player event")
            {
                break state;
            }
        };
        assert!(loaded.width.is_some_and(|width| width > 0));
        assert!(loaded.height.is_some_and(|height| height > 0));
    }

    #[test]
    fn player_worker_applies_coarse_controls_through_libmpv() {
        let _libmpv = lock_libmpv();
        let root = tempfile::tempdir().expect("create approved root");
        let lesson = root.path().join("lesson.wav");
        write_wav_lesson(&lesson);
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(4).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(50).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![root.path().to_path_buf()],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");
        loop {
            if matches!(
                event_receiver
                    .recv_timeout(Duration::from_secs(2))
                    .expect("receive load event"),
                PlayerEvent::FileLoaded { .. }
            ) {
                break;
            }
        }

        let commands = [
            PlayerCommand::Play,
            PlayerCommand::Pause,
            PlayerCommand::Seek {
                seconds: 0.0,
                mode: PlayerSeekMode::Absolute,
            },
            PlayerCommand::Seek {
                seconds: 0.0,
                mode: PlayerSeekMode::Relative,
            },
            PlayerCommand::SetVolume(0.4),
            PlayerCommand::SetMuted(true),
            PlayerCommand::SetRate(1.25),
            PlayerCommand::StepFrame,
        ];
        let mut final_state = None;
        for (index, command) in commands.into_iter().enumerate() {
            let request_id = NonZeroU64::new(51 + u64::try_from(index).expect("request id"))
                .expect("nonzero request");
            worker
                .try_submit(request_id, command)
                .expect("submit Player control");
            loop {
                if let PlayerEvent::CommandCompleted {
                    request_id: completed_id,
                    result,
                } = event_receiver
                    .recv_timeout(Duration::from_secs(2))
                    .expect("receive control event")
                    && completed_id == request_id
                {
                    final_state = Some(result.expect("apply Player control").into_state());
                    break;
                }
            }
        }

        let state = final_state.expect("final Player state");
        assert!(state.paused);
        assert_eq!(state.volume, 0.4);
        assert!(state.muted);
        assert_eq!(state.rate, 1.25);
    }

    #[test]
    fn player_worker_emits_coarse_position_state_after_load() {
        let _libmpv = lock_libmpv();
        let root = tempfile::tempdir().expect("create approved root");
        let lesson = root.path().join("lesson.wav");
        write_wav_lesson(&lesson);
        let canonical_lesson = lesson.canonicalize().expect("canonical Lesson");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(60).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![root.path().to_path_buf()],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");

        let position = loop {
            let event = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Player event");
            if let PlayerEvent::Position { state } = event {
                break state;
            }
        };
        assert_eq!(position.path, Some(canonical_lesson));
        assert!(position.paused);
    }

    #[test]
    fn player_worker_emits_end_file_from_libmpv() {
        let _libmpv = lock_libmpv();
        let root = tempfile::tempdir().expect("create approved root");
        let lesson = root.path().join("lesson.wav");
        write_wav_lesson(&lesson);
        let canonical_lesson = lesson.canonicalize().expect("canonical Lesson");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(61).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![root.path().to_path_buf()],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: true,
                }),
            )
            .expect("submit load request");

        let deadline = Instant::now() + Duration::from_secs(2);
        let mut last_position = None;
        let mut last_error = None;
        let ended = loop {
            let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
                panic!("receive end-file before deadline; last position: {last_position:?}");
            };
            match event_receiver.recv_timeout(remaining) {
                Ok(PlayerEvent::EndFile { state }) => break state,
                Ok(PlayerEvent::Position { state }) => last_position = Some(state),
                Ok(PlayerEvent::Error { error }) => last_error = Some(error),
                Ok(_) => {}
                Err(error) => {
                    panic!(
                        "receive end-file before deadline: {error}; last position: {last_position:?}; last error: {last_error:?}"
                    )
                }
            }
        };
        assert_eq!(ended.path, Some(canonical_lesson));
    }

    #[test]
    fn player_state_reads_tracks_and_chapters_from_the_frozen_fixture() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("02 Multi audio chapters.mkv");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(70).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");

        let state = loop {
            let event = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Player event");
            if let PlayerEvent::FileLoaded { state } = event {
                break state;
            }
        };
        assert_eq!(
            state
                .audio_tracks
                .iter()
                .map(|track| (track.title.as_deref(), track.language.as_deref()))
                .collect::<Vec<_>>(),
            vec![
                (Some("English"), Some("eng")),
                (Some("Japanese"), Some("jpn"))
            ]
        );
        assert_eq!(
            state
                .chapters
                .iter()
                .map(|chapter| (chapter.title.as_deref(), chapter.start_time))
                .collect::<Vec<_>>(),
            vec![(Some("Foundations"), 0.0), (Some("Review"), 1.0)]
        );
        assert!(state.selected_audio_track_id.is_some());
        assert_eq!(state.current_chapter_id, Some(0));
    }

    #[test]
    fn player_worker_selects_tracks_and_chapters_from_the_frozen_fixture() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("02 Multi audio chapters.mkv");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(3).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(71).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");
        let loaded = loop {
            if let PlayerEvent::FileLoaded { state } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Player event")
            {
                break state;
            }
        };
        let japanese = loaded
            .audio_tracks
            .iter()
            .find(|track| track.language.as_deref() == Some("jpn"))
            .expect("Japanese audio track")
            .id;

        let cases = [
            (
                NonZeroU64::new(72).expect("nonzero request"),
                PlayerCommand::SelectAudioTrack(Some(japanese)),
            ),
            (
                NonZeroU64::new(73).expect("nonzero request"),
                PlayerCommand::SelectChapter(1),
            ),
        ];
        let mut selected = loaded;
        for (request_id, command) in cases {
            worker
                .try_submit(request_id, command)
                .expect("submit selection");
            loop {
                if let PlayerEvent::CommandCompleted {
                    request_id: completed_id,
                    result,
                } = event_receiver
                    .recv_timeout(Duration::from_secs(2))
                    .expect("receive selection result")
                    && completed_id == request_id
                {
                    selected = result.expect("apply selection").into_state();
                    break;
                }
            }
        }
        assert_eq!(selected.selected_audio_track_id, Some(japanese));
        assert_eq!(selected.current_chapter_id, Some(1));
    }

    #[test]
    fn player_worker_loads_and_selects_frozen_external_subtitles() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("Systems 日本語/01 H264 AAC.mp4");
        let subtitles = vec![
            PlayerSubtitleLoadRequest {
                path: parity_media("01 H264 AAC.en.srt"),
                label: Some("English".to_string()),
                language: Some("eng".to_string()),
            },
            PlayerSubtitleLoadRequest {
                path: parity_media("01 H264 AAC.ja.vtt"),
                label: Some("Japanese".to_string()),
                language: Some("jpn".to_string()),
            },
        ];
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(74).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson,
                    approved_roots: vec![media_root],
                    subtitles,
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit load request");
        let loaded = loop {
            if let PlayerEvent::FileLoaded { state } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Player event")
            {
                break state;
            }
        };
        let japanese = loaded
            .subtitle_tracks
            .iter()
            .find(|track| {
                track.title.as_deref() == Some("Japanese")
                    && track.language.as_deref() == Some("jpn")
            })
            .expect("Japanese external subtitle")
            .id;

        worker
            .try_submit(
                NonZeroU64::new(75).expect("nonzero request"),
                PlayerCommand::SelectSubtitleTrack(Some(japanese)),
            )
            .expect("submit Subtitle selection");
        let selected = loop {
            if let PlayerEvent::CommandCompleted { request_id, result } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive Subtitle selection")
                && request_id == NonZeroU64::new(75).expect("nonzero request")
            {
                break result.expect("select Subtitle").into_state();
            }
        };
        assert_eq!(selected.selected_subtitle_track_id, Some(japanese));
    }

    #[test]
    fn player_worker_reports_a_typed_failure_for_the_frozen_corrupt_fixture() {
        let _libmpv = lock_libmpv();
        let media_root = parity_media("");
        let lesson = parity_media("corrupt-media.bin");
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start(NonZeroUsize::new(2).expect("nonzero capacity"), sink)
            .expect("start embedded Player worker");

        worker
            .try_submit(
                NonZeroU64::new(76).expect("nonzero request"),
                PlayerCommand::Load(PlayerLoadRequest {
                    path: lesson.clone(),
                    approved_roots: vec![media_root],
                    subtitles: Vec::new(),
                    start_time: None,
                    autoplay: false,
                }),
            )
            .expect("submit corrupt load request");

        let error = loop {
            if let PlayerEvent::Error { error } = event_receiver
                .recv_timeout(Duration::from_secs(2))
                .expect("receive corrupt-media failure")
            {
                break error;
            }
        };
        assert!(matches!(
            error,
            PlayerError::MediaPlaybackFailed { path, .. } if path == lesson
        ));
    }

    #[test]
    fn player_worker_completes_destroy_before_shutdown() {
        let (event_sender, event_receiver) = mpsc::channel();
        let sink = Arc::new(move |event| event_sender.send(event).is_ok());
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(1).expect("nonzero capacity"),
            StateBackend,
            sink,
        )
        .expect("start Player worker");
        let request_id = NonZeroU64::new(77).expect("nonzero request");

        worker
            .try_submit(request_id, PlayerCommand::Destroy)
            .expect("submit destroy");

        assert!(matches!(
            event_receiver.recv().expect("receive destroy result"),
            PlayerEvent::CommandCompleted {
                request_id: completed_id,
                result: Ok(_),
            } if completed_id == request_id
        ));
        assert_eq!(
            event_receiver.recv().expect("receive shutdown"),
            PlayerEvent::Shutdown
        );
        assert!(
            event_receiver
                .recv_timeout(Duration::from_millis(20))
                .is_err()
        );
    }

    #[test]
    fn player_worker_reports_queue_pressure_without_blocking_the_caller() {
        let (started_sender, started_receiver) = mpsc::channel();
        let (release_sender, release_receiver) = mpsc::channel();
        let sink = Arc::new(|_| true);
        let worker = PlayerWorker::start_with_backend(
            NonZeroUsize::new(1).expect("nonzero capacity"),
            BlockingBackend {
                started: started_sender,
                release: release_receiver,
            },
            sink,
        )
        .expect("start Player worker");

        worker
            .try_submit(
                NonZeroU64::new(80).expect("nonzero request"),
                PlayerCommand::State,
            )
            .expect("submit executing command");
        started_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("worker starts first command");
        worker
            .try_submit(
                NonZeroU64::new(81).expect("nonzero request"),
                PlayerCommand::State,
            )
            .expect("fill bounded queue");

        assert_eq!(
            worker.try_submit(
                NonZeroU64::new(82).expect("nonzero request"),
                PlayerCommand::State,
            ),
            Err(PlayerSubmitError::Full)
        );

        release_sender.send(()).expect("release first command");
        release_sender.send(()).expect("release queued command");
    }
}
