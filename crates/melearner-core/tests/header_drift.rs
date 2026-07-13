use std::fs;
use std::path::PathBuf;

#[test]
fn checked_header_matches_the_rust_abi() {
    let generated = PathBuf::from(env!("MELEARNER_CORE_GENERATED_HEADER"));
    let checked = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join("include/melearner_core.h");

    assert_eq!(
        fs::read_to_string(&checked).expect("read checked header"),
        fs::read_to_string(&generated).expect("read generated header"),
        "include/melearner_core.h is stale; regenerate it from crates/melearner-core"
    );
}
