use libmpv::{FileState, Mpv};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

static PLAYER: OnceLock<Mutex<Option<NativePlayer>>> = OnceLock::new();

fn player_slot() -> &'static Mutex<Option<NativePlayer>> {
    PLAYER.get_or_init(|| Mutex::new(None))
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
    audio_tracks: Vec<NativeTrack>,
    subtitle_tracks: Vec<NativeTrack>,
    selected_audio_track_id: Option<String>,
    selected_subtitle_track_id: Option<String>,
}

#[derive(Debug, Deserialize)]
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
    start_time: Option<f64>,
    autoplay: Option<bool>,
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

struct NativePlayer {
    mpv: Mpv,
    path: Option<PathBuf>,
    bounds: Option<NativePlayerBounds>,
}

impl NativePlayer {
    fn new() -> Result<Self, String> {
        let mpv = Mpv::with_initializer(|init| {
            init.set_property("config", false)?;
            init.set_property("load-scripts", false)?;
            init.set_property("ytdl", false)?;
            init.set_property("terminal", false)?;
            init.set_property("idle", true)?;
            init.set_property("keep-open", true)?;
            init.set_property("hwdec", "auto-safe")?;
            init.set_property("vo", "libmpv")?;
            Ok(())
        })
        .map_err(|err| format!("failed to initialize libmpv: {err}"))?;

        Ok(Self {
            mpv,
            path: None,
            bounds: None,
        })
    }

    fn state(&self) -> NativePlayerState {
        let paused = self.mpv.get_property("pause").unwrap_or(true);
        let current_time = self.mpv.get_property("time-pos").unwrap_or(0.0);
        let duration = self.mpv.get_property("duration").unwrap_or(0.0);
        let muted = self.mpv.get_property("mute").unwrap_or(false);
        let volume_percent = self.mpv.get_property("volume").unwrap_or(100.0);
        let rate = self.mpv.get_property("speed").unwrap_or(1.0);
        let width = self.mpv.get_property("width").ok();
        let height = self.mpv.get_property("height").ok();

        NativePlayerState {
            path: self
                .path
                .as_ref()
                .map(|path| path.to_string_lossy().to_string()),
            paused,
            buffering: false,
            current_time,
            duration,
            volume: (volume_percent / 100.0).clamp(0.0, 1.0),
            muted,
            rate,
            width,
            height,
            audio_tracks: Vec::new(),
            subtitle_tracks: Vec::new(),
            selected_audio_track_id: None,
            selected_subtitle_track_id: None,
        }
    }

    fn load(
        &mut self,
        path: PathBuf,
        start_time: Option<f64>,
        autoplay: bool,
    ) -> Result<NativePlayerState, String> {
        let path_string = path.to_string_lossy().to_string();
        self.mpv
            .playlist_load_files(&[(&path_string, FileState::Replace, None)])
            .map_err(|err| format!("libmpv could not load file: {err}"))?;

        if let Some(start_time) = start_time.filter(|value| value.is_finite() && *value > 0.0) {
            self.mpv
                .seek_absolute(start_time)
                .map_err(|err| format!("libmpv could not seek to resume position: {err}"))?;
        }

        if autoplay {
            self.mpv
                .unpause()
                .map_err(|err| format!("libmpv could not start playback: {err}"))?;
        } else {
            self.mpv
                .pause()
                .map_err(|err| format!("libmpv could not pause playback: {err}"))?;
        }

        self.path = Some(path);
        Ok(self.state())
    }
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

fn with_player<T>(f: impl FnOnce(&mut NativePlayer) -> Result<T, String>) -> Result<T, String> {
    let mut guard = player_slot()
        .lock()
        .map_err(|_| "native player lock is poisoned".to_string())?;
    if guard.is_none() {
        *guard = Some(NativePlayer::new()?);
    }
    f(guard.as_mut().expect("native player initialized"))
}

#[tauri::command]
pub fn native_player_load(options: NativePlayerLoadOptions) -> Result<NativePlayerState, String> {
    let path = canonical_local_file(&options.path, &options.allowed_roots)?;
    with_player(|player| player.load(path, options.start_time, options.autoplay.unwrap_or(false)))
}

#[tauri::command]
pub fn native_player_state() -> Result<NativePlayerState, String> {
    with_player(|player| Ok(player.state()))
}

#[tauri::command]
pub fn native_player_play() -> Result<NativePlayerState, String> {
    with_player(|player| {
        player
            .mpv
            .unpause()
            .map_err(|err| format!("libmpv could not play: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_pause() -> Result<NativePlayerState, String> {
    with_player(|player| {
        player
            .mpv
            .pause()
            .map_err(|err| format!("libmpv could not pause: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_seek(options: NativePlayerSeekOptions) -> Result<NativePlayerState, String> {
    if !options.seconds.is_finite() {
        return Err("seek target is not finite".to_string());
    }

    with_player(|player| {
        match options.mode {
            NativePlayerSeekMode::Absolute => player.mpv.seek_absolute(options.seconds),
            NativePlayerSeekMode::Relative if options.seconds >= 0.0 => {
                player.mpv.seek_forward(options.seconds)
            }
            NativePlayerSeekMode::Relative => player.mpv.seek_backward(options.seconds.abs()),
        }
        .map_err(|err| format!("libmpv could not seek: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_set_volume(volume: f64) -> Result<NativePlayerState, String> {
    if !volume.is_finite() {
        return Err("volume is not finite".to_string());
    }

    with_player(|player| {
        player
            .mpv
            .set_property("volume", volume.clamp(0.0, 1.0) * 100.0)
            .map_err(|err| format!("libmpv could not set volume: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_set_muted(muted: bool) -> Result<NativePlayerState, String> {
    with_player(|player| {
        player
            .mpv
            .set_property("mute", muted)
            .map_err(|err| format!("libmpv could not set mute: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_set_rate(rate: f64) -> Result<NativePlayerState, String> {
    if !rate.is_finite() || !(0.25..=4.0).contains(&rate) {
        return Err("playback rate must be between 0.25 and 4.0".to_string());
    }

    with_player(|player| {
        player
            .mpv
            .set_property("speed", rate)
            .map_err(|err| format!("libmpv could not set playback rate: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_set_bounds(bounds: NativePlayerBounds) -> Result<(), String> {
    if bounds.width <= 0 || bounds.height <= 0 || bounds.scale_factor <= 0.0 {
        return Err("native player bounds are invalid".to_string());
    }
    let _origin = (bounds.x, bounds.y);
    with_player(|player| {
        player.bounds = Some(bounds);
        Ok(())
    })
}

#[tauri::command]
pub fn native_player_step_frame() -> Result<NativePlayerState, String> {
    with_player(|player| {
        player
            .mpv
            .seek_frame()
            .map_err(|err| format!("libmpv could not step frame: {err}"))?;
        Ok(player.state())
    })
}

#[tauri::command]
pub fn native_player_screenshot() -> Result<String, String> {
    with_player(|player| {
        player
            .mpv
            .screenshot_video(None)
            .map_err(|err| format!("libmpv could not take screenshot: {err}"))?;
        Ok(String::new())
    })
}

#[tauri::command]
pub fn native_player_destroy() -> Result<(), String> {
    let mut guard = player_slot()
        .lock()
        .map_err(|_| "native player lock is poisoned".to_string())?;
    *guard = None;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_media_file() -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!("melearner-native-player-{suffix}.mp4"));
        fs::write(&path, b"fixture").expect("write fixture");
        path
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
        let canonical = canonical_local_file(&file.to_string_lossy(), &roots).expect("canonical file");

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
}
