mod surface;

use libmpv2::{
    Mpv,
    events::{Event, mpv_event_id},
    mpv_end_file_reason,
};
use libmpv2_sys as mpv_sys;
use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use surface::NativeVideoSurface;
use tauri::{AppHandle, Emitter, Manager};

static PLAYER: OnceLock<Mutex<Option<NativePlayer>>> = OnceLock::new();
static PLAYBACK_EVENT_RUN: OnceLock<Mutex<u64>> = OnceLock::new();
static POSITION_EVENT_RUN: OnceLock<Mutex<u64>> = OnceLock::new();
const EVENT_STATE: &str = "native-player://state";
const EVENT_POSITION: &str = "native-player://position";
const EVENT_TRACKS: &str = "native-player://tracks";
const EVENT_CHAPTERS: &str = "native-player://chapters";
const EVENT_FILE_LOADED: &str = "native-player://file-loaded";
const EVENT_END_FILE: &str = "native-player://end-file";
const EVENT_ERROR: &str = "native-player://error";

fn player_slot() -> &'static Mutex<Option<NativePlayer>> {
    PLAYER.get_or_init(|| Mutex::new(None))
}

fn position_event_slot() -> &'static Mutex<u64> {
    POSITION_EVENT_RUN.get_or_init(|| Mutex::new(0))
}

fn playback_event_slot() -> &'static Mutex<u64> {
    PLAYBACK_EVENT_RUN.get_or_init(|| Mutex::new(0))
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativeTrack {
    id: String,
    title: Option<String>,
    language: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativeChapter {
    id: String,
    title: Option<String>,
    start_time: f64,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerTracksEvent {
    audio_tracks: Vec<NativeTrack>,
    subtitle_tracks: Vec<NativeTrack>,
    selected_audio_track_id: Option<String>,
    selected_subtitle_track_id: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerChaptersEvent {
    chapters: Vec<NativeChapter>,
    current_chapter_id: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerFileLoadedEvent {
    path: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerErrorEvent {
    message: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerState {
    path: Option<String>,
    paused: bool,
    buffering: bool,
    current_time: f64,
    duration: f64,
    volume: f64,
    muted: bool,
    rate: f64,
    width: Option<i64>,
    height: Option<i64>,
    surface_attached: bool,
    surface_backend: Option<String>,
    surface_render_api: bool,
    audio_tracks: Vec<NativeTrack>,
    subtitle_tracks: Vec<NativeTrack>,
    selected_audio_track_id: Option<String>,
    selected_subtitle_track_id: Option<String>,
    chapters: Vec<NativeChapter>,
    current_chapter_id: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerPositionEvent {
    path: Option<String>,
    paused: bool,
    buffering: bool,
    current_time: f64,
    duration: f64,
    volume: f64,
    muted: bool,
    rate: f64,
    width: Option<i64>,
    height: Option<i64>,
    current_chapter_id: Option<String>,
}

#[derive(Clone, Copy, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerBounds {
    x: i64,
    y: i64,
    width: i64,
    height: i64,
    scale_factor: f64,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerLoadOptions {
    path: String,
    allowed_roots: Vec<String>,
    #[serde(default)]
    subtitles: Vec<NativeSubtitleLoadOptions>,
    start_time: Option<f64>,
    autoplay: Option<bool>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NativeSubtitleLoadOptions {
    path: String,
    label: Option<String>,
    language: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NativePlayerSeekOptions {
    seconds: f64,
    mode: NativePlayerSeekMode,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum NativePlayerSeekMode {
    Absolute,
    Relative,
}

#[derive(Default)]
struct NativeTrackState {
    audio_tracks: Vec<NativeTrack>,
    subtitle_tracks: Vec<NativeTrack>,
    selected_audio_track_id: Option<String>,
    selected_subtitle_track_id: Option<String>,
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
        unsafe {
            mpv_sys::mpv_free_node_contents(&mut self.raw);
        }
    }
}

impl MpvNode {
    fn get(mpv: &Mpv, property: &str) -> Result<Self, String> {
        let property =
            CString::new(property).map_err(|err| format!("invalid mpv property: {err}"))?;
        let mut raw = MaybeUninit::<mpv_sys::mpv_node>::zeroed();
        let result = unsafe {
            mpv_sys::mpv_get_property(
                mpv.ctx.as_ptr(),
                property.as_ptr(),
                mpv_sys::mpv_format_MPV_FORMAT_NODE,
                raw.as_mut_ptr().cast(),
            )
        };
        if result < 0 {
            return Err(format!("libmpv could not read node property: {result}"));
        }

        Ok(Self {
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
    fn format(&self) -> mpv_sys::mpv_format {
        unsafe { (*self.raw).format }
    }

    fn to_bool(&self) -> Option<bool> {
        if self.format() == mpv_sys::mpv_format_MPV_FORMAT_FLAG {
            Some(unsafe { (*self.raw).u.flag } != 0)
        } else {
            None
        }
    }

    fn to_i64(&self) -> Option<i64> {
        if self.format() == mpv_sys::mpv_format_MPV_FORMAT_INT64 {
            Some(unsafe { (*self.raw).u.int64 })
        } else {
            None
        }
    }

    fn to_f64(&self) -> Option<f64> {
        if self.format() == mpv_sys::mpv_format_MPV_FORMAT_DOUBLE {
            Some(unsafe { (*self.raw).u.double_ })
        } else {
            None
        }
    }

    fn to_str(&self) -> Option<String> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_STRING {
            return None;
        }
        let raw = unsafe { (*self.raw).u.string };
        if raw.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(raw) }
            .to_str()
            .ok()
            .map(str::to_string)
    }

    fn array_items(&self) -> Vec<MpvNodeRef<'a>> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_NODE_ARRAY {
            return Vec::new();
        }
        let list = unsafe { (*self.raw).u.list };
        if list.is_null() {
            return Vec::new();
        }
        let list = unsafe { &*list };
        if list.num <= 0 || list.values.is_null() {
            return Vec::new();
        }

        (0..list.num)
            .map(|index| MpvNodeRef {
                raw: unsafe { list.values.offset(index as isize) },
                _owner: PhantomData,
            })
            .collect()
    }

    fn map_entries(&self) -> Vec<(String, MpvNodeRef<'a>)> {
        if self.format() != mpv_sys::mpv_format_MPV_FORMAT_NODE_MAP {
            return Vec::new();
        }
        let list = unsafe { (*self.raw).u.list };
        if list.is_null() {
            return Vec::new();
        }
        let list = unsafe { &*list };
        if list.num <= 0 || list.values.is_null() || list.keys.is_null() {
            return Vec::new();
        }

        (0..list.num)
            .filter_map(|index| {
                let key = unsafe { *list.keys.offset(index as isize) };
                if key.is_null() {
                    return None;
                }
                let key = unsafe { CStr::from_ptr(key) }.to_str().ok()?.to_string();
                Some((
                    key,
                    MpvNodeRef {
                        raw: unsafe { list.values.offset(index as isize) },
                        _owner: PhantomData,
                    },
                ))
            })
            .collect()
    }
}

struct NativePlayer {
    mpv: Mpv,
    path: Option<PathBuf>,
    bounds: Option<NativePlayerBounds>,
    surface: Option<NativeVideoSurface>,
}

impl NativePlayer {
    fn new() -> Result<Self, String> {
        let mpv = Mpv::with_initializer(|init| {
            init.set_option("config", false)?;
            init.set_option("load-scripts", false)?;
            init.set_option("ytdl", false)?;
            init.set_option("terminal", false)?;
            init.set_option("idle", true)?;
            init.set_option("keep-open", true)?;
            init.set_option("hwdec", "auto-safe")?;
            init.set_option("vo", "null")?;
            Ok(())
        })
        .map_err(|err| format!("failed to initialize libmpv: {err}"))?;

        Ok(Self {
            mpv,
            path: None,
            bounds: None,
            surface: None,
        })
    }

    fn set_bounds(&mut self, app: &AppHandle, bounds: NativePlayerBounds) -> Result<(), String> {
        let parent = app
            .get_webview_window("main")
            .ok_or_else(|| "native player host window is not available".to_string())?;

        match &mut self.surface {
            Some(surface) => surface.move_to(&parent, bounds)?,
            None => {
                let mut surface = NativeVideoSurface::attach(app, &parent, bounds)?;
                surface.attach_to_mpv(&self.mpv)?;
                self.surface = Some(surface);
            }
        }
        self.bounds = Some(bounds);
        Ok(())
    }

    fn playback_event_client(&self) -> Result<Mpv, String> {
        let client = self
            .mpv
            .create_client(None)
            .map_err(|err| format!("libmpv could not create playback event client: {err}"))?;
        client
            .disable_deprecated_events()
            .map_err(|err| format!("libmpv could not disable deprecated playback events: {err}"))?;
        client
            .enable_event(mpv_event_id::FileLoaded)
            .map_err(|err| format!("libmpv could not enable file-loaded events: {err}"))?;
        client
            .enable_event(mpv_event_id::EndFile)
            .map_err(|err| format!("libmpv could not enable end-file events: {err}"))?;
        client
            .enable_event(mpv_event_id::QueueOverflow)
            .map_err(|err| format!("libmpv could not enable queue-overflow events: {err}"))?;
        client
            .enable_event(mpv_event_id::Shutdown)
            .map_err(|err| format!("libmpv could not enable shutdown events: {err}"))?;
        Ok(client)
    }

    fn state(&self) -> NativePlayerState {
        let tracks = self.track_state();
        let chapters = self.chapters();
        let position = self.position_event();
        let surface_attached = self.surface.is_some();
        let surface_backend = self.surface.as_ref().map(|surface| surface.backend_label());
        let surface_render_api = self
            .surface
            .as_ref()
            .map(|surface| surface.uses_render_api())
            .unwrap_or(false);

        NativePlayerState {
            path: position.path,
            paused: position.paused,
            buffering: position.buffering,
            current_time: position.current_time,
            duration: position.duration,
            volume: position.volume,
            muted: position.muted,
            rate: position.rate,
            width: position.width,
            height: position.height,
            surface_attached,
            surface_backend,
            surface_render_api,
            audio_tracks: tracks.audio_tracks,
            subtitle_tracks: tracks.subtitle_tracks,
            selected_audio_track_id: tracks.selected_audio_track_id,
            selected_subtitle_track_id: tracks.selected_subtitle_track_id,
            chapters,
            current_chapter_id: position.current_chapter_id,
        }
    }

    fn position_event(&self) -> NativePlayerPositionEvent {
        let volume_percent = self.mpv.get_property("volume").unwrap_or(100.0);
        let current_chapter_id = self
            .mpv
            .get_property("chapter")
            .ok()
            .filter(|chapter: &i64| *chapter >= 0)
            .map(|chapter| chapter.to_string());

        NativePlayerPositionEvent {
            path: self
                .path
                .as_ref()
                .map(|path| path.to_string_lossy().to_string()),
            paused: self.mpv.get_property("pause").unwrap_or(true),
            buffering: false,
            current_time: self.mpv.get_property("time-pos").unwrap_or(0.0),
            duration: self.mpv.get_property("duration").unwrap_or(0.0),
            volume: (volume_percent / 100.0).clamp(0.0, 1.0),
            muted: self.mpv.get_property("mute").unwrap_or(false),
            rate: self.mpv.get_property("speed").unwrap_or(1.0),
            width: self.mpv.get_property("width").ok(),
            height: self.mpv.get_property("height").ok(),
            current_chapter_id,
        }
    }

    fn track_state(&self) -> NativeTrackState {
        MpvNode::get(&self.mpv, "track-list")
            .map(|tracks| parse_track_list(tracks.as_ref()))
            .unwrap_or_default()
    }

    fn chapters(&self) -> Vec<NativeChapter> {
        MpvNode::get(&self.mpv, "chapter-list")
            .map(|chapters| parse_chapter_list(chapters.as_ref()))
            .unwrap_or_default()
    }

    fn wait_until_track_list_ready(&self) -> Result<(), String> {
        for _ in 0..40 {
            let has_tracks = MpvNode::get(&self.mpv, "track-list")
                .map(|tracks| !tracks.as_ref().array_items().is_empty())
                .unwrap_or(false);
            if has_tracks {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(50));
        }

        Err("libmpv did not finish opening media for external tracks".to_string())
    }

    fn load(
        &mut self,
        path: PathBuf,
        subtitles: Vec<NativeSubtitleFile>,
        start_time: Option<f64>,
        autoplay: bool,
    ) -> Result<NativePlayerState, String> {
        let path_string = path.to_string_lossy().to_string();
        self.mpv
            .command("loadfile", &[&path_string, "replace"])
            .map_err(|err| format!("libmpv could not load file: {err}"))?;

        if !subtitles.is_empty() {
            self.wait_until_track_list_ready()?;
        }

        for subtitle in subtitles {
            subtitle.add_to_mpv(&self.mpv)?;
        }

        if let Some(start_time) = start_time.filter(|value| value.is_finite() && *value > 0.0) {
            self.mpv
                .command("seek", &[&start_time.to_string(), "absolute"])
                .map_err(|err| format!("libmpv could not seek to resume position: {err}"))?;
        }

        if autoplay {
            self.mpv
                .set_property("pause", false)
                .map_err(|err| format!("libmpv could not start playback: {err}"))?;
        } else {
            self.mpv
                .set_property("pause", true)
                .map_err(|err| format!("libmpv could not pause playback: {err}"))?;
        }

        self.path = Some(path);
        Ok(self.state())
    }

    fn load_visible(
        &mut self,
        path: PathBuf,
        subtitles: Vec<NativeSubtitleFile>,
        start_time: Option<f64>,
        autoplay: bool,
    ) -> Result<NativePlayerState, String> {
        if self.surface.is_none() {
            return Err("native video surface is not attached".to_string());
        }

        self.load(path, subtitles, start_time, autoplay)
    }

    fn select_track(
        &mut self,
        property: &str,
        id: Option<String>,
    ) -> Result<NativePlayerState, String> {
        match id {
            Some(id) => {
                let track_id = id
                    .trim()
                    .parse::<i64>()
                    .map_err(|_| "track id must be numeric".to_string())?;
                self.mpv
                    .set_property(property, track_id)
                    .map_err(|err| format!("libmpv could not select track: {err}"))?;
            }
            None => {
                self.mpv
                    .set_property(property, "no")
                    .map_err(|err| format!("libmpv could not disable track: {err}"))?;
            }
        }
        Ok(self.state())
    }

    fn set_surface_visible(&mut self, visible: bool) -> Result<(), String> {
        if let Some(surface) = &self.surface {
            surface.set_visible(visible)?;
        }
        Ok(())
    }
}

fn empty_state() -> NativePlayerState {
    NativePlayerState {
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
        surface_attached: false,
        surface_backend: None,
        surface_render_api: false,
        audio_tracks: Vec::new(),
        subtitle_tracks: Vec::new(),
        selected_audio_track_id: None,
        selected_subtitle_track_id: None,
        chapters: Vec::new(),
        current_chapter_id: None,
    }
}

struct NativeSubtitleFile {
    path: PathBuf,
    label: Option<String>,
    language: Option<String>,
}

impl NativeSubtitleFile {
    fn add_to_mpv(&self, mpv: &Mpv) -> Result<(), String> {
        let path = self.path.to_string_lossy().to_string();
        match (&self.label, &self.language) {
            (Some(label), Some(language)) => {
                mpv.command("sub-add", &[&path, "auto", label, language])
            }
            (Some(label), None) => mpv.command("sub-add", &[&path, "auto", label]),
            (None, Some(language)) => mpv.command("sub-add", &[&path, "auto", "", language]),
            (None, None) => mpv.command("sub-add", &[&path, "auto"]),
        }
        .map_err(|err| format!("libmpv could not add subtitle file: {err}"))
    }
}

fn blank_option(value: &Option<String>) -> Option<String> {
    value.as_deref().and_then(|value| {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.to_string())
        }
    })
}

fn node_string_field(node: MpvNodeRef<'_>, field: &str) -> Option<String> {
    node.map_entries().into_iter().find_map(|(key, value)| {
        if key == field {
            value
                .to_str()
                .or_else(|| value.to_i64().map(|number| number.to_string()))
        } else {
            None
        }
    })
}

fn node_f64_field(node: MpvNodeRef<'_>, field: &str) -> Option<f64> {
    node.map_entries().into_iter().find_map(|(key, value)| {
        if key == field {
            value
                .to_f64()
                .or_else(|| value.to_i64().map(|number| number as f64))
        } else {
            None
        }
    })
}

fn node_bool_field(node: MpvNodeRef<'_>, field: &str) -> Option<bool> {
    node.map_entries().into_iter().find_map(
        |(key, value)| {
            if key == field { value.to_bool() } else { None }
        },
    )
}

fn blank_to_none(value: Option<String>) -> Option<String> {
    value.and_then(|value| {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.to_string())
        }
    })
}

fn parse_track_list(node: MpvNodeRef<'_>) -> NativeTrackState {
    let mut state = NativeTrackState::default();

    for track in node.array_items() {
        let Some(kind) = node_string_field(track, "type") else {
            continue;
        };
        if kind != "audio" && kind != "sub" {
            continue;
        }
        let Some(id) = node_string_field(track, "id") else {
            continue;
        };
        let selected = node_bool_field(track, "selected").unwrap_or(false);
        let native_track = NativeTrack {
            id: id.clone(),
            title: blank_to_none(node_string_field(track, "title")),
            language: blank_to_none(node_string_field(track, "lang")),
        };

        if kind == "audio" {
            if selected {
                state.selected_audio_track_id = Some(id);
            }
            state.audio_tracks.push(native_track);
        } else {
            if selected {
                state.selected_subtitle_track_id = Some(id);
            }
            state.subtitle_tracks.push(native_track);
        }
    }

    state
}

fn parse_chapter_list(node: MpvNodeRef<'_>) -> Vec<NativeChapter> {
    node.array_items()
        .into_iter()
        .enumerate()
        .map(|(index, chapter)| NativeChapter {
            id: index.to_string(),
            title: blank_to_none(node_string_field(chapter, "title")),
            start_time: node_f64_field(chapter, "time").unwrap_or(0.0),
        })
        .collect()
}

fn reject_url_or_scheme(path: &str) -> Result<(), String> {
    if path.contains("://") || path.starts_with("file:") {
        return Err("native player only accepts local filesystem paths".to_string());
    }
    Ok(())
}

fn canonical_allowed_roots(allowed_roots: &[String]) -> Result<Vec<PathBuf>, String> {
    if allowed_roots.is_empty() {
        return Err("native player requires an approved library root".to_string());
    }

    allowed_roots
        .iter()
        .map(|root| {
            let trimmed = root.trim();
            if trimmed.is_empty() {
                return Err("approved library root is empty".to_string());
            }
            reject_url_or_scheme(trimmed)?;
            let canonical = Path::new(trimmed)
                .canonicalize()
                .map_err(|err| format!("cannot resolve approved library root: {err}"))?;
            if !canonical.is_dir() {
                return Err("approved library root is not a directory".to_string());
            }
            Ok(canonical)
        })
        .collect()
}

fn canonical_local_file(path: &str, allowed_roots: &[String]) -> Result<PathBuf, String> {
    let trimmed = path.trim();
    if trimmed.is_empty() {
        return Err("media path is empty".to_string());
    }
    reject_url_or_scheme(trimmed)?;

    let path = Path::new(trimmed);
    let canonical = path
        .canonicalize()
        .map_err(|err| format!("cannot resolve media path: {err}"))?;
    if !canonical.is_file() {
        return Err("media path is not a file".to_string());
    }
    let roots = canonical_allowed_roots(allowed_roots)?;
    if !roots.iter().any(|root| canonical.starts_with(root)) {
        return Err("media path is outside the approved library root".to_string());
    }

    Ok(canonical)
}

fn canonical_subtitle_files(
    subtitles: &[NativeSubtitleLoadOptions],
    allowed_roots: &[String],
) -> Result<Vec<NativeSubtitleFile>, String> {
    subtitles
        .iter()
        .map(|subtitle| {
            Ok(NativeSubtitleFile {
                path: canonical_local_file(&subtitle.path, allowed_roots)?,
                label: blank_option(&subtitle.label),
                language: blank_option(&subtitle.language),
            })
        })
        .collect()
}

fn screenshot_root() -> PathBuf {
    if let Ok(path) = std::env::var("MELEARNER_SCREENSHOT_DIR") {
        if !path.trim().is_empty() {
            return PathBuf::from(path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(home) = std::env::var("USERPROFILE") {
            return PathBuf::from(home).join("Pictures").join("melearner");
        }
    }

    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home).join("Pictures").join("melearner");
    }

    std::env::temp_dir().join("melearner-screenshots")
}

fn screenshot_file_stem(path: Option<&Path>) -> String {
    let source = path
        .and_then(Path::file_stem)
        .and_then(|stem| stem.to_str())
        .unwrap_or("capture");
    let mut out = String::new();
    for ch in source.chars().take(64) {
        if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
            out.push(ch);
        } else {
            out.push('_');
        }
    }
    if out.is_empty() {
        "capture".to_string()
    } else {
        out
    }
}

fn screenshot_output_path(path: Option<&Path>) -> Result<PathBuf, String> {
    let root = screenshot_root();
    std::fs::create_dir_all(&root)
        .map_err(|err| format!("failed to create screenshot directory: {err}"))?;
    let suffix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|err| format!("system clock is before unix epoch: {err}"))?
        .as_nanos();
    Ok(root.join(format!("{}-{suffix}.png", screenshot_file_stem(path))))
}

fn with_player<T>(f: impl FnOnce(&mut NativePlayer) -> Result<T, String>) -> Result<T, String> {
    let mut guard = player_slot()
        .lock()
        .map_err(|_| "native player lock is poisoned".to_string())?;
    if guard.is_none() {
        *guard = Some(NativePlayer::new()?);
    }
    f(guard.as_mut().expect("native player initialized"))
}

fn with_existing_player<T>(
    f: impl FnOnce(&mut NativePlayer) -> Result<T, String>,
) -> Result<Option<T>, String> {
    let mut guard = player_slot()
        .lock()
        .map_err(|_| "native player lock is poisoned".to_string())?;
    match guard.as_mut() {
        Some(player) => f(player).map(Some),
        None => Ok(None),
    }
}

fn next_position_event_run() -> u64 {
    let mut guard = position_event_slot()
        .lock()
        .expect("position event run lock poisoned");
    *guard = guard.wrapping_add(1);
    *guard
}

fn is_position_event_run_current(run_id: u64) -> bool {
    position_event_slot()
        .lock()
        .map(|guard| *guard == run_id)
        .unwrap_or(false)
}

fn stop_position_events() {
    let _ = next_position_event_run();
}

fn next_playback_event_run() -> u64 {
    let mut guard = playback_event_slot()
        .lock()
        .expect("playback event run lock poisoned");
    *guard = guard.wrapping_add(1);
    *guard
}

fn is_playback_event_run_current(run_id: u64) -> bool {
    playback_event_slot()
        .lock()
        .map(|guard| *guard == run_id)
        .unwrap_or(false)
}

fn stop_playback_events() {
    let _ = next_playback_event_run();
}

fn emit_native_error(app: &AppHandle, message: &str) {
    let _ = app.emit(
        EVENT_ERROR,
        NativePlayerErrorEvent {
            message: message.to_string(),
        },
    );
}

fn emit_native_state(app: &AppHandle, state: &NativePlayerState) -> Result<(), String> {
    app.emit(EVENT_STATE, state.clone())
        .map_err(|err| format!("native player could not emit state event: {err}"))?;
    app.emit(
        EVENT_TRACKS,
        NativePlayerTracksEvent {
            audio_tracks: state.audio_tracks.clone(),
            subtitle_tracks: state.subtitle_tracks.clone(),
            selected_audio_track_id: state.selected_audio_track_id.clone(),
            selected_subtitle_track_id: state.selected_subtitle_track_id.clone(),
        },
    )
    .map_err(|err| format!("native player could not emit tracks event: {err}"))?;
    app.emit(
        EVENT_CHAPTERS,
        NativePlayerChaptersEvent {
            chapters: state.chapters.clone(),
            current_chapter_id: state.current_chapter_id.clone(),
        },
    )
    .map_err(|err| format!("native player could not emit chapters event: {err}"))?;
    Ok(())
}

fn emit_native_position(
    app: &AppHandle,
    position: &NativePlayerPositionEvent,
) -> Result<(), String> {
    app.emit(EVENT_POSITION, position.clone())
        .map_err(|err| format!("native player could not emit position event: {err}"))
}

fn emit_file_loaded(app: &AppHandle, state: &NativePlayerState) -> Result<(), String> {
    if let Some(path) = &state.path {
        app.emit(
            EVENT_FILE_LOADED,
            NativePlayerFileLoadedEvent { path: path.clone() },
        )
        .map_err(|err| format!("native player could not emit file-loaded event: {err}"))?;
    }
    Ok(())
}

fn finish_state_command(
    app: &AppHandle,
    result: Result<NativePlayerState, String>,
) -> Result<NativePlayerState, String> {
    match result {
        Ok(state) => {
            emit_native_state(app, &state)?;
            Ok(state)
        }
        Err(err) => {
            emit_native_error(app, &err);
            Err(err)
        }
    }
}

fn start_position_events(app: AppHandle) {
    let run_id = next_position_event_run();
    std::thread::spawn(move || {
        loop {
            std::thread::sleep(std::time::Duration::from_millis(500));
            if !is_position_event_run_current(run_id) {
                break;
            }

            match with_existing_player(|player| Ok(player.position_event())) {
                Ok(Some(position)) => {
                    let _ = emit_native_position(&app, &position);
                }
                Ok(None) => break,
                Err(err) => {
                    emit_native_error(&app, &err);
                    break;
                }
            }
        }
    });
}

fn start_playback_events(app: AppHandle, event_client: Mpv) {
    let run_id = next_playback_event_run();
    std::thread::spawn(move || {
        loop {
            if !is_playback_event_run_current(run_id) {
                break;
            }

            let Some(event) = event_client.wait_event(0.25) else {
                continue;
            };

            if !is_playback_event_run_current(run_id) {
                break;
            }

            match event {
                Ok(Event::FileLoaded) => {
                    if let Ok(Some(state)) = with_existing_player(|player| Ok(player.state())) {
                        let _ = emit_native_state(&app, &state);
                        let _ = emit_file_loaded(&app, &state);
                    }
                }
                Ok(Event::EndFile(reason)) => {
                    if reason == mpv_end_file_reason::Eof {
                        if let Ok(Some(state)) = with_existing_player(|player| Ok(player.state())) {
                            let _ = app.emit(EVENT_END_FILE, state);
                        }
                    } else if reason == mpv_end_file_reason::Quit {
                        break;
                    }
                }
                Ok(Event::QueueOverflow) => {
                    emit_native_error(&app, "libmpv playback event queue overflowed");
                }
                Ok(Event::Shutdown) => break,
                Err(err) => {
                    emit_native_error(&app, &format!("libmpv playback event failed: {err}"));
                    break;
                }
                _ => {}
            }
        }
    });
}

#[tauri::command]
pub fn native_player_load(
    app: AppHandle,
    options: NativePlayerLoadOptions,
) -> Result<NativePlayerState, String> {
    stop_position_events();
    stop_playback_events();
    let result = canonical_local_file(&options.path, &options.allowed_roots).and_then(|path| {
        let subtitles = canonical_subtitle_files(&options.subtitles, &options.allowed_roots)?;
        with_player(|player| {
            let event_client = player.playback_event_client()?;
            let state = player.load_visible(
                path,
                subtitles,
                options.start_time,
                options.autoplay.unwrap_or(false),
            )?;
            Ok((state, event_client))
        })
    });
    let (state, event_client) = match result {
        Ok((state, event_client)) => {
            emit_native_state(&app, &state)?;
            (state, event_client)
        }
        Err(err) => {
            emit_native_error(&app, &err);
            return Err(err);
        }
    };
    start_position_events(app.clone());
    start_playback_events(app, event_client);
    Ok(state)
}

#[tauri::command]
pub fn native_player_state(app: AppHandle) -> Result<NativePlayerState, String> {
    finish_state_command(&app, with_player(|player| Ok(player.state())))
}

#[tauri::command]
pub fn native_player_play(app: AppHandle) -> Result<NativePlayerState, String> {
    let result = with_player(|player| {
        player
            .mpv
            .set_property("pause", false)
            .map_err(|err| format!("libmpv could not play: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_pause(app: AppHandle) -> Result<NativePlayerState, String> {
    let result = with_player(|player| {
        player
            .mpv
            .set_property("pause", true)
            .map_err(|err| format!("libmpv could not pause: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_seek(
    app: AppHandle,
    options: NativePlayerSeekOptions,
) -> Result<NativePlayerState, String> {
    if !options.seconds.is_finite() {
        let err = "seek target is not finite".to_string();
        emit_native_error(&app, &err);
        return Err(err);
    }

    let result = with_player(|player| {
        let seconds = options.seconds.to_string();
        let mode = match options.mode {
            NativePlayerSeekMode::Absolute => "absolute",
            NativePlayerSeekMode::Relative => "relative",
        };
        player
            .mpv
            .command("seek", &[&seconds, mode])
            .map_err(|err| format!("libmpv could not seek: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_set_volume(app: AppHandle, volume: f64) -> Result<NativePlayerState, String> {
    if !volume.is_finite() {
        let err = "volume is not finite".to_string();
        emit_native_error(&app, &err);
        return Err(err);
    }

    let result = with_player(|player| {
        player
            .mpv
            .set_property("volume", volume.clamp(0.0, 1.0) * 100.0)
            .map_err(|err| format!("libmpv could not set volume: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_set_muted(app: AppHandle, muted: bool) -> Result<NativePlayerState, String> {
    let result = with_player(|player| {
        player
            .mpv
            .set_property("mute", muted)
            .map_err(|err| format!("libmpv could not set mute: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_set_rate(app: AppHandle, rate: f64) -> Result<NativePlayerState, String> {
    if !rate.is_finite() || !(0.25..=4.0).contains(&rate) {
        let err = "playback rate must be between 0.25 and 4.0".to_string();
        emit_native_error(&app, &err);
        return Err(err);
    }

    let result = with_player(|player| {
        player
            .mpv
            .set_property("speed", rate)
            .map_err(|err| format!("libmpv could not set playback rate: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_select_audio_track(
    app: AppHandle,
    id: Option<String>,
) -> Result<NativePlayerState, String> {
    finish_state_command(&app, with_player(|player| player.select_track("aid", id)))
}

#[tauri::command]
pub fn native_player_select_subtitle_track(
    app: AppHandle,
    id: Option<String>,
) -> Result<NativePlayerState, String> {
    finish_state_command(&app, with_player(|player| player.select_track("sid", id)))
}

#[tauri::command]
pub fn native_player_select_chapter(
    app: AppHandle,
    id: String,
) -> Result<NativePlayerState, String> {
    let chapter_id = match id.trim().parse::<i64>() {
        Ok(chapter_id) => chapter_id,
        Err(_) => {
            let err = "chapter id must be numeric".to_string();
            emit_native_error(&app, &err);
            return Err(err);
        }
    };
    if chapter_id < 0 {
        let err = "chapter id must not be negative".to_string();
        emit_native_error(&app, &err);
        return Err(err);
    }

    let result = with_player(|player| {
        player
            .mpv
            .set_property("chapter", chapter_id)
            .map_err(|err| format!("libmpv could not select chapter: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub async fn native_player_set_bounds(
    app: AppHandle,
    bounds: NativePlayerBounds,
) -> Result<(), String> {
    if bounds.width <= 0 || bounds.height <= 0 || bounds.scale_factor <= 0.0 {
        return Err("native player bounds are invalid".to_string());
    }
    with_player(|player| player.set_bounds(&app, bounds))
}

#[tauri::command]
pub fn native_player_set_surface_visible(app: AppHandle, visible: bool) -> Result<(), String> {
    let result = with_existing_player(|player| player.set_surface_visible(visible)).map(|_| ());
    if let Err(err) = &result {
        emit_native_error(&app, err);
    }
    result
}

#[tauri::command]
pub fn native_player_step_frame(app: AppHandle) -> Result<NativePlayerState, String> {
    let result = with_player(|player| {
        player
            .mpv
            .command("frame-step", &[])
            .map_err(|err| format!("libmpv could not step frame: {err}"))?;
        Ok(player.state())
    });
    finish_state_command(&app, result)
}

#[tauri::command]
pub fn native_player_screenshot(app: AppHandle) -> Result<String, String> {
    let result = with_player(|player| {
        let output = screenshot_output_path(player.path.as_deref())?;
        let output_string = output.to_string_lossy().to_string();
        player
            .mpv
            .command("screenshot-to-file", &[&output_string, "video"])
            .map_err(|err| format!("libmpv could not take screenshot: {err}"))?;
        Ok(output_string)
    });
    if let Err(err) = &result {
        emit_native_error(&app, err);
    }
    result
}

#[tauri::command]
pub fn native_player_destroy(app: AppHandle) -> Result<(), String> {
    stop_position_events();
    stop_playback_events();
    let mut guard = player_slot()
        .lock()
        .map_err(|_| "native player lock is poisoned".to_string())?;
    *guard = None;
    let _ = app.emit(EVENT_STATE, empty_state());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::process::Command;
    use std::thread;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    struct MediaFixture {
        root: PathBuf,
        file: PathBuf,
    }

    struct ExternalSubtitleFixture {
        media: MediaFixture,
        subtitles: Vec<NativeSubtitleFile>,
    }

    impl Drop for MediaFixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    fn temp_media_file() -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!("melearner-native-player-{suffix}.mp4"));
        fs::write(&path, b"fixture").expect("write fixture");
        path
    }

    fn multitrack_media_fixture() -> Option<MediaFixture> {
        if !Command::new("ffmpeg")
            .arg("-version")
            .status()
            .map(|status| status.success())
            .unwrap_or(false)
        {
            eprintln!("ffmpeg is unavailable; skipping native player media fixture test");
            return None;
        }

        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("melearner-native-player-media-{suffix}"));
        fs::create_dir(&root).expect("create media fixture root");
        fs::write(
            root.join("en.srt"),
            "1\n00:00:00,000 --> 00:00:01,000\nEnglish caption\n",
        )
        .expect("write english subtitle");
        fs::write(
            root.join("es.srt"),
            "1\n00:00:00,000 --> 00:00:01,000\nSpanish caption\n",
        )
        .expect("write spanish subtitle");
        fs::write(
            root.join("chapters.ffmetadata"),
            ";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=Intro\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=2000\ntitle=Review\n",
        )
        .expect("write chapters");

        let file = root.join("fixture.mkv");
        let status = Command::new("ffmpeg")
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "testsrc=size=160x90:rate=1:duration=2",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:duration=2",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=880:duration=2",
                "-i",
                &root.join("en.srt").to_string_lossy(),
                "-i",
                &root.join("es.srt").to_string_lossy(),
                "-i",
                &root.join("chapters.ffmetadata").to_string_lossy(),
                "-map",
                "0:v:0",
                "-map",
                "1:a:0",
                "-map",
                "2:a:0",
                "-map",
                "3:s:0",
                "-map",
                "4:s:0",
                "-map_chapters",
                "5",
                "-metadata:s:a:0",
                "language=eng",
                "-metadata:s:a:0",
                "title=English audio",
                "-metadata:s:a:1",
                "language=jpn",
                "-metadata:s:a:1",
                "title=Japanese audio",
                "-metadata:s:s:0",
                "language=eng",
                "-metadata:s:s:0",
                "title=English captions",
                "-metadata:s:s:1",
                "language=spa",
                "-metadata:s:s:1",
                "title=Spanish captions",
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-c:a",
                "aac",
                "-c:s",
                "srt",
                "-t",
                "2",
                &file.to_string_lossy(),
            ])
            .status()
            .expect("run ffmpeg fixture command");
        assert!(status.success(), "ffmpeg fixture command failed");

        Some(MediaFixture { root, file })
    }

    fn external_subtitle_fixture() -> Option<ExternalSubtitleFixture> {
        let Some(media) = multitrack_media_fixture() else {
            return None;
        };
        let root = media.root.clone();
        let english = root.join("external.en.srt");
        let spanish = root.join("external.es.vtt");
        fs::write(
            &english,
            "1\n00:00:00,000 --> 00:00:01,000\nExternal English\n",
        )
        .expect("write external english subtitle");
        fs::write(
            &spanish,
            "WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nExternal Spanish\n",
        )
        .expect("write external spanish subtitle");

        Some(ExternalSubtitleFixture {
            media,
            subtitles: vec![
                NativeSubtitleFile {
                    path: english,
                    label: Some("External English".to_string()),
                    language: Some("eng".to_string()),
                },
                NativeSubtitleFile {
                    path: spanish,
                    label: Some("External Spanish".to_string()),
                    language: Some("spa".to_string()),
                },
            ],
        })
    }

    fn wait_for_state(
        player: &NativePlayer,
        predicate: impl Fn(&NativePlayerState) -> bool,
    ) -> NativePlayerState {
        for _ in 0..50 {
            let state = player.state();
            if predicate(&state) {
                return state;
            }
            thread::sleep(Duration::from_millis(100));
        }
        player.state()
    }

    #[test]
    fn canonical_local_file_rejects_urls() {
        let root = std::env::temp_dir().to_string_lossy().to_string();

        assert!(canonical_local_file("https://example.com/video.mp4", &[root.clone()]).is_err());
        assert!(canonical_local_file("file:///tmp/video.mp4", &[root]).is_err());
    }

    #[test]
    fn canonical_local_file_accepts_file_under_approved_root() {
        let root = std::env::temp_dir().join(format!(
            "melearner-native-player-root-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("system time before unix epoch")
                .as_nanos()
        ));
        fs::create_dir(&root).expect("create temp root");
        let file = root.join("lesson.mp4");
        fs::write(&file, b"fixture").expect("write fixture");
        let roots = vec![root.to_string_lossy().to_string()];
        let canonical =
            canonical_local_file(&file.to_string_lossy(), &roots).expect("canonical file");

        assert!(canonical.is_file());

        let _ = fs::remove_file(file);
        let _ = fs::remove_dir(root);
    }

    #[test]
    fn canonical_local_file_rejects_file_outside_approved_root() {
        let root = std::env::temp_dir().join(format!(
            "melearner-native-player-root-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("system time before unix epoch")
                .as_nanos()
        ));
        fs::create_dir(&root).expect("create temp root");
        let file = temp_media_file();
        let roots = vec![root.to_string_lossy().to_string()];

        assert!(canonical_local_file(&file.to_string_lossy(), &roots).is_err());

        let _ = fs::remove_file(file);
        let _ = fs::remove_dir(root);
    }

    #[test]
    fn canonical_subtitle_files_rejects_file_outside_approved_root() {
        let root = std::env::temp_dir().join(format!(
            "melearner-native-player-root-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("system time before unix epoch")
                .as_nanos()
        ));
        fs::create_dir(&root).expect("create temp root");
        let subtitle = temp_media_file();
        let roots = vec![root.to_string_lossy().to_string()];
        let subtitles = vec![NativeSubtitleLoadOptions {
            path: subtitle.to_string_lossy().to_string(),
            label: Some(" English ".to_string()),
            language: Some(" en ".to_string()),
        }];

        assert!(canonical_subtitle_files(&subtitles, &roots).is_err());

        let _ = fs::remove_file(subtitle);
        let _ = fs::remove_dir(root);
    }

    #[test]
    fn screenshot_file_stem_sanitizes_source_name() {
        let path = PathBuf::from("13 - Shifting Operations!.mkv");

        assert_eq!(
            screenshot_file_stem(Some(&path)),
            "13_-_Shifting_Operations_"
        );
    }

    #[test]
    fn surface_rect_uses_window_origin_and_scale() {
        let bounds = NativePlayerBounds {
            x: 12,
            y: 34,
            width: 640,
            height: 360,
            scale_factor: 1.5,
        };
        let origin = tauri::PhysicalPosition::new(100, 200);

        assert_eq!(
            surface::surface_rect_for_bounds(origin, bounds).expect("surface rect"),
            surface::NativeSurfaceRect {
                x: 118,
                y: 251,
                width: 960,
                height: 540,
            }
        );
    }

    #[test]
    fn native_surface_window_labels_are_unique() {
        let first = surface::next_surface_window_label().expect("first surface label");
        let second = surface::next_surface_window_label().expect("second surface label");

        assert!(first.starts_with("native-player-surface"));
        assert!(second.starts_with("native-player-surface"));
        assert_ne!(first, second);
    }

    #[test]
    fn native_surface_backend_labels_window_handle_path() {
        let backend = surface::NativeSurfaceBackend::WindowHandle("xcb");

        assert_eq!(backend.label(), "window-handle:xcb");
        assert!(!backend.uses_render_api());
    }

    #[test]
    fn native_surface_backend_labels_render_api_path() {
        let backend = surface::NativeSurfaceBackend::RenderApi("opengl");

        assert_eq!(backend.label(), "render-api:opengl");
        assert!(backend.uses_render_api());
    }

    #[test]
    fn native_surface_backend_preference_is_explicit() {
        assert_eq!(
            surface::NativeSurfaceBackendPreference::from_env_value(None)
                .expect("default backend preference"),
            surface::NativeSurfaceBackendPreference::WindowHandle
        );
        assert_eq!(
            surface::NativeSurfaceBackendPreference::from_env_value(Some("render-api"))
                .expect("render api backend preference"),
            surface::NativeSurfaceBackendPreference::RenderApi
        );
        assert!(
            surface::NativeSurfaceBackendPreference::from_env_value(Some("browser-video"))
                .expect_err("invalid backend preference should fail")
                .contains("MELEARNER_SURFACE_BACKEND")
        );
    }

    #[test]
    fn native_player_visible_load_requires_surface() {
        let file = temp_media_file();
        let mut player = NativePlayer::new().expect("create native player");

        assert_eq!(
            player
                .load_visible(file.clone(), Vec::new(), None, false)
                .expect_err("visible load should require surface"),
            "native video surface is not attached"
        );

        let _ = fs::remove_file(file);
    }

    #[test]
    fn empty_state_reports_no_native_surface() {
        let state = empty_state();

        assert!(!state.surface_attached);
        assert_eq!(state.surface_backend, None);
    }

    #[test]
    fn native_player_surface_visibility_is_safe_without_surface() {
        let mut player = NativePlayer::new().expect("create native player");

        player
            .set_surface_visible(false)
            .expect("hide missing surface");
        player
            .set_surface_visible(true)
            .expect("show missing surface");
    }

    #[test]
    fn native_player_reports_tracks_and_chapters() {
        let Some(fixture) = multitrack_media_fixture() else {
            return;
        };
        let mut player = NativePlayer::new().expect("create native player");

        player
            .load(fixture.file.clone(), Vec::new(), None, false)
            .expect("load media fixture");
        let state = wait_for_state(&player, |state| {
            state.audio_tracks.len() == 2
                && state.subtitle_tracks.len() == 2
                && state.chapters.len() == 2
        });

        assert_eq!(state.audio_tracks.len(), 2);
        assert_eq!(
            state.audio_tracks[0].title.as_deref(),
            Some("English audio")
        );
        assert_eq!(state.audio_tracks[0].language.as_deref(), Some("eng"));
        assert_eq!(state.subtitle_tracks.len(), 2);
        assert_eq!(
            state.subtitle_tracks[1].title.as_deref(),
            Some("Spanish captions")
        );
        assert_eq!(state.chapters.len(), 2);
        assert_eq!(state.chapters[0].title.as_deref(), Some("Intro"));
        assert_eq!(state.chapters[1].start_time, 1.0);
    }

    #[test]
    fn native_player_reports_external_subtitle_tracks() {
        let Some(fixture) = external_subtitle_fixture() else {
            return;
        };
        let mut player = NativePlayer::new().expect("create native player");

        player
            .load(fixture.media.file.clone(), fixture.subtitles, None, false)
            .expect("load media fixture");
        let state = wait_for_state(&player, |state| {
            state
                .subtitle_tracks
                .iter()
                .any(|track| track.title.as_deref() == Some("External English"))
                && state
                    .subtitle_tracks
                    .iter()
                    .any(|track| track.title.as_deref() == Some("External Spanish"))
        });

        assert!(state.subtitle_tracks.iter().any(|track| {
            track.title.as_deref() == Some("External English")
                && track.language.as_deref() == Some("eng")
        }));
        assert!(state.subtitle_tracks.iter().any(|track| {
            track.title.as_deref() == Some("External Spanish")
                && track.language.as_deref() == Some("spa")
        }));
    }
}
