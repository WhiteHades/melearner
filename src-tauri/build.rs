use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let out_dir = manifest_dir.join("..").join("out");

    println!("cargo:rerun-if-changed={}", out_dir.display());
    println!("cargo:rerun-if-changed=build.rs");

    if !has_built_frontend(&out_dir) {
        build_frontend(&manifest_dir.join(".."));
    }

    emit_build_info(&manifest_dir);

    tauri_build::build()
}

fn has_built_frontend(out_dir: &PathBuf) -> bool {
    if !out_dir.exists() {
        return false;
    }
    let entries = match std::fs::read_dir(out_dir) {
        Ok(entries) => entries,
        Err(_) => return false,
    };
    entries.count() > 0
}

fn build_frontend(repo_root: &PathBuf) {
    eprintln!("[melearner build.rs] out/ missing or empty; running pnpm build");

    let candidates: [(&str, &[&str]); 4] = [
        ("pnpm", &["build"]),
        ("npm", &["run", "build"]),
        ("bun", &["run", "build"]),
        ("yarn", &["build"]),
    ];

    for (cmd, args) in candidates {
        let status = std::process::Command::new(cmd)
            .args(args)
            .current_dir(repo_root)
            .status();
        match status {
            Ok(s) if s.success() => return,
            Ok(_) => continue,
            Err(_) => continue,
        }
    }

    panic!(
        "[melearner build.rs] could not build frontend. install pnpm, npm, bun, or yarn, \
         or run `pnpm build` manually before `cargo build`."
    );
}

fn emit_build_info(manifest_dir: &PathBuf) {
    let git_sha = run_capture("git", &["rev-parse", "--short=12", "HEAD"], manifest_dir)
        .unwrap_or_else(|| "unknown".to_string());
    let git_sha_long = run_capture("git", &["rev-parse", "HEAD"], manifest_dir)
        .unwrap_or_else(|| "unknown".to_string());
    let build_ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs().to_string())
        .unwrap_or_else(|_| "0".to_string());

    println!("cargo:rustc-env=MELEARNER_GIT_SHA={git_sha}");
    println!("cargo:rustc-env=MELEARNER_GIT_SHA_LONG={git_sha_long}");
    println!("cargo:rustc-env=MELEARNER_BUILD_TIMESTAMP={build_ts}");

    let git_dir = manifest_dir.join("..").join(".git");
    println!("cargo:rerun-if-changed={}", git_dir.join("HEAD").display());
    println!(
        "cargo:rerun-if-changed={}",
        git_dir.join("refs/heads/main").display()
    );
}

fn run_capture(cmd: &str, args: &[&str], cwd: &PathBuf) -> Option<String> {
    let out = std::process::Command::new(cmd)
        .args(args)
        .current_dir(cwd)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8(out.stdout).ok()?;
    let trimmed = s.trim();
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}
