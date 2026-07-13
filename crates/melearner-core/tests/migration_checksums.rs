use melearner_core::migrations::MIGRATIONS;
use sha2::{Digest, Sha384};

const SHIPPED: &[(i64, &str, &str)] = &[
    (
        1,
        "create_courses_table",
        "1fcc9396adcb95748aaa55a0cd8248fc07b2a350e054f86245eff1d392303e1c2468e73f1148270cfbe8d7bdf7fbe121",
    ),
    (
        2,
        "create_lessons_table",
        "e88e49f542c618cf33f7a11da0d0a1e58478c464ee012cd229a8e65660740d794d0e2f115dd9cdd77a548a894d58d436",
    ),
    (
        3,
        "create_notes_table",
        "ae02eae910f88c8ce07dfecbddc3b45566e435dfe3446797d1a7851109a01ad0ef4802ed7776a2be512face930d75d2f",
    ),
    (
        4,
        "create_bookmarks_table",
        "230493440739bac24a5933a95de9830a693160bafcdae7c83a740f8fad7f25c3fe26769a0b9cc1f348af3fe972430593",
    ),
    (
        5,
        "create_settings_table",
        "262282ea61499b7c811c401d4b86f831344791e2e53fe00bafdf551e993175359f84fa630e7f09f3f82da482bccc9063",
    ),
    (
        6,
        "create_indexes",
        "02176440df9fc089928a5a10a4d09f4967fabf14a764399716f423052562493c4c93e47e12c724f5178b8be44e14b6a0",
    ),
    (
        7,
        "drop_orphan_tables",
        "fc2e0b2d224eb4a2bf317b647aa89fae42a982922c8d0fe92546dc3cc42e4e3743f7fb53f1ac86639cbf65452da85f27",
    ),
    (
        8,
        "create_notes_lessons_indexes",
        "68b0cf35175df8e58c300077d23305f006b95a2c3c6e0d34798ca24fd99277a2d777581388ccd7040a383257956faa5b",
    ),
    (
        9,
        "noop_migration_9",
        "26e71cc37450b183fb5bb72ec4f644ed27de1b55fad3d4d6cfb0ca0d71f42ca990911d74649814105a190325e15d2092",
    ),
    (
        10,
        "noop_migration_10",
        "26e71cc37450b183fb5bb72ec4f644ed27de1b55fad3d4d6cfb0ca0d71f42ca990911d74649814105a190325e15d2092",
    ),
    (
        11,
        "create_sections_subtitles_settings",
        "3a09837148bb2f83e3dea028398621996d4e81e9ef667f054c654aa164f17fdbc83825e5c7ca9956b41d967c169c46ff",
    ),
    (
        12,
        "add_structured_lesson_metadata",
        "12014eeae510ce26aa4001251141e949489377b79fd3420481622fd6b3953218d2edd4c10137f55c7cf381001ec29789",
    ),
    (
        13,
        "create_structured_metadata_indexes",
        "cc3a9da465f390c2d6ac0c1a8b3ce462a3e7fea6cdfd42b012291e9750ed20a95d3d1b503d75607b732b84167674a6ce",
    ),
    (
        14,
        "backfill_sections_from_existing_lessons",
        "cac95a20e2bbb48820f9d56703be49c5b8307f62215f05ef97ee8d5301b86a01ea174b422494d52ee47579a8d57d8feb",
    ),
    (
        15,
        "add_durable_course_identity_fields",
        "cff0e88dde46c0c46f8abea11150a9b3619c1f84618ab169fede88df9823c73597d0675c0628e23924d3c09290696158",
    ),
    (
        16,
        "create_lesson_activity",
        "e4d105f4751c8a2b649f0d8e64ca11a64d78f6b200c1d93c26a3ffd637fb6d687a7c3ba9664bf2bc6edbf30add11a50a",
    ),
];

#[test]
fn shipped_sqlx_migration_checksums_do_not_change() {
    assert_eq!(MIGRATIONS.len(), SHIPPED.len());
    for (migration, &(version, description, checksum)) in MIGRATIONS.iter().zip(SHIPPED) {
        assert_eq!(migration.version, version);
        assert_eq!(migration.description, description);
        assert_eq!(format!("{:x}", Sha384::digest(migration.sql)), checksum);
    }
}
