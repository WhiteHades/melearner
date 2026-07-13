use std::fs;

#[test]
fn checked_header_matches_the_rust_abi() {
    let crate_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let config = cbindgen::Config::from_file(crate_dir.join("cbindgen.toml"))
        .expect("valid cbindgen config");
    let bindings = cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("generate melearner_core.h");
    let mut generated = Vec::new();
    bindings.write(&mut generated);
    let checked = crate_dir.join("../..").join("include/melearner_core.h");

    assert_eq!(
        fs::read(&checked).expect("read checked header"),
        generated,
        "include/melearner_core.h is stale; regenerate it from crates/melearner-core"
    );
}
