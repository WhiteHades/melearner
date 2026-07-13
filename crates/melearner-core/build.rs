use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("crate directory"));
    let config_path = crate_dir.join("cbindgen.toml");
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("build output directory"))
        .join("melearner_core.h");
    let config = cbindgen::Config::from_file(&config_path).expect("valid cbindgen config");
    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("generate melearner_core.h")
        .write_to_file(&output);

    println!(
        "cargo:rustc-env=MELEARNER_CORE_GENERATED_HEADER={}",
        output.display()
    );
    println!("cargo:rerun-if-changed={}", crate_dir.join("src").display());
    println!("cargo:rerun-if-changed={}", config_path.display());
}
