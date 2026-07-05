use seahash::SeaHasher;
use serde::Serialize;
use std::collections::HashMap;
use std::hash::Hasher;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::time::UNIX_EPOCH;

#[derive(Debug, Serialize)]
pub struct PreparedMedia {
    path: String,
}

static ACTIVE_PLAYBACK_JOBS: OnceLock<Mutex<HashMap<String, u32>>> = OnceLock::new();

fn active_playback_jobs() -> &'static Mutex<HashMap<String, u32>> {
    ACTIVE_PLAYBACK_JOBS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn new_hasher() -> SeaHasher {
    SeaHasher::with_seeds(
        0x7B10D9E02B5C7A41,
        0x8E4C6F2A19D3B507,
        0xC3A5F681D924E0BB,
        0x4F6E8A1C35D7920E,
    )
}

fn media_cache_root() -> PathBuf {
    if let Ok(path) = std::env::var("MELEARNER_MEDIA_CACHE") {
        return PathBuf::from(path);
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(path) = std::env::var("LOCALAPPDATA") {
            return PathBuf::from(path).join("melearner").join("media");
        }
        if let Ok(path) = std::env::var("APPDATA") {
            return PathBuf::from(path).join("melearner").join("media");
        }
    }

    #[cfg(target_os = "macos")]
    {
        if let Ok(home) = std::env::var("HOME") {
            return PathBuf::from(home)
                .join("Library")
                .join("Caches")
                .join("melearner")
                .join("media");
        }
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    {
        if let Ok(path) = std::env::var("XDG_CACHE_HOME") {
            return PathBuf::from(path).join("melearner").join("media");
        }
        if let Ok(home) = std::env::var("HOME") {
            return PathBuf::from(home)
                .join(".cache")
                .join("melearner")
                .join("media");
        }
    }

    std::env::temp_dir().join("melearner-media")
}

fn cache_key(path: &Path, metadata: &std::fs::Metadata, media_type: &str) -> String {
    let mut hasher = new_hasher();
    hasher.write(path.to_string_lossy().as_bytes());
    hasher.write(media_type.as_bytes());
    hasher.write(&metadata.len().to_le_bytes());

    if let Ok(modified) = metadata.modified() {
        if let Ok(duration) = modified.duration_since(UNIX_EPOCH) {
            hasher.write(&duration.as_secs().to_le_bytes());
            hasher.write(&duration.subsec_nanos().to_le_bytes());
        }
    }

    format!("{:016x}", hasher.finish())
}

fn output_path(input: &Path, media_type: &str) -> Result<PathBuf, String> {
    let metadata =
        std::fs::metadata(input).map_err(|err| format!("failed to read media metadata: {err}"))?;
    let extension = if media_type == "audio" { "m4a" } else { "mp4" };
    Ok(media_cache_root().join(format!(
        "{}.{}",
        cache_key(input, &metadata, media_type),
        extension
    )))
}

fn media_tool_program(env_name: &str, program: &str) -> String {
    if let Ok(path) = std::env::var(env_name) {
        if !path.trim().is_empty() {
            return path;
        }
    }

    for prefix in [
        "/usr/bin",
        "/usr/local/bin",
        "/opt/homebrew/bin",
        "/opt/local/bin",
    ] {
        let candidate = Path::new(prefix).join(program);
        if candidate.is_file() {
            return candidate.to_string_lossy().to_string();
        }
    }

    program.to_string()
}

fn ffmpeg_program() -> String {
    media_tool_program("MELEARNER_FFMPEG", "ffmpeg")
}

fn ffprobe_program() -> String {
    media_tool_program("MELEARNER_FFPROBE", "ffprobe")
}

fn temp_output_path(output: &Path) -> PathBuf {
    let file_name = output
        .file_name()
        .map(|name| name.to_string_lossy().to_string())
        .unwrap_or_else(|| "prepared-media".to_string());
    output.with_file_name(format!("{file_name}.part"))
}

fn existing_output_ready(output: &Path) -> bool {
    let has_bytes = output
        .metadata()
        .map(|metadata| metadata.len() > 0)
        .unwrap_or(false);
    if !has_bytes {
        return false;
    }

    Command::new(ffprobe_program())
        .args(["-v", "error"])
        .arg(output)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|status| status.success())
        .unwrap_or(true)
}

fn remux_args(input: &Path, output: &Path, media_type: &str) -> Vec<String> {
    let mut args = vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
        "-nostdin".to_string(),
        "-y".to_string(),
        "-i".to_string(),
        input.to_string_lossy().to_string(),
    ];

    if media_type == "audio" {
        args.extend([
            "-vn".to_string(),
            "-map".to_string(),
            "0:a:0".to_string(),
            "-c:a".to_string(),
            "copy".to_string(),
            "-movflags".to_string(),
            "+faststart".to_string(),
            "-f".to_string(),
            "mp4".to_string(),
        ]);
    } else {
        args.extend([
            "-map".to_string(),
            "0:v:0?".to_string(),
            "-map".to_string(),
            "0:a:0?".to_string(),
            "-dn".to_string(),
            "-sn".to_string(),
            "-c".to_string(),
            "copy".to_string(),
            "-movflags".to_string(),
            "+faststart".to_string(),
            "-f".to_string(),
            "mp4".to_string(),
        ]);
    }

    args.push(output.to_string_lossy().to_string());
    args
}

fn transcode_args(input: &Path, output: &Path, media_type: &str) -> Vec<String> {
    let mut args = vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
        "-nostdin".to_string(),
        "-y".to_string(),
        "-i".to_string(),
        input.to_string_lossy().to_string(),
    ];

    if media_type == "audio" {
        args.extend([
            "-vn".to_string(),
            "-map".to_string(),
            "0:a:0".to_string(),
            "-c:a".to_string(),
            "aac".to_string(),
            "-b:a".to_string(),
            "160k".to_string(),
            "-threads".to_string(),
            "1".to_string(),
            "-f".to_string(),
            "mp4".to_string(),
        ]);
    } else {
        args.extend([
            "-map".to_string(),
            "0:v:0?".to_string(),
            "-map".to_string(),
            "0:a:0?".to_string(),
            "-dn".to_string(),
            "-sn".to_string(),
            "-c:v".to_string(),
            "libx264".to_string(),
            "-preset".to_string(),
            "veryfast".to_string(),
            "-crf".to_string(),
            "23".to_string(),
            "-pix_fmt".to_string(),
            "yuv420p".to_string(),
            "-c:a".to_string(),
            "aac".to_string(),
            "-b:a".to_string(),
            "160k".to_string(),
            "-threads".to_string(),
            "2".to_string(),
            "-filter_threads".to_string(),
            "1".to_string(),
            "-filter_complex_threads".to_string(),
            "1".to_string(),
            "-movflags".to_string(),
            "+faststart".to_string(),
            "-f".to_string(),
            "mp4".to_string(),
        ]);
    }

    args.push(output.to_string_lossy().to_string());
    args
}

fn unregister_playback_job(job_key: &str, pid: u32) {
    let Ok(mut jobs) = active_playback_jobs().lock() else {
        return;
    };

    if jobs.get(job_key).copied() == Some(pid) {
        jobs.remove(job_key);
    }
}

fn kill_process(pid: u32) {
    #[cfg(target_family = "unix")]
    let _ = Command::new("kill")
        .args(["-TERM", &pid.to_string()])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();

    #[cfg(target_os = "windows")]
    let _ = Command::new("taskkill")
        .args(["/PID", &pid.to_string(), "/T", "/F"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

fn cancel_all_playback_jobs() {
    let pids = {
        let Ok(mut jobs) = active_playback_jobs().lock() else {
            return;
        };

        jobs.drain().map(|(_, pid)| pid).collect::<Vec<_>>()
    };

    for pid in pids {
        kill_process(pid);
    }
}

fn run_ffmpeg(job_key: &str, args: Vec<String>) -> Result<(), String> {
    let child = Command::new(ffmpeg_program())
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|err| format!("failed to start ffmpeg: {err}"))?;

    let pid = child.id();
    if let Ok(mut jobs) = active_playback_jobs().lock() {
        jobs.insert(job_key.to_string(), pid);
    }

    let ffmpeg_output = child
        .wait_with_output()
        .map_err(|err| format!("failed to wait for ffmpeg: {err}"));
    unregister_playback_job(job_key, pid);
    let ffmpeg_output = ffmpeg_output?;

    if ffmpeg_output.status.success() {
        return Ok(());
    }

    let stderr = String::from_utf8_lossy(&ffmpeg_output.stderr)
        .trim()
        .to_string();
    let details = if stderr.is_empty() {
        ffmpeg_output.status.to_string()
    } else {
        format!("{}: {stderr}", ffmpeg_output.status)
    };
    Err(details)
}

fn finalize_output(temp_output: &Path, output: &Path) -> Result<(), String> {
    if !existing_output_ready(temp_output) {
        let _ = std::fs::remove_file(temp_output);
        return Err("ffmpeg produced an unreadable media file".to_string());
    }

    if output.exists() {
        let _ = std::fs::remove_file(output);
    }

    std::fs::rename(temp_output, output)
        .map_err(|err| format!("failed to move prepared media into cache: {err}"))
}

#[tauri::command]
pub async fn prepare_playback_media(
    path: String,
    media_type: String,
) -> Result<PreparedMedia, String> {
    tokio::task::spawn_blocking(move || {
        let input = PathBuf::from(path);
        if !input.exists() {
            return Err("media file does not exist".to_string());
        }

        let normalized_type = if media_type == "audio" {
            "audio"
        } else {
            "video"
        };
        let output = output_path(&input, normalized_type)?;
        let temp_output = temp_output_path(&output);
        let job_key = output.to_string_lossy().to_string();

        if existing_output_ready(&output) {
            return Ok(PreparedMedia {
                path: output.to_string_lossy().to_string(),
            });
        }
        let _ = std::fs::remove_file(&output);

        if let Some(parent) = output.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|err| format!("failed to create media cache: {err}"))?;
        }
        cancel_all_playback_jobs();
        let _ = std::fs::remove_file(&temp_output);

        let remux_result = run_ffmpeg(&job_key, remux_args(&input, &temp_output, normalized_type));
        if remux_result.is_err() {
            let _ = std::fs::remove_file(&temp_output);
            let transcode_result = run_ffmpeg(
                &job_key,
                transcode_args(&input, &temp_output, normalized_type),
            );
            if let Err(details) = transcode_result {
                let _ = std::fs::remove_file(&temp_output);
                return Err(format!(
                    "ffmpeg could not prepare this media file: {details}"
                ));
            }
        }

        finalize_output(&temp_output, &output)?;

        Ok(PreparedMedia {
            path: output.to_string_lossy().to_string(),
        })
    })
    .await
    .map_err(|err| format!("media preparation task failed: {err}"))?
}

#[tauri::command]
pub async fn cancel_playback_media(path: String, media_type: String) -> Result<(), String> {
    tokio::task::spawn_blocking(move || {
        let input = PathBuf::from(path);
        let normalized_type = if media_type == "audio" {
            "audio"
        } else {
            "video"
        };
        let Ok(output) = output_path(&input, normalized_type) else {
            return;
        };
        let job_key = output.to_string_lossy().to_string();
        let pid = active_playback_jobs()
            .lock()
            .ok()
            .and_then(|mut jobs| jobs.remove(&job_key));

        if let Some(pid) = pid {
            kill_process(pid);
        }
        let _ = std::fs::remove_file(temp_output_path(&output));
    })
    .await
    .map_err(|err| format!("media cancellation task failed: {err}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_file(name: &str) -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!("melearner-media-{name}-{suffix}.bin"));
        fs::write(&path, b"fixture").expect("write fixture");
        path
    }

    #[test]
    fn output_path_uses_media_type_extension() {
        let input = temp_file("output-path");

        let video = output_path(&input, "video").expect("video output path");
        let audio = output_path(&input, "audio").expect("audio output path");

        assert_eq!(video.extension().and_then(|ext| ext.to_str()), Some("mp4"));
        assert_eq!(audio.extension().and_then(|ext| ext.to_str()), Some("m4a"));
        assert_ne!(video, audio);

        let _ = fs::remove_file(input);
    }

    #[test]
    fn remux_args_prepare_browser_safe_video_without_reencoding() {
        let input = PathBuf::from("source.ts");
        let output = PathBuf::from("prepared.mp4");
        let args = remux_args(&input, &output, "video");

        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-c" && pair[1] == "copy")
        );
        assert!(args.iter().any(|arg| arg == "-dn"));
        assert!(args.iter().any(|arg| arg == "-sn"));
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-f" && pair[1] == "mp4")
        );
        assert_eq!(args.last().map(String::as_str), Some("prepared.mp4"));
    }

    #[test]
    fn transcode_args_bound_cpu_for_browser_safe_video() {
        let input = PathBuf::from("source.mkv");
        let output = PathBuf::from("prepared.mp4");
        let args = transcode_args(&input, &output, "video");

        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-c:v" && pair[1] == "libx264")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-pix_fmt" && pair[1] == "yuv420p")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-c:a" && pair[1] == "aac")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-movflags" && pair[1] == "+faststart")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-threads" && pair[1] == "2")
        );
        assert!(args.iter().any(|arg| arg == "-nostdin"));
        assert_eq!(args.last().map(String::as_str), Some("prepared.mp4"));
    }
}
