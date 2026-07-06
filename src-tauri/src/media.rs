use seahash::SeaHasher;
use serde::Serialize;
use std::hash::Hasher;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::time::UNIX_EPOCH;

#[derive(Debug, Serialize)]
pub struct GeneratedThumbnail {
    path: String,
}

static THUMBNAIL_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
const THUMBNAIL_CACHE_VERSION: &[u8] = b"course-thumbnail-v1";

fn thumbnail_lock() -> &'static Mutex<()> {
    THUMBNAIL_LOCK.get_or_init(|| Mutex::new(()))
}

fn new_hasher() -> SeaHasher {
    SeaHasher::with_seeds(
        0x7B10D9E02B5C7A41,
        0x8E4C6F2A19D3B507,
        0xC3A5F681D924E0BB,
        0x4F6E8A1C35D7920E,
    )
}

fn thumbnail_cache_root() -> PathBuf {
    if let Ok(path) = std::env::var("MELEARNER_THUMBNAIL_CACHE") {
        return PathBuf::from(path);
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(path) = std::env::var("LOCALAPPDATA") {
            return PathBuf::from(path).join("melearner").join("thumbnails");
        }
        if let Ok(path) = std::env::var("APPDATA") {
            return PathBuf::from(path).join("melearner").join("thumbnails");
        }
    }

    #[cfg(target_os = "macos")]
    {
        if let Ok(home) = std::env::var("HOME") {
            return PathBuf::from(home)
                .join("Library")
                .join("Caches")
                .join("melearner")
                .join("thumbnails");
        }
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    {
        if let Ok(path) = std::env::var("XDG_CACHE_HOME") {
            return PathBuf::from(path).join("melearner").join("thumbnails");
        }
        if let Ok(home) = std::env::var("HOME") {
            return PathBuf::from(home)
                .join(".cache")
                .join("melearner")
                .join("thumbnails");
        }
    }

    std::env::temp_dir().join("melearner-thumbnails")
}

fn thumbnail_cache_key(path: &Path, metadata: &std::fs::Metadata) -> String {
    let mut hasher = new_hasher();
    hasher.write(path.to_string_lossy().as_bytes());
    hasher.write(THUMBNAIL_CACHE_VERSION);
    hasher.write(&metadata.len().to_le_bytes());

    if let Ok(modified) = metadata.modified() {
        if let Ok(duration) = modified.duration_since(UNIX_EPOCH) {
            hasher.write(&duration.as_secs().to_le_bytes());
            hasher.write(&duration.subsec_nanos().to_le_bytes());
        }
    }

    format!("{:016x}", hasher.finish())
}

fn thumbnail_output_path(input: &Path) -> Result<PathBuf, String> {
    let metadata =
        std::fs::metadata(input).map_err(|err| format!("failed to read video metadata: {err}"))?;
    Ok(thumbnail_cache_root().join(format!("{}.jpg", thumbnail_cache_key(input, &metadata))))
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

fn thumbnail_temp_output_path(output: &Path) -> PathBuf {
    let file_name = output
        .file_name()
        .map(|name| name.to_string_lossy().to_string())
        .unwrap_or_else(|| "course-thumbnail.jpg".to_string());
    output.with_file_name(format!("{file_name}.part.jpg"))
}

fn existing_thumbnail_ready(output: &Path) -> bool {
    output
        .metadata()
        .map(|metadata| metadata.len() > 0)
        .unwrap_or(false)
}

fn thumbnail_args(input: &Path, output: &Path, timestamp: &str) -> Vec<String> {
    vec![
        "-hide_banner".to_string(),
        "-loglevel".to_string(),
        "error".to_string(),
        "-nostdin".to_string(),
        "-y".to_string(),
        "-ss".to_string(),
        timestamp.to_string(),
        "-i".to_string(),
        input.to_string_lossy().to_string(),
        "-frames:v".to_string(),
        "1".to_string(),
        "-vf".to_string(),
        "scale='min(640,iw)':-2".to_string(),
        "-q:v".to_string(),
        "4".to_string(),
        "-pix_fmt".to_string(),
        "yuvj420p".to_string(),
        "-f".to_string(),
        "image2".to_string(),
        output.to_string_lossy().to_string(),
    ]
}

fn run_thumbnail_ffmpeg(input: &Path, output: &Path) -> Result<(), String> {
    let attempts = ["00:00:03", "00:00:00.5", "00:00:00"];
    let mut last_error = "ffmpeg did not produce a thumbnail".to_string();

    for timestamp in attempts {
        let _ = std::fs::remove_file(output);
        let ffmpeg_output = Command::new(ffmpeg_program())
            .args(thumbnail_args(input, output, timestamp))
            .stdin(Stdio::null())
            .output()
            .map_err(|err| format!("failed to start ffmpeg: {err}"))?;

        if ffmpeg_output.status.success() && existing_thumbnail_ready(output) {
            return Ok(());
        }

        let stderr = String::from_utf8_lossy(&ffmpeg_output.stderr)
            .trim()
            .to_string();
        last_error = if stderr.is_empty() {
            ffmpeg_output.status.to_string()
        } else {
            format!("{}: {stderr}", ffmpeg_output.status)
        };
    }

    Err(last_error)
}

#[tauri::command]
pub async fn generate_video_thumbnail(path: String) -> Result<GeneratedThumbnail, String> {
    tokio::task::spawn_blocking(move || {
        let input = PathBuf::from(path);
        if !input.exists() {
            return Err("video file does not exist".to_string());
        }

        let output = thumbnail_output_path(&input)?;
        if existing_thumbnail_ready(&output) {
            return Ok(GeneratedThumbnail {
                path: output.to_string_lossy().to_string(),
            });
        }

        if let Some(parent) = output.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|err| format!("failed to create thumbnail cache: {err}"))?;
        }

        let _guard = thumbnail_lock()
            .lock()
            .map_err(|_| "thumbnail generator lock is poisoned".to_string())?;

        if existing_thumbnail_ready(&output) {
            return Ok(GeneratedThumbnail {
                path: output.to_string_lossy().to_string(),
            });
        }

        let temp_output = thumbnail_temp_output_path(&output);
        let _ = std::fs::remove_file(&temp_output);

        if let Err(details) = run_thumbnail_ffmpeg(&input, &temp_output) {
            let _ = std::fs::remove_file(&temp_output);
            return Err(format!("ffmpeg could not generate a thumbnail: {details}"));
        }

        if output.exists() {
            let _ = std::fs::remove_file(&output);
        }

        std::fs::rename(&temp_output, &output)
            .map_err(|err| format!("failed to move thumbnail into cache: {err}"))?;

        Ok(GeneratedThumbnail {
            path: output.to_string_lossy().to_string(),
        })
    })
    .await
    .map_err(|err| format!("thumbnail generation task failed: {err}"))?
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

    fn temp_path(name: &str, extension: &str) -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time before unix epoch")
            .as_nanos();
        std::env::temp_dir().join(format!("melearner-media-{name}-{suffix}.{extension}"))
    }

    #[test]
    fn thumbnail_output_path_uses_jpg_extension() {
        let input = temp_file("thumbnail-output-path");

        let thumbnail = thumbnail_output_path(&input).expect("thumbnail output path");

        assert_eq!(
            thumbnail.extension().and_then(|ext| ext.to_str()),
            Some("jpg")
        );

        let _ = fs::remove_file(input);
    }

    #[test]
    fn thumbnail_args_extract_scaled_jpeg_frame() {
        let input = PathBuf::from("lecture.mp4");
        let output = PathBuf::from("thumbnail.jpg");
        let args = thumbnail_args(&input, &output, "00:00:03");

        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-ss" && pair[1] == "00:00:03")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-frames:v" && pair[1] == "1")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-vf" && pair[1] == "scale='min(640,iw)':-2")
        );
        assert!(
            args.windows(2)
                .any(|pair| pair[0] == "-f" && pair[1] == "image2")
        );
        assert!(args.iter().any(|arg| arg == "-nostdin"));
        assert_eq!(args.last().map(String::as_str), Some("thumbnail.jpg"));
    }

    #[test]
    fn thumbnail_ffmpeg_generates_jpeg_when_ffmpeg_is_available() {
        let source = temp_path("thumbnail-source", "mp4");
        let output = temp_path("thumbnail-output", "jpg");

        let make_source = Command::new(ffmpeg_program())
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "testsrc=size=320x240:rate=1",
                "-t",
                "1",
                "-pix_fmt",
                "yuv420p",
            ])
            .arg(&source)
            .stdin(Stdio::null())
            .status();

        if !make_source.map(|status| status.success()).unwrap_or(false) {
            let _ = fs::remove_file(source);
            let _ = fs::remove_file(output);
            return;
        }

        run_thumbnail_ffmpeg(&source, &output).expect("generate thumbnail");
        assert!(existing_thumbnail_ready(&output));

        let _ = fs::remove_file(source);
        let _ = fs::remove_file(output);
    }
}
