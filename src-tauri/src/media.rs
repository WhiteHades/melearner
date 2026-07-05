use seahash::SeaHasher;
use serde::Serialize;
use std::hash::Hasher;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::UNIX_EPOCH;

#[derive(Debug, Serialize)]
pub struct PreparedMedia {
    path: String,
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

    for prefix in ["/usr/bin", "/usr/local/bin", "/opt/homebrew/bin", "/opt/local/bin"] {
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

fn ffmpeg_args(input: &Path, output: &Path, media_type: &str) -> Vec<String> {
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
        ]);
    } else {
        args.extend([
            "-map".to_string(),
            "0:v:0?".to_string(),
            "-map".to_string(),
            "0:a:0?".to_string(),
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
            "-movflags".to_string(),
            "+faststart".to_string(),
        ]);
    }

    args.push(output.to_string_lossy().to_string());
    args
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

        if output
            .metadata()
            .map(|metadata| metadata.len() > 0)
            .unwrap_or(false)
        {
            return Ok(PreparedMedia {
                path: output.to_string_lossy().to_string(),
            });
        }

        if let Some(parent) = output.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|err| format!("failed to create media cache: {err}"))?;
        }

        let ffmpeg_output = Command::new(ffmpeg_program())
            .args(ffmpeg_args(&input, &output, normalized_type))
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .output()
            .map_err(|err| format!("failed to start ffmpeg: {err}"))?;

        if !ffmpeg_output.status.success() {
            let _ = std::fs::remove_file(&output);
            let stderr = String::from_utf8_lossy(&ffmpeg_output.stderr)
                .trim()
                .to_string();
            let details = if stderr.is_empty() {
                ffmpeg_output.status.to_string()
            } else {
                format!("{}: {stderr}", ffmpeg_output.status)
            };
            return Err(format!(
                "ffmpeg could not prepare this media file: {details}"
            ));
        }

        Ok(PreparedMedia {
            path: output.to_string_lossy().to_string(),
        })
    })
    .await
    .map_err(|err| format!("media preparation task failed: {err}"))?
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
    fn ffmpeg_args_prepare_browser_safe_video() {
        let input = PathBuf::from("source.mkv");
        let output = PathBuf::from("prepared.mp4");
        let args = ffmpeg_args(&input, &output, "video");

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
        assert!(args.iter().any(|arg| arg == "-nostdin"));
        assert_eq!(args.last().map(String::as_str), Some("prepared.mp4"));
    }
}
