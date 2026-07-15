use std::collections::{HashMap, HashSet};
use std::path::Path;

use cap_std::ambient_authority;
use cap_std::fs::Dir as CapabilityDir;
use serde::Serialize;
use sqlx::{Connection, QueryBuilder, Row, Sqlite, Transaction};

use super::{LibraryDatabase, LibraryError, child_path_range, natural_cmp};
use crate::scanner::{
    CapturedScan, CourseData, FileEntry, FileType, ScanError, ScanResult,
    ensure_course_marker_in_dir, scan_library_checked_in_root, verify_captured_root,
};
use crate::{MutationControl, next_library_revision};

const WRITE_BATCH_SIZE: usize = 500;
const WARNING_LIMIT: usize = 64;

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ReconcileResult {
    pub(crate) revision: u64,
    pub(crate) course_count: u64,
    pub(crate) warnings: Vec<String>,
}

struct PersistedCourse {
    id: String,
    identity_id: String,
    path: String,
    fingerprint: String,
}

struct PersistedSection {
    id: String,
    course_id: String,
    name: String,
}

struct PersistedLesson {
    id: String,
    course_id: String,
    section_name: String,
    name: String,
    path: String,
    relative_path: String,
    kind: String,
    file_size: i64,
}

struct ResolvedCourseSeed {
    id: String,
    identity_id: String,
    scanned: CourseData,
}

struct ResolvedCourse {
    id: String,
    identity_id: String,
    name: String,
    path: String,
    fingerprint: String,
    thumbnail_source_path: Option<String>,
}

struct ResolvedSection {
    id: String,
    course_id: String,
    name: String,
    order: i64,
}

struct PendingLesson {
    scanned_id: String,
    course_id: String,
    course_name: String,
    section_id: String,
    section_name: String,
    name: String,
    path: String,
    relative_path: String,
    kind: String,
    file_size: i64,
    order: i64,
    subtitles: Vec<PendingSubtitle>,
}

struct PendingSubtitle {
    path: String,
    language: String,
    label: String,
}

struct ResolvedLesson {
    id: String,
    course_id: String,
    section_id: String,
    name: String,
    path: String,
    relative_path: String,
    kind: String,
    file_size: i64,
    order: i64,
}

struct ResolvedSubtitle {
    id: String,
    lesson_id: String,
    path: String,
    language: String,
    label: String,
    order: i64,
}

struct MarkerWrite {
    course_name: String,
    path: String,
    identity_id: String,
}

struct TransactionPlan {
    course_count: u64,
    warnings: Vec<String>,
    marker_writes: Vec<MarkerWrite>,
}

struct ScannedStructure {
    courses: Vec<ResolvedCourse>,
    sections: Vec<ResolvedSection>,
    lessons: Vec<PendingLesson>,
}

struct LessonResolution {
    lessons: Vec<ResolvedLesson>,
    subtitles: Vec<ResolvedSubtitle>,
    claimed_ids: HashSet<String>,
}

struct LessonMatchState<'a> {
    matches: &'a mut [Option<usize>],
    claimed: &'a mut HashSet<usize>,
    blocked: &'a mut [bool],
}

impl LibraryDatabase {
    pub(crate) async fn scan_and_reconcile(
        &mut self,
        expected_revision: u64,
        root_path: &str,
        max_payload_bytes: usize,
        control: &MutationControl,
    ) -> Result<ReconcileResult, LibraryError> {
        self.require_revision(expected_revision)?;
        require_active(control)?;
        let root = std::fs::canonicalize(root_path).map_err(|error| {
            LibraryError::InvalidScan(format!("cannot resolve directory {root_path}: {error}"))
        })?;
        let canonical_root = root
            .to_str()
            .ok_or_else(|| {
                LibraryError::InvalidScan(format!(
                    "library root is not valid UTF-8: {}",
                    root.display()
                ))
            })?
            .to_string();
        let marker_root =
            CapabilityDir::open_ambient_dir(&root, ambient_authority()).map_err(|error| {
                LibraryError::InvalidScan(format!(
                    "cannot open library root {}: {error}",
                    root.display()
                ))
            })?;
        let captured_scan = scan_library_checked_in_root(&root, &marker_root, control)
            .map_err(library_scan_error)?;
        require_active(control)?;
        self.reconcile_scan(
            expected_revision,
            &canonical_root,
            captured_scan,
            max_payload_bytes,
            control,
            &marker_root,
        )
        .await
    }

    async fn reconcile_scan(
        &mut self,
        expected_revision: u64,
        root_path: &str,
        captured_scan: CapturedScan,
        max_payload_bytes: usize,
        control: &MutationControl,
        marker_root: &CapabilityDir,
    ) -> Result<ReconcileResult, LibraryError> {
        self.require_revision(expected_revision)?;
        require_active(control)?;
        let CapturedScan {
            result: scan,
            mut course_dirs,
        } = captured_scan;

        let mut transaction = self.connection.begin_with("BEGIN IMMEDIATE").await?;
        let mut plan = match reconcile_transaction(&mut transaction, root_path, scan, control).await
        {
            Ok(plan) => plan,
            Err(error) => return Err(rollback_error(transaction, error).await),
        };
        let marker_dirs = match plan
            .marker_writes
            .iter()
            .map(|marker| {
                course_dirs.remove(Path::new(&marker.path)).ok_or_else(|| {
                    LibraryError::InvalidScan(format!(
                        "scan did not retain course folder: {}",
                        marker.path
                    ))
                })
            })
            .collect::<Result<Vec<_>, LibraryError>>()
        {
            Ok(marker_dirs) => marker_dirs,
            Err(error) => {
                return Err(rollback_error(transaction, error).await);
            }
        };

        if control.is_cancelled() || !control.begin_commit() {
            return Err(rollback_error(transaction, LibraryError::Cancelled).await);
        }
        let Some(next_revision) = next_library_revision() else {
            return Err(rollback_error(transaction, LibraryError::RevisionExhausted).await);
        };
        if !fit_response(
            next_revision.get(),
            plan.course_count,
            &mut plan.warnings,
            max_payload_bytes,
        ) {
            return Err(rollback_error(
                transaction,
                LibraryError::ResponseTooLarge {
                    limit: max_payload_bytes,
                },
            )
            .await);
        }
        if let Err(error) = verify_captured_root(Path::new(root_path), marker_root) {
            return Err(rollback_error(transaction, library_scan_error(error)).await);
        }
        transaction.commit().await?;

        self.revision = next_revision.get();
        self.library_path = Some(root_path.to_string());
        self.lesson_order_indexes.clear();
        #[cfg(test)]
        {
            self.lesson_order_index_builds = 0;
        }

        let mut warnings = plan.warnings;
        for (marker, marker_dir) in plan.marker_writes.into_iter().zip(marker_dirs) {
            if let Err(error) = ensure_course_marker_in_dir(
                &marker_dir,
                Path::new(&marker.path),
                marker.identity_id.as_str(),
            ) {
                push_warning(
                    &mut warnings,
                    format!(
                        "Could not write marker for \"{}\": {error}",
                        marker.course_name
                    ),
                );
            }
        }
        let response_fits = fit_response(
            self.revision,
            plan.course_count,
            &mut warnings,
            max_payload_bytes,
        );
        debug_assert!(
            response_fits,
            "warning-free scan response fit before commit"
        );

        Ok(ReconcileResult {
            revision: self.revision,
            course_count: plan.course_count,
            warnings,
        })
    }

    fn require_revision(&self, expected_revision: u64) -> Result<(), LibraryError> {
        if expected_revision == self.revision {
            Ok(())
        } else {
            Err(LibraryError::StaleRevision {
                expected: expected_revision,
                actual: self.revision,
            })
        }
    }
}

fn library_scan_error(error: ScanError) -> LibraryError {
    match error {
        ScanError::Cancelled => LibraryError::Cancelled,
        ScanError::Invalid(message) => LibraryError::InvalidScan(message),
    }
}

fn fit_response(
    revision: u64,
    course_count: u64,
    warnings: &mut Vec<String>,
    max_payload_bytes: usize,
) -> bool {
    loop {
        let response = ReconcileResult {
            revision,
            course_count,
            warnings: warnings.clone(),
        };
        if serde_json::to_vec(&response).is_ok_and(|payload| payload.len() <= max_payload_bytes) {
            return true;
        }
        if warnings.pop().is_none() {
            return false;
        }
    }
}

async fn reconcile_transaction(
    transaction: &mut Transaction<'_, Sqlite>,
    root_path: &str,
    scan: ScanResult,
    control: &MutationControl,
) -> Result<TransactionPlan, LibraryError> {
    require_active(control)?;
    let scan_stamp =
        sqlx::query_scalar::<_, String>("SELECT strftime('%Y-%m-%dT%H:%M:%fZ', 'now')")
            .fetch_one(&mut **transaction)
            .await?;
    let persisted_courses = load_persisted_courses(transaction).await?;
    let mut warnings = scan.warnings.into_vec();
    warnings.truncate(WARNING_LIMIT);
    let course_seeds = resolve_courses(scan.courses.into_vec(), &persisted_courses, &mut warnings)?;
    let course_ids = course_seeds
        .iter()
        .map(|course| course.id.clone())
        .collect::<Vec<_>>();
    let persisted_section_ids = load_persisted_ids(transaction, "sections").await?;
    let persisted_lesson_ids = load_persisted_ids(transaction, "lessons").await?;
    let persisted_lesson_paths = load_persisted_lesson_paths(transaction).await?;
    let persisted_sections = load_persisted_sections(transaction, &course_ids).await?;
    let persisted_lessons = load_persisted_lessons(transaction, &course_ids).await?;
    let ScannedStructure {
        mut courses,
        sections,
        lessons: pending_lessons,
    } = build_scanned_structure(course_seeds, &persisted_sections, &persisted_section_ids)?;
    let LessonResolution {
        lessons,
        subtitles,
        claimed_ids: claimed_lesson_ids,
    } = resolve_lessons(
        pending_lessons,
        &persisted_lessons,
        &persisted_lesson_ids,
        &persisted_lesson_paths,
        &mut warnings,
    )?;

    for course in &mut courses {
        course.thumbnail_source_path = lessons
            .iter()
            .find(|lesson| lesson.course_id == course.id && lesson.kind == "video")
            .map(|lesson| lesson.path.clone());
    }

    require_active(control)?;
    mark_root_courses_missing(transaction, root_path, &scan_stamp).await?;
    write_courses(transaction, &courses, &scan_stamp, control).await?;

    let stale_lesson_ids = persisted_lessons
        .iter()
        .filter(|lesson| !claimed_lesson_ids.contains(&lesson.id))
        .map(|lesson| lesson.id.clone())
        .collect::<Vec<_>>();
    delete_rows_by_id(transaction, "lessons", &stale_lesson_ids, control).await?;

    write_sections(transaction, &sections, &scan_stamp, control).await?;
    write_lessons(transaction, &lessons, &scan_stamp, control).await?;

    let resolved_lesson_ids = lessons
        .iter()
        .map(|lesson| lesson.id.clone())
        .collect::<Vec<_>>();
    delete_subtitles(transaction, &resolved_lesson_ids, control).await?;
    write_subtitles(transaction, &subtitles, control).await?;

    let retained_section_ids = sections
        .iter()
        .map(|section| section.id.as_str())
        .collect::<HashSet<_>>();
    let stale_section_ids = persisted_sections
        .iter()
        .filter(|section| !retained_section_ids.contains(section.id.as_str()))
        .map(|section| section.id.clone())
        .collect::<Vec<_>>();
    delete_rows_by_id(transaction, "sections", &stale_section_ids, control).await?;
    write_library_path(transaction, root_path, &scan_stamp).await?;
    require_active(control)?;

    let marker_writes = courses
        .iter()
        .map(|course| {
            Path::new(&course.path)
                .strip_prefix(root_path)
                .map_err(|_| {
                    LibraryError::InvalidScan(format!(
                        "scanned course escaped library root: {}",
                        course.path
                    ))
                })?;
            Ok(MarkerWrite {
                course_name: course.name.clone(),
                path: course.path.clone(),
                identity_id: course.identity_id.clone(),
            })
        })
        .collect::<Result<Vec<_>, LibraryError>>()?;
    let course_count = u64::try_from(courses.len())
        .map_err(|_| LibraryError::InvalidScan("course count exceeds u64".to_string()))?;
    Ok(TransactionPlan {
        course_count,
        warnings,
        marker_writes,
    })
}

async fn rollback_error(transaction: Transaction<'_, Sqlite>, error: LibraryError) -> LibraryError {
    match transaction.rollback().await {
        Ok(()) => error,
        Err(rollback_error) => LibraryError::from(rollback_error),
    }
}

fn require_active(control: &MutationControl) -> Result<(), LibraryError> {
    if control.is_cancelled() {
        Err(LibraryError::Cancelled)
    } else {
        Ok(())
    }
}

fn push_warning(warnings: &mut Vec<String>, warning: String) {
    if warnings.len() < WARNING_LIMIT {
        warnings.push(warning);
    }
}

fn allocate_unique_id<F>(base: &str, is_unavailable: F) -> String
where
    F: Fn(&str) -> bool,
{
    if !is_unavailable(base) {
        return base.to_string();
    }
    let mut suffix = 1_u64;
    loop {
        let candidate = format!("{base}:{suffix}");
        if !is_unavailable(&candidate) {
            return candidate;
        }
        suffix += 1;
    }
}

async fn load_persisted_courses(
    transaction: &mut Transaction<'_, Sqlite>,
) -> Result<Vec<PersistedCourse>, LibraryError> {
    sqlx::query("SELECT id, identity_id, path, fingerprint FROM courses")
        .fetch_all(&mut **transaction)
        .await?
        .into_iter()
        .map(|row| {
            Ok(PersistedCourse {
                id: row.try_get("id")?,
                identity_id: row.try_get("identity_id")?,
                path: row.try_get("path")?,
                fingerprint: row.try_get("fingerprint")?,
            })
        })
        .collect()
}

async fn load_persisted_ids(
    transaction: &mut Transaction<'_, Sqlite>,
    table: &str,
) -> Result<HashSet<String>, LibraryError> {
    debug_assert!(matches!(table, "sections" | "lessons"));
    let rows = sqlx::query(&format!("SELECT id FROM {table}"))
        .fetch_all(&mut **transaction)
        .await?;
    rows.into_iter()
        .map(|row| row.try_get("id").map_err(LibraryError::from))
        .collect()
}

async fn load_persisted_sections(
    transaction: &mut Transaction<'_, Sqlite>,
    course_ids: &[String],
) -> Result<Vec<PersistedSection>, LibraryError> {
    let mut sections = Vec::new();
    for course_ids in course_ids.chunks(WRITE_BATCH_SIZE) {
        let mut query = QueryBuilder::<Sqlite>::new(
            "SELECT id, course_id, name FROM sections WHERE course_id IN (",
        );
        let mut separated = query.separated(", ");
        for course_id in course_ids {
            separated.push_bind(course_id);
        }
        separated.push_unseparated(")");
        for row in query.build().fetch_all(&mut **transaction).await? {
            sections.push(PersistedSection {
                id: row.try_get("id")?,
                course_id: row.try_get("course_id")?,
                name: row.try_get("name")?,
            });
        }
    }
    Ok(sections)
}

async fn load_persisted_lessons(
    transaction: &mut Transaction<'_, Sqlite>,
    course_ids: &[String],
) -> Result<Vec<PersistedLesson>, LibraryError> {
    let mut lessons = Vec::new();
    for course_ids in course_ids.chunks(WRITE_BATCH_SIZE) {
        let mut query = QueryBuilder::<Sqlite>::new(
            "SELECT lessons.id, lessons.course_id, sections.name AS section_name,
                    lessons.name, lessons.path, lessons.relative_path, lessons.type,
                    lessons.file_size
             FROM lessons
             INNER JOIN sections ON sections.id = lessons.section_id
                                AND sections.course_id = lessons.course_id
             WHERE lessons.course_id IN (",
        );
        let mut separated = query.separated(", ");
        for course_id in course_ids {
            separated.push_bind(course_id);
        }
        separated.push_unseparated(")");
        for row in query.build().fetch_all(&mut **transaction).await? {
            lessons.push(PersistedLesson {
                id: row.try_get("id")?,
                course_id: row.try_get("course_id")?,
                section_name: row.try_get("section_name")?,
                name: row.try_get("name")?,
                path: row.try_get("path")?,
                relative_path: row.try_get("relative_path")?,
                kind: row.try_get("type")?,
                file_size: row.try_get("file_size")?,
            });
        }
    }
    Ok(lessons)
}

async fn load_persisted_lesson_paths(
    transaction: &mut Transaction<'_, Sqlite>,
) -> Result<HashMap<String, String>, LibraryError> {
    let rows = sqlx::query("SELECT path, course_id FROM lessons")
        .fetch_all(&mut **transaction)
        .await?;
    rows.into_iter()
        .map(|row| Ok((row.try_get("path")?, row.try_get("course_id")?)))
        .collect::<Result<_, sqlx::Error>>()
        .map_err(LibraryError::from)
}

fn resolve_courses(
    scanned: Vec<CourseData>,
    persisted: &[PersistedCourse],
    warnings: &mut Vec<String>,
) -> Result<Vec<ResolvedCourseSeed>, LibraryError> {
    let mut scanned_paths = HashSet::with_capacity(scanned.len());
    let mut scanned_ids = HashSet::with_capacity(scanned.len());
    for course in &scanned {
        if !scanned_paths.insert(course.path.as_ref()) {
            return Err(LibraryError::InvalidScan(format!(
                "duplicate scanned course path: {}",
                course.path
            )));
        }
        if !scanned_ids.insert(course.id.as_ref()) {
            return Err(LibraryError::InvalidScan(format!(
                "duplicate scanned course id: {}",
                course.id
            )));
        }
    }

    let by_path = persisted
        .iter()
        .enumerate()
        .map(|(index, course)| (course.path.as_str(), index))
        .collect::<HashMap<_, _>>();
    let by_identity = persisted
        .iter()
        .enumerate()
        .map(|(index, course)| (course.identity_id.as_str(), index))
        .collect::<HashMap<_, _>>();
    let persisted_ids = persisted
        .iter()
        .map(|course| course.id.clone())
        .collect::<HashSet<_>>();
    let mut by_fingerprint: HashMap<&str, Vec<usize>> = HashMap::new();
    for (index, course) in persisted.iter().enumerate() {
        by_fingerprint
            .entry(course.fingerprint.as_str())
            .or_default()
            .push(index);
    }
    let mut marker_counts: HashMap<&str, usize> = HashMap::new();
    for course in &scanned {
        if let Some(identity) = course.marker_identity_id.as_deref() {
            *marker_counts.entry(identity).or_default() += 1;
        }
    }

    let mut matches = vec![None; scanned.len()];
    let mut claimed = HashSet::new();
    let mut marker_is_usable = vec![false; scanned.len()];
    let mut fingerprint_blocked = vec![false; scanned.len()];

    for (index, course) in scanned.iter().enumerate() {
        if let Some(&persisted_index) = by_path.get(course.path.as_ref()) {
            matches[index] = Some(persisted_index);
            claimed.insert(persisted_index);
        }
    }

    for (index, course) in scanned.iter().enumerate() {
        if matches[index].is_some() {
            continue;
        }
        let Some(marker_identity) = course.marker_identity_id.as_deref() else {
            continue;
        };
        if marker_counts
            .get(marker_identity)
            .copied()
            .unwrap_or_default()
            != 1
        {
            push_warning(
                warnings,
                format!(
                    "Skipped marker identity for \"{}\": the same marker identity appears in multiple scanned courses.",
                    course.name
                ),
            );
            continue;
        }
        let Some(&persisted_index) = by_identity.get(marker_identity) else {
            marker_is_usable[index] = true;
            fingerprint_blocked[index] = true;
            continue;
        };
        if claimed.insert(persisted_index) {
            matches[index] = Some(persisted_index);
            marker_is_usable[index] = true;
        } else {
            fingerprint_blocked[index] = true;
            push_warning(
                warnings,
                format!(
                    "Skipped marker identity for \"{}\": that identity was already claimed by another scanned course.",
                    course.name
                ),
            );
        }
    }

    let mut scanned_by_fingerprint: HashMap<&str, Vec<usize>> = HashMap::new();
    for (index, course) in scanned.iter().enumerate() {
        if matches[index].is_none() && !fingerprint_blocked[index] {
            scanned_by_fingerprint
                .entry(course.fingerprint.as_ref())
                .or_default()
                .push(index);
        }
    }
    for (fingerprint, scanned_indexes) in scanned_by_fingerprint {
        let available = by_fingerprint
            .get(fingerprint)
            .into_iter()
            .flatten()
            .copied()
            .filter(|index| !claimed.contains(index))
            .collect::<Vec<_>>();
        if scanned_indexes.len() == 1 && available.len() == 1 {
            let scanned_index = scanned_indexes[0];
            let persisted_index = available[0];
            matches[scanned_index] = Some(persisted_index);
            claimed.insert(persisted_index);
        } else if by_fingerprint.contains_key(fingerprint) {
            for scanned_index in scanned_indexes {
                push_warning(
                    warnings,
                    format!(
                        "Skipped progress reuse for \"{}\": the course fingerprint is ambiguous.",
                        scanned[scanned_index].name
                    ),
                );
            }
        }
    }

    let mut resolved = Vec::with_capacity(scanned.len());
    let mut resolved_ids = HashSet::with_capacity(scanned.len());
    let mut resolved_identities = HashSet::with_capacity(scanned.len());
    for (index, course) in scanned.into_iter().enumerate() {
        let (id, identity_id) = if let Some(persisted_index) = matches[index] {
            let persisted = &persisted[persisted_index];
            (persisted.id.clone(), persisted.identity_id.clone())
        } else {
            let has_explicit_identity = marker_is_usable[index];
            let id = allocate_unique_id(course.id.as_ref(), |candidate| {
                persisted_ids.contains(candidate)
                    || resolved_ids.contains(candidate)
                    || (!has_explicit_identity
                        && (by_identity.contains_key(candidate)
                            || resolved_identities.contains(candidate)))
            });
            let identity_id = if has_explicit_identity {
                course
                    .marker_identity_id
                    .as_deref()
                    .unwrap_or(id.as_str())
                    .to_string()
            } else {
                id.clone()
            };
            if by_identity.contains_key(identity_id.as_str()) {
                return Err(LibraryError::InvalidScan(format!(
                    "new scanned course identity collides with stored course: {identity_id}"
                )));
            }
            (id, identity_id)
        };
        if !resolved_ids.insert(id.clone()) {
            return Err(LibraryError::InvalidScan(format!(
                "duplicate resolved course id: {id}"
            )));
        }
        if !resolved_identities.insert(identity_id.clone()) {
            return Err(LibraryError::InvalidScan(format!(
                "duplicate resolved course identity: {identity_id}"
            )));
        }
        resolved.push(ResolvedCourseSeed {
            id,
            identity_id,
            scanned: course,
        });
    }
    Ok(resolved)
}

fn build_scanned_structure(
    course_seeds: Vec<ResolvedCourseSeed>,
    persisted_sections: &[PersistedSection],
    persisted_section_ids: &HashSet<String>,
) -> Result<ScannedStructure, LibraryError> {
    let section_by_name = persisted_sections
        .iter()
        .map(|section| {
            (
                (section.course_id.as_str(), section.name.as_str()),
                section.id.as_str(),
            )
        })
        .collect::<HashMap<_, _>>();
    let mut courses = Vec::with_capacity(course_seeds.len());
    let mut sections = Vec::new();
    let mut lessons = Vec::new();
    let mut scanned_lesson_paths = HashSet::new();
    let mut scanned_lesson_ids = HashSet::new();
    let mut resolved_section_ids = HashSet::new();

    for seed in course_seeds {
        let mut scanned_sections = seed.scanned.sections.iter().collect::<Vec<_>>();
        scanned_sections.sort_by(|left, right| {
            left.order
                .cmp(&right.order)
                .then_with(|| natural_cmp(&left.name, &right.name))
                .then_with(|| left.id.cmp(&right.id))
        });
        let mut section_names = HashSet::with_capacity(scanned_sections.len());
        for scanned_section in scanned_sections {
            if !section_names.insert(scanned_section.name.as_ref()) {
                return Err(LibraryError::InvalidScan(format!(
                    "duplicate section name in course {}: {}",
                    seed.scanned.name, scanned_section.name
                )));
            }
            let section_id = match section_by_name
                .get(&(seed.id.as_str(), scanned_section.name.as_ref()))
                .copied()
            {
                Some(section_id) => section_id.to_string(),
                None => allocate_unique_id(scanned_section.id.as_ref(), |candidate| {
                    persisted_section_ids.contains(candidate)
                        || resolved_section_ids.contains(candidate)
                }),
            };
            if !resolved_section_ids.insert(section_id.clone()) {
                return Err(LibraryError::InvalidScan(format!(
                    "duplicate resolved section id: {section_id}"
                )));
            }
            let section_order = i64::try_from(scanned_section.order).map_err(|_| {
                LibraryError::InvalidScan(format!(
                    "section order exceeds SQLite integer range: {}",
                    scanned_section.name
                ))
            })?;
            sections.push(ResolvedSection {
                id: section_id.clone(),
                course_id: seed.id.clone(),
                name: scanned_section.name.to_string(),
                order: section_order,
            });

            let mut learning_files = scanned_section
                .files
                .iter()
                .filter(|file| file.file_type != FileType::Subtitle)
                .collect::<Vec<_>>();
            learning_files.sort_by(|left, right| {
                natural_cmp(&left.name, &right.name).then_with(|| left.id.cmp(&right.id))
            });
            for (order, file) in learning_files.into_iter().enumerate() {
                if !scanned_lesson_paths.insert(file.path.to_string()) {
                    return Err(LibraryError::InvalidScan(format!(
                        "duplicate scanned lesson path: {}",
                        file.path
                    )));
                }
                if !scanned_lesson_ids.insert(file.id.to_string()) {
                    return Err(LibraryError::InvalidScan(format!(
                        "duplicate scanned lesson id: {}",
                        file.id
                    )));
                }
                let order = i64::try_from(order).map_err(|_| {
                    LibraryError::InvalidScan(
                        "lesson order exceeds SQLite integer range".to_string(),
                    )
                })?;
                let file_size = i64::try_from(file.size).map_err(|_| {
                    LibraryError::InvalidScan(format!(
                        "lesson file is too large for SQLite: {}",
                        file.path
                    ))
                })?;
                let kind = lesson_kind(file.file_type, &file.path)?.to_string();
                let name = file_stem(&file.name);
                let subtitles = subtitles_for_file(&scanned_section.files, file);
                lessons.push(PendingLesson {
                    scanned_id: file.id.to_string(),
                    course_id: seed.id.clone(),
                    course_name: seed.scanned.name.to_string(),
                    section_id: section_id.clone(),
                    section_name: scanned_section.name.to_string(),
                    name,
                    path: file.path.to_string(),
                    relative_path: file.relative_path.to_string(),
                    kind,
                    file_size,
                    order,
                    subtitles,
                });
            }
        }

        courses.push(ResolvedCourse {
            id: seed.id,
            identity_id: seed.identity_id,
            name: seed.scanned.name.to_string(),
            path: seed.scanned.path.to_string(),
            fingerprint: seed.scanned.fingerprint.to_string(),
            thumbnail_source_path: None,
        });
    }

    Ok(ScannedStructure {
        courses,
        sections,
        lessons,
    })
}

fn lesson_kind(file_type: FileType, path: &str) -> Result<&'static str, LibraryError> {
    match file_type {
        FileType::Video => Ok("video"),
        FileType::Audio => Ok("audio"),
        FileType::Document => Ok("document"),
        FileType::Quiz => Ok("quiz"),
        FileType::Subtitle | FileType::Unknown => Err(LibraryError::InvalidScan(format!(
            "unsupported scanned lesson type: {path}"
        ))),
    }
}

fn file_stem(name: &str) -> String {
    name.rsplit_once('.')
        .map_or(name, |(stem, _)| stem)
        .to_string()
}

fn subtitles_for_file(files: &[FileEntry], media: &FileEntry) -> Vec<PendingSubtitle> {
    let media_stem = file_stem(
        media
            .path
            .rsplit(['/', '\\'])
            .next()
            .unwrap_or(media.path.as_ref()),
    );
    files
        .iter()
        .filter(|file| file.file_type == FileType::Subtitle)
        .filter_map(|file| {
            let language = subtitle_language(&file.name, &media_stem)?;
            Some(PendingSubtitle {
                path: file.path.to_string(),
                label: language.clone(),
                language,
            })
        })
        .collect()
}

fn subtitle_language(name: &str, media_stem: &str) -> Option<String> {
    let subtitle_stem = file_stem(name);
    if subtitle_stem == media_stem {
        return Some("default".to_string());
    }
    let suffix = subtitle_stem.strip_prefix(media_stem)?;
    if let Some(language) = suffix
        .strip_prefix('.')
        .or_else(|| suffix.strip_prefix('_'))
    {
        let language = language.trim();
        return (!language.is_empty()).then(|| language.to_string());
    }
    let language = suffix.strip_prefix(' ')?.trim().to_ascii_lowercase();
    let code = match language.as_str() {
        "english" | "en" => "en",
        "spanish" | "es" => "es",
        "french" | "fr" => "fr",
        "german" | "de" => "de",
        "portuguese" | "pt" => "pt",
        "italian" | "it" => "it",
        _ => return None,
    };
    Some(code.to_string())
}

fn resolve_lessons(
    pending: Vec<PendingLesson>,
    persisted: &[PersistedLesson],
    persisted_ids: &HashSet<String>,
    persisted_paths: &HashMap<String, String>,
    warnings: &mut Vec<String>,
) -> Result<LessonResolution, LibraryError> {
    let exact = persisted
        .iter()
        .enumerate()
        .map(|(index, lesson)| ((lesson.course_id.as_str(), lesson.path.as_str()), index))
        .collect::<HashMap<_, _>>();
    let mut relative: HashMap<(&str, &str), Vec<usize>> = HashMap::new();
    let mut signature: HashMap<(&str, &str, &str, &str, i64), Vec<usize>> = HashMap::new();
    for (index, lesson) in persisted.iter().enumerate() {
        relative
            .entry((lesson.course_id.as_str(), lesson.relative_path.as_str()))
            .or_default()
            .push(index);
        signature
            .entry((
                lesson.course_id.as_str(),
                lesson.section_name.as_str(),
                lesson.name.as_str(),
                lesson.kind.as_str(),
                lesson.file_size,
            ))
            .or_default()
            .push(index);
    }

    let mut matches = vec![None; pending.len()];
    let mut claimed = HashSet::new();
    let mut fallback_blocked = vec![false; pending.len()];
    for (index, lesson) in pending.iter().enumerate() {
        if let Some(&persisted_index) =
            exact.get(&(lesson.course_id.as_str(), lesson.path.as_str()))
        {
            matches[index] = Some(persisted_index);
            claimed.insert(persisted_index);
        }
    }
    let mut state = LessonMatchState {
        matches: &mut matches,
        claimed: &mut claimed,
        blocked: &mut fallback_blocked,
    };
    resolve_lesson_phase(
        &pending,
        &relative,
        &mut state,
        warnings,
        |lesson| (lesson.course_id.as_str(), lesson.relative_path.as_str()),
        "relative path",
    );
    resolve_lesson_phase(
        &pending,
        &signature,
        &mut state,
        warnings,
        |lesson| {
            (
                lesson.course_id.as_str(),
                lesson.section_name.as_str(),
                lesson.name.as_str(),
                lesson.kind.as_str(),
                lesson.file_size,
            )
        },
        "metadata",
    );

    let mut resolved_ids = HashSet::with_capacity(pending.len());
    let mut claimed_ids = HashSet::with_capacity(claimed.len());
    let mut lessons = Vec::with_capacity(pending.len());
    let mut subtitles = Vec::new();
    for (index, pending) in pending.into_iter().enumerate() {
        let id = if let Some(persisted_index) = matches[index] {
            let id = persisted[persisted_index].id.clone();
            claimed_ids.insert(id.clone());
            id
        } else {
            if let Some(course_id) = persisted_paths.get(pending.path.as_str()) {
                return Err(LibraryError::InvalidScan(format!(
                    "scanned lesson path belongs to another stored course: {} ({})",
                    pending.path, course_id
                )));
            }
            allocate_unique_id(pending.scanned_id.as_str(), |candidate| {
                persisted_ids.contains(candidate) || resolved_ids.contains(candidate)
            })
        };
        if !resolved_ids.insert(id.clone()) {
            return Err(LibraryError::InvalidScan(format!(
                "duplicate resolved lesson id: {id}"
            )));
        }
        for (order, subtitle) in pending.subtitles.into_iter().enumerate() {
            let order = i64::try_from(order).map_err(|_| {
                LibraryError::InvalidScan("subtitle order exceeds SQLite integer range".to_string())
            })?;
            subtitles.push(ResolvedSubtitle {
                id: format!("{id}:subtitle:{order}"),
                lesson_id: id.clone(),
                path: subtitle.path,
                language: subtitle.language,
                label: subtitle.label,
                order,
            });
        }
        lessons.push(ResolvedLesson {
            id,
            course_id: pending.course_id,
            section_id: pending.section_id,
            name: pending.name,
            path: pending.path,
            relative_path: pending.relative_path,
            kind: pending.kind,
            file_size: pending.file_size,
            order: pending.order,
        });
    }
    Ok(LessonResolution {
        lessons,
        subtitles,
        claimed_ids,
    })
}

fn resolve_lesson_phase<'a, K, F>(
    pending: &'a [PendingLesson],
    candidates: &HashMap<K, Vec<usize>>,
    state: &mut LessonMatchState<'_>,
    warnings: &mut Vec<String>,
    key: F,
    label: &str,
) where
    K: Eq + std::hash::Hash,
    F: Fn(&'a PendingLesson) -> K,
{
    let mut scanned_by_key: HashMap<K, Vec<usize>> = HashMap::new();
    for (index, lesson) in pending.iter().enumerate() {
        if state.matches[index].is_none() && !state.blocked[index] {
            scanned_by_key.entry(key(lesson)).or_default().push(index);
        }
    }
    for (key, scanned_indexes) in scanned_by_key {
        let available = candidates
            .get(&key)
            .into_iter()
            .flatten()
            .copied()
            .filter(|index| !state.claimed.contains(index))
            .collect::<Vec<_>>();
        if scanned_indexes.len() == 1 && available.len() == 1 {
            let scanned_index = scanned_indexes[0];
            let persisted_index = available[0];
            state.matches[scanned_index] = Some(persisted_index);
            state.claimed.insert(persisted_index);
        } else if candidates.contains_key(&key) {
            for scanned_index in scanned_indexes {
                state.blocked[scanned_index] = true;
                let lesson = &pending[scanned_index];
                push_warning(
                    warnings,
                    format!(
                        "Skipped progress reuse for lesson \"{}\" in \"{}\": the {label} match is ambiguous.",
                        lesson.name, lesson.course_name
                    ),
                );
            }
        }
    }
}

async fn mark_root_courses_missing(
    transaction: &mut Transaction<'_, Sqlite>,
    root_path: &str,
    scan_stamp: &str,
) -> Result<(), LibraryError> {
    let (prefix, upper_bound) = child_path_range(root_path);
    sqlx::query(
        "UPDATE courses
         SET missing_since = COALESCE(missing_since, ?1), last_scanned_at = ?1
         WHERE path = ?2 OR (path > ?3 AND path < ?4)",
    )
    .bind(scan_stamp)
    .bind(root_path)
    .bind(prefix)
    .bind(upper_bound)
    .execute(&mut **transaction)
    .await?;
    Ok(())
}

async fn write_courses(
    transaction: &mut Transaction<'_, Sqlite>,
    courses: &[ResolvedCourse],
    scan_stamp: &str,
    control: &MutationControl,
) -> Result<(), LibraryError> {
    for courses in courses.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query = QueryBuilder::<Sqlite>::new(
            "INSERT INTO courses
             (id, identity_id, name, path, fingerprint, thumbnail_source_path, last_scanned_at)",
        );
        query.push_values(courses, |mut row, course| {
            row.push_bind(&course.id)
                .push_bind(&course.identity_id)
                .push_bind(&course.name)
                .push_bind(&course.path)
                .push_bind(&course.fingerprint)
                .push_bind(&course.thumbnail_source_path)
                .push_bind(scan_stamp);
        });
        query.push(
            " ON CONFLICT(id) DO UPDATE SET
                identity_id = excluded.identity_id,
                name = excluded.name,
                path = excluded.path,
                fingerprint = excluded.fingerprint,
                thumbnail_source_path = excluded.thumbnail_source_path,
                last_scanned_at = excluded.last_scanned_at,
                missing_since = NULL",
        );
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn write_sections(
    transaction: &mut Transaction<'_, Sqlite>,
    sections: &[ResolvedSection],
    scan_stamp: &str,
    control: &MutationControl,
) -> Result<(), LibraryError> {
    for sections in sections.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query = QueryBuilder::<Sqlite>::new(
            "INSERT INTO sections (id, course_id, name, order_index, updated_at)",
        );
        query.push_values(sections, |mut row, section| {
            row.push_bind(&section.id)
                .push_bind(&section.course_id)
                .push_bind(&section.name)
                .push_bind(section.order)
                .push_bind(scan_stamp);
        });
        query.push(
            " ON CONFLICT(id) DO UPDATE SET
                course_id = excluded.course_id,
                name = excluded.name,
                order_index = excluded.order_index,
                updated_at = excluded.updated_at",
        );
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn write_lessons(
    transaction: &mut Transaction<'_, Sqlite>,
    lessons: &[ResolvedLesson],
    scan_stamp: &str,
    control: &MutationControl,
) -> Result<(), LibraryError> {
    for lessons in lessons.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query = QueryBuilder::<Sqlite>::new(
            "INSERT INTO lessons
             (id, course_id, section_id, name, path, relative_path, type,
              file_size, order_index, updated_at)",
        );
        query.push_values(lessons, |mut row, lesson| {
            row.push_bind(&lesson.id)
                .push_bind(&lesson.course_id)
                .push_bind(&lesson.section_id)
                .push_bind(&lesson.name)
                .push_bind(&lesson.path)
                .push_bind(&lesson.relative_path)
                .push_bind(&lesson.kind)
                .push_bind(lesson.file_size)
                .push_bind(lesson.order)
                .push_bind(scan_stamp);
        });
        query.push(
            " ON CONFLICT(id) DO UPDATE SET
                course_id = excluded.course_id,
                section_id = excluded.section_id,
                name = excluded.name,
                path = excluded.path,
                relative_path = excluded.relative_path,
                type = excluded.type,
                file_size = excluded.file_size,
                order_index = excluded.order_index,
                updated_at = excluded.updated_at",
        );
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn delete_rows_by_id(
    transaction: &mut Transaction<'_, Sqlite>,
    table: &str,
    ids: &[String],
    control: &MutationControl,
) -> Result<(), LibraryError> {
    debug_assert!(matches!(table, "lessons" | "sections"));
    for ids in ids.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query = QueryBuilder::<Sqlite>::new(format!("DELETE FROM {table} WHERE id IN ("));
        let mut separated = query.separated(", ");
        for id in ids {
            separated.push_bind(id);
        }
        separated.push_unseparated(")");
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn delete_subtitles(
    transaction: &mut Transaction<'_, Sqlite>,
    lesson_ids: &[String],
    control: &MutationControl,
) -> Result<(), LibraryError> {
    for lesson_ids in lesson_ids.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query =
            QueryBuilder::<Sqlite>::new("DELETE FROM lesson_subtitles WHERE lesson_id IN (");
        let mut separated = query.separated(", ");
        for lesson_id in lesson_ids {
            separated.push_bind(lesson_id);
        }
        separated.push_unseparated(")");
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn write_subtitles(
    transaction: &mut Transaction<'_, Sqlite>,
    subtitles: &[ResolvedSubtitle],
    control: &MutationControl,
) -> Result<(), LibraryError> {
    for subtitles in subtitles.chunks(WRITE_BATCH_SIZE) {
        require_active(control)?;
        let mut query = QueryBuilder::<Sqlite>::new(
            "INSERT INTO lesson_subtitles
             (id, lesson_id, path, language, label, order_index)",
        );
        query.push_values(subtitles, |mut row, subtitle| {
            row.push_bind(&subtitle.id)
                .push_bind(&subtitle.lesson_id)
                .push_bind(&subtitle.path)
                .push_bind(&subtitle.language)
                .push_bind(&subtitle.label)
                .push_bind(subtitle.order);
        });
        query.push(
            " ON CONFLICT(lesson_id, path) DO UPDATE SET
                language = excluded.language,
                label = excluded.label,
                order_index = excluded.order_index",
        );
        query.build().execute(&mut **transaction).await?;
    }
    Ok(())
}

async fn write_library_path(
    transaction: &mut Transaction<'_, Sqlite>,
    root_path: &str,
    scan_stamp: &str,
) -> Result<(), LibraryError> {
    sqlx::query(
        "INSERT INTO app_settings (key, value, updated_at)
         VALUES ('libraryPath', ?1, ?2)
         ON CONFLICT(key) DO UPDATE SET
             value = excluded.value,
             updated_at = excluded.updated_at",
    )
    .bind(root_path)
    .bind(scan_stamp)
    .execute(&mut **transaction)
    .await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::future::Future;
    use std::path::Path;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;

    use sqlx::Row;

    use super::*;

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        tokio::runtime::Builder::new_current_thread()
            .build()
            .expect("build reconciliation test runtime")
            .block_on(future)
    }

    fn touch(path: &Path) {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).expect("create learning item parent");
        }
        std::fs::write(path, b"learning item").expect("write learning item");
    }

    fn root_text(path: &Path) -> &str {
        path.to_str().expect("temporary path is UTF-8")
    }

    async fn open_test_library(data_dir: &Path) -> LibraryDatabase {
        LibraryDatabase::open_current(
            data_dir,
            next_library_revision().expect("allocate test library revision"),
            Arc::new(AtomicBool::new(false)),
        )
        .await
        .expect("open fresh native library")
    }

    #[test]
    fn global_identity_phases_reserve_exact_matches_before_fallbacks() {
        let persisted_courses = vec![
            PersistedCourse {
                id: "course-exact".to_string(),
                identity_id: "identity-exact".to_string(),
                path: "/library/exact".to_string(),
                fingerprint: "shared".to_string(),
            },
            PersistedCourse {
                id: "course-moved".to_string(),
                identity_id: "identity-moved".to_string(),
                path: "/old/moved".to_string(),
                fingerprint: "shared".to_string(),
            },
        ];
        let scanned_courses = vec![
            CourseData {
                id: "new-moved".into(),
                marker_identity_id: None,
                name: "moved".into(),
                path: "/library/moved".into(),
                fingerprint: "shared".into(),
                sections: Box::new([]),
            },
            CourseData {
                id: "new-exact".into(),
                marker_identity_id: None,
                name: "exact".into(),
                path: "/library/exact".into(),
                fingerprint: "shared".into(),
                sections: Box::new([]),
            },
        ];
        let mut warnings = Vec::new();

        let resolved = resolve_courses(scanned_courses, &persisted_courses, &mut warnings)
            .expect("resolve courses globally");

        assert_eq!(resolved[0].id, "course-moved");
        assert_eq!(resolved[1].id, "course-exact");
        assert!(warnings.is_empty());

        let persisted_lessons = vec![
            PersistedLesson {
                id: "lesson-exact".to_string(),
                course_id: "course".to_string(),
                section_name: "section".to_string(),
                name: "exact".to_string(),
                path: "/library/exact.mp4".to_string(),
                relative_path: "same.mp4".to_string(),
                kind: "video".to_string(),
                file_size: 13,
            },
            PersistedLesson {
                id: "lesson-moved".to_string(),
                course_id: "course".to_string(),
                section_name: "section".to_string(),
                name: "moved".to_string(),
                path: "/old/moved.mp4".to_string(),
                relative_path: "same.mp4".to_string(),
                kind: "video".to_string(),
                file_size: 13,
            },
        ];
        let pending_lessons = vec![
            pending_lesson("new-moved", "/library/moved.mp4", "same.mp4", "moved"),
            pending_lesson("new-exact", "/library/exact.mp4", "same.mp4", "exact"),
        ];
        let persisted_ids = persisted_lessons
            .iter()
            .map(|lesson| lesson.id.clone())
            .collect();
        let persisted_paths = persisted_lessons
            .iter()
            .map(|lesson| (lesson.path.clone(), lesson.course_id.clone()))
            .collect();

        let resolved = resolve_lessons(
            pending_lessons,
            &persisted_lessons,
            &persisted_ids,
            &persisted_paths,
            &mut warnings,
        )
        .expect("resolve lessons globally");

        assert_eq!(resolved.lessons[0].id, "lesson-moved");
        assert_eq!(resolved.lessons[1].id, "lesson-exact");
    }

    #[test]
    fn duplicate_markers_fall_through_but_claimed_markers_block_fingerprints() {
        let persisted = vec![
            PersistedCourse {
                id: "course-a".to_string(),
                identity_id: "identity-a".to_string(),
                path: "/library/a".to_string(),
                fingerprint: "fingerprint-a".to_string(),
            },
            PersistedCourse {
                id: "course-b".to_string(),
                identity_id: "identity-b".to_string(),
                path: "/old/b".to_string(),
                fingerprint: "fingerprint-b".to_string(),
            },
        ];
        let duplicate_markers = vec![
            scanned_course("new-a", "/new/a", "fingerprint-a", Some("identity-a")),
            scanned_course("new-b", "/new/b", "fingerprint-b", Some("identity-a")),
        ];
        let mut warnings = Vec::new();
        let resolved = resolve_courses(duplicate_markers, &persisted, &mut warnings)
            .expect("fall through duplicate markers");
        assert_eq!(resolved[0].id, "course-a");
        assert_eq!(resolved[1].id, "course-b");
        assert_eq!(
            warnings
                .iter()
                .filter(|warning| warning.contains("same marker identity"))
                .count(),
            2
        );

        warnings.clear();
        let claimed_marker = vec![
            scanned_course("new-exact", "/library/a", "fingerprint-a", None),
            scanned_course(
                "new-blocked",
                "/new/blocked",
                "fingerprint-b",
                Some("identity-a"),
            ),
        ];
        let resolved = resolve_courses(claimed_marker, &persisted, &mut warnings)
            .expect("block fallback from claimed marker");
        assert_eq!(resolved[0].id, "course-a");
        assert_eq!(resolved[1].id, "new-blocked");
        assert!(
            warnings
                .iter()
                .any(|warning| warning.contains("already claimed"))
        );

        warnings.clear();
        let distinct_marker = vec![scanned_course(
            "new-copy",
            "/new/copy",
            "fingerprint-a",
            Some("identity-copy"),
        )];
        let resolved = resolve_courses(distinct_marker, &persisted, &mut warnings)
            .expect("keep a distinct explicit identity");
        assert_eq!(resolved[0].id, "new-copy");
        assert_eq!(resolved[0].identity_id, "identity-copy");
        assert!(warnings.is_empty());
    }

    fn scanned_course(
        id: &str,
        path: &str,
        fingerprint: &str,
        marker_identity_id: Option<&str>,
    ) -> CourseData {
        CourseData {
            id: id.into(),
            marker_identity_id: marker_identity_id.map(Into::into),
            name: id.into(),
            path: path.into(),
            fingerprint: fingerprint.into(),
            sections: Box::new([]),
        }
    }

    #[test]
    fn ambiguous_relative_lesson_matches_do_not_fall_through_to_metadata() {
        let persisted = vec![
            PersistedLesson {
                id: "lesson-a".to_string(),
                course_id: "course".to_string(),
                section_name: "section".to_string(),
                name: "target".to_string(),
                path: "/old/a.mp4".to_string(),
                relative_path: "duplicate.mp4".to_string(),
                kind: "video".to_string(),
                file_size: 13,
            },
            PersistedLesson {
                id: "lesson-b".to_string(),
                course_id: "course".to_string(),
                section_name: "other".to_string(),
                name: "other".to_string(),
                path: "/old/b.mp4".to_string(),
                relative_path: "duplicate.mp4".to_string(),
                kind: "video".to_string(),
                file_size: 99,
            },
        ];
        let persisted_ids = persisted.iter().map(|lesson| lesson.id.clone()).collect();
        let persisted_paths = persisted
            .iter()
            .map(|lesson| (lesson.path.clone(), lesson.course_id.clone()))
            .collect();
        let mut pending =
            pending_lesson("new-lesson", "/new/target.mp4", "duplicate.mp4", "target");
        pending.section_name = "section".to_string();
        let mut warnings = Vec::new();

        let resolved = resolve_lessons(
            vec![pending],
            &persisted,
            &persisted_ids,
            &persisted_paths,
            &mut warnings,
        )
        .expect("resolve ambiguous relative lesson");

        assert_eq!(resolved.lessons[0].id, "new-lesson");
        assert!(
            warnings
                .iter()
                .any(|warning| warning.contains("relative path match is ambiguous"))
        );
    }

    fn pending_lesson(
        scanned_id: &str,
        path: &str,
        relative_path: &str,
        name: &str,
    ) -> PendingLesson {
        PendingLesson {
            scanned_id: scanned_id.to_string(),
            course_id: "course".to_string(),
            course_name: "course".to_string(),
            section_id: "section".to_string(),
            section_name: "section".to_string(),
            name: name.to_string(),
            path: path.to_string(),
            relative_path: relative_path.to_string(),
            kind: "video".to_string(),
            file_size: 13,
            order: 0,
            subtitles: Vec::new(),
        }
    }

    #[test]
    fn moved_courses_preserve_progress_and_missing_courses_keep_their_graph() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            touch(&root.path().join("Course A/01 Intro/01 keep.mp4"));
            touch(&root.path().join("Course A/01 Intro/02 stale.mp4"));
            touch(&root.path().join("Course B/01 Intro/01 missing.mp4"));
            let mut library = open_test_library(data.path()).await;

            let first = library
                .scan_and_reconcile(
                    library.revision(),
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("initial reconciliation");
            assert_eq!(first.course_count, 2);

            let keep = sqlx::query(
                "SELECT lessons.id, lessons.course_id
                 FROM lessons
                 INNER JOIN courses ON courses.id = lessons.course_id
                 WHERE courses.name = 'Course A' AND lessons.relative_path = '01 Intro/01 keep.mp4'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load retained lesson");
            let keep_id: String = keep.try_get("id").expect("retained lesson id");
            let course_id: String = keep.try_get("course_id").expect("retained course id");
            let stale_id: String = sqlx::query_scalar(
                "SELECT lessons.id
                 FROM lessons
                 INNER JOIN courses ON courses.id = lessons.course_id
                 WHERE courses.name = 'Course A' AND lessons.relative_path = '01 Intro/02 stale.mp4'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load stale lesson");
            let missing_course_id: String =
                sqlx::query_scalar("SELECT id FROM courses WHERE name = 'Course B'")
                    .fetch_one(&mut library.connection)
                    .await
                    .expect("load missing course");
            let missing_lesson_id: String =
                sqlx::query_scalar("SELECT id FROM lessons WHERE course_id = ?1")
                    .bind(&missing_course_id)
                    .fetch_one(&mut library.connection)
                    .await
                    .expect("load missing course lesson");
            sqlx::query(
                "UPDATE lessons
                 SET duration = 500, watched_time = 321, completed = 1, last_position = 42.5
                 WHERE id = ?1",
            )
            .bind(&keep_id)
            .execute(&mut library.connection)
            .await
            .expect("seed progress");
            sqlx::query(
                "UPDATE lessons
                 SET duration = 700, watched_time = 222, completed = 0, last_position = 17.25
                 WHERE id = ?1",
            )
            .bind(&missing_lesson_id)
            .execute(&mut library.connection)
            .await
            .expect("seed missing course progress");
            sqlx::query("UPDATE courses SET last_accessed = '2026-07-15T00:00:00Z' WHERE id = ?1")
                .bind(&course_id)
                .execute(&mut library.connection)
                .await
                .expect("seed last accessed");
            sqlx::query(
                "INSERT INTO notes (id, lesson_id, timestamp, text)
                 VALUES ('note-keep', ?1, 12.5, 'keep'),
                        ('note-stale', ?2, 1, 'delete'),
                        ('note-missing', ?3, 2, 'retain')",
            )
            .bind(&keep_id)
            .bind(&stale_id)
            .bind(&missing_lesson_id)
            .execute(&mut library.connection)
            .await
            .expect("seed notes");
            sqlx::query(
                "INSERT INTO lesson_activity
                 (id, course_id, lesson_id, activity_date, watched_seconds, completed)
                 VALUES ('activity-keep', ?1, ?2, '2026-07-15', 321, 1),
                        ('activity-stale', ?1, ?3, '2026-07-14', 10, 0),
                        ('activity-missing', ?4, ?5, '2026-07-13', 222, 0)",
            )
            .bind(&course_id)
            .bind(&keep_id)
            .bind(&stale_id)
            .bind(&missing_course_id)
            .bind(&missing_lesson_id)
            .execute(&mut library.connection)
            .await
            .expect("seed activity");
            sqlx::query(
                "INSERT INTO lesson_subtitles
                 (id, lesson_id, path, language, label, order_index)
                 VALUES ('subtitle-stale', ?1, '/stale.srt', 'en', 'English', 0),
                        ('subtitle-missing', ?2, '/missing.srt', 'en', 'English', 0)",
            )
            .bind(&stale_id)
            .bind(&missing_lesson_id)
            .execute(&mut library.connection)
            .await
            .expect("seed subtitles");

            let moved = root.path().join("Course A Moved");
            std::fs::rename(root.path().join("Course A"), &moved).expect("move marked course");
            std::fs::remove_file(moved.join("01 Intro/02 stale.mp4")).expect("remove stale lesson");
            std::fs::remove_dir_all(root.path().join("Course B")).expect("remove missing course");

            let second = library
                .scan_and_reconcile(
                    first.revision,
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("reconcile moved course");
            assert!(second.revision > first.revision);
            assert_eq!(second.course_count, 1);

            let course = sqlx::query(
                "SELECT id, identity_id, last_accessed, missing_since
                 FROM courses WHERE name = 'Course A Moved'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load moved course");
            assert_eq!(course.try_get::<String, _>("id").unwrap(), course_id);
            assert_eq!(
                course
                    .try_get::<Option<String>, _>("last_accessed")
                    .unwrap()
                    .as_deref(),
                Some("2026-07-15T00:00:00Z")
            );
            assert_eq!(
                course
                    .try_get::<Option<String>, _>("missing_since")
                    .unwrap(),
                None
            );

            let lesson = sqlx::query(
                "SELECT id, duration, watched_time, completed, last_position
                 FROM lessons WHERE course_id = ?1",
            )
            .bind(&course_id)
            .fetch_one(&mut library.connection)
            .await
            .expect("load retained lesson progress");
            assert_eq!(lesson.try_get::<String, _>("id").unwrap(), keep_id);
            assert_eq!(lesson.try_get::<i64, _>("duration").unwrap(), 500);
            assert_eq!(lesson.try_get::<i64, _>("watched_time").unwrap(), 321);
            assert_eq!(lesson.try_get::<i64, _>("completed").unwrap(), 1);
            assert_eq!(lesson.try_get::<f64, _>("last_position").unwrap(), 42.5);
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM notes WHERE lesson_id = ?1")
                    .bind(&keep_id)
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                1
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM notes WHERE lesson_id = ?1")
                    .bind(&stale_id)
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                0
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT COUNT(*) FROM lesson_activity WHERE lesson_id = ?1"
                )
                .bind(&keep_id)
                .fetch_one(&mut library.connection)
                .await
                .unwrap(),
                1
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT COUNT(*) FROM lesson_activity WHERE lesson_id = ?1"
                )
                .bind(&stale_id)
                .fetch_one(&mut library.connection)
                .await
                .unwrap(),
                0
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT COUNT(*) FROM lesson_subtitles WHERE lesson_id = ?1"
                )
                .bind(&stale_id)
                .fetch_one(&mut library.connection)
                .await
                .unwrap(),
                0
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT COUNT(*) FROM courses WHERE id = ?1 AND missing_since IS NOT NULL"
                )
                .bind(&missing_course_id)
                .fetch_one(&mut library.connection)
                .await
                .unwrap(),
                1
            );
            let missing_lesson = sqlx::query(
                "SELECT duration, watched_time, completed, last_position
                 FROM lessons WHERE id = ?1",
            )
            .bind(&missing_lesson_id)
            .fetch_one(&mut library.connection)
            .await
            .expect("load retained missing lesson");
            assert_eq!(missing_lesson.try_get::<i64, _>("duration").unwrap(), 700);
            assert_eq!(
                missing_lesson.try_get::<i64, _>("watched_time").unwrap(),
                222
            );
            assert_eq!(missing_lesson.try_get::<i64, _>("completed").unwrap(), 0);
            assert_eq!(
                missing_lesson.try_get::<f64, _>("last_position").unwrap(),
                17.25
            );
            for table in ["notes", "lesson_activity", "lesson_subtitles"] {
                let count: i64 = sqlx::query_scalar(&format!(
                    "SELECT COUNT(*) FROM {table} WHERE lesson_id = ?1"
                ))
                .bind(&missing_lesson_id)
                .fetch_one(&mut library.connection)
                .await
                .unwrap();
                assert_eq!(count, 1, "missing lesson {table} should survive");
            }
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM lessons WHERE course_id = ?1")
                    .bind(&missing_course_id)
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                1
            );
            assert!(
                sqlx::query("PRAGMA foreign_key_check")
                    .fetch_all(&mut library.connection)
                    .await
                    .unwrap()
                    .is_empty()
            );
        });
    }

    #[test]
    fn cancellation_and_small_payloads_leave_no_scan_state() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            touch(&root.path().join("Course/01 Intro/01 lesson.mp4"));
            let mut library = open_test_library(data.path()).await;
            let initial_revision = library.revision();

            let cancelled = MutationControl::new();
            assert!(cancelled.cancel());
            assert!(matches!(
                library
                    .scan_and_reconcile(
                        initial_revision,
                        root_text(root.path()),
                        usize::MAX,
                        &cancelled,
                    )
                    .await,
                Err(LibraryError::Cancelled)
            ));

            assert!(matches!(
                library
                    .scan_and_reconcile(
                        initial_revision,
                        root_text(root.path()),
                        0,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::ResponseTooLarge { limit: 0 })
            ));
            assert_eq!(library.revision(), initial_revision);
            assert_eq!(library.library_path, None);
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM courses")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                0
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM app_settings")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                0
            );
            assert!(!root.path().join("Course/.melearner-course.json").exists());
        });
    }

    #[test]
    fn marker_failures_are_postcommit_warnings_and_respect_payload_bounds() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            touch(&root.path().join("Course/01 Intro/01 lesson.mp4"));
            let marker = root.path().join("Course/.melearner-course.json");
            let existing = b"not owned marker bytes";
            std::fs::write(&marker, existing).expect("write invalid marker");
            let mut library = open_test_library(data.path()).await;
            let initial_revision = library.revision();

            let first = library
                .scan_and_reconcile(
                    initial_revision,
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("commit despite marker failure");

            assert!(first.revision > initial_revision);
            assert_eq!(first.course_count, 1);
            assert!(
                first
                    .warnings
                    .iter()
                    .any(|warning| warning.contains("Could not write marker"))
            );
            assert_eq!(std::fs::read(&marker).unwrap(), existing);
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM courses")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                1
            );

            let bounded = library
                .scan_and_reconcile(
                    first.revision,
                    root_text(root.path()),
                    64,
                    &MutationControl::new(),
                )
                .await
                .expect("trim postcommit marker warning");
            assert!(bounded.revision > first.revision);
            assert!(bounded.warnings.is_empty());
            assert_eq!(std::fs::read(&marker).unwrap(), existing);
        });
    }

    #[cfg(unix)]
    #[test]
    fn reconciliation_keeps_scan_time_course_handles_for_marker_writes() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            let course = root.path().join("Course");
            let original = root.path().join("Original");
            touch(&course.join("01 Intro/01 lesson.mp4"));
            let canonical_root = std::fs::canonicalize(root.path()).expect("resolve library root");
            let root_dir = CapabilityDir::open_ambient_dir(&canonical_root, ambient_authority())
                .expect("capture library root");
            let captured =
                scan_library_checked_in_root(&canonical_root, &root_dir, &MutationControl::new())
                    .expect("capture scanned courses");
            std::fs::rename(&course, &original).expect("move scanned course");
            std::fs::create_dir(&course).expect("create replacement course");
            let mut library = open_test_library(data.path()).await;

            library
                .reconcile_scan(
                    library.revision(),
                    root_text(&canonical_root),
                    captured,
                    usize::MAX,
                    &MutationControl::new(),
                    &root_dir,
                )
                .await
                .expect("reconcile captured course");

            assert!(original.join(".melearner-course.json").exists());
            assert!(!course.join(".melearner-course.json").exists());
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM courses")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                1
            );
        });
    }

    #[cfg(unix)]
    #[test]
    fn invalid_present_courses_do_not_become_missing() {
        use std::os::unix::net::UnixListener;

        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            let lesson = root.path().join("Course/01 Intro/01 lesson.mp4");
            touch(&lesson);
            let mut library = open_test_library(data.path()).await;
            let first = library
                .scan_and_reconcile(
                    library.revision(),
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("initial scan");
            std::fs::remove_file(&lesson).expect("remove learning file");
            let _socket = UnixListener::bind(&lesson).expect("make course entry unsupported");

            assert!(matches!(
                library
                    .scan_and_reconcile(
                        first.revision,
                        root_text(root.path()),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::InvalidScan(_))
            ));
            assert_eq!(library.revision(), first.revision);
            assert_eq!(
                sqlx::query_scalar::<_, i64>(
                    "SELECT COUNT(*) FROM courses WHERE missing_since IS NULL"
                )
                .fetch_one(&mut library.connection)
                .await
                .unwrap(),
                1
            );
        });
    }

    #[test]
    fn database_failures_roll_back_before_marker_writes() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            touch(&root.path().join("Course/01 Intro/01 lesson.mp4"));
            let mut library = open_test_library(data.path()).await;
            let initial_revision = library.revision();
            sqlx::query(
                "CREATE TEMP TRIGGER fail_lesson_insert
                 BEFORE INSERT ON lessons
                 BEGIN SELECT RAISE(ABORT, 'forced lesson failure'); END",
            )
            .execute(&mut library.connection)
            .await
            .expect("install rollback trigger");

            assert!(matches!(
                library
                    .scan_and_reconcile(
                        initial_revision,
                        root_text(root.path()),
                        usize::MAX,
                        &MutationControl::new(),
                    )
                    .await,
                Err(LibraryError::Database(_))
            ));
            assert_eq!(library.revision(), initial_revision);
            assert_eq!(library.library_path, None);
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM courses")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                0
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM app_settings")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                0
            );
            assert!(!root.path().join("Course/.melearner-course.json").exists());
        });
    }

    #[test]
    fn topology_changes_cannot_reassign_a_lesson_to_another_course() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            let lesson_path = root.path().join("Course/01 Intro/01 lesson.mp4");
            touch(&lesson_path);
            let mut library = open_test_library(data.path()).await;
            let first = library
                .scan_and_reconcile(
                    library.revision(),
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("initial library scan");
            let original = sqlx::query("SELECT id, course_id FROM lessons")
                .fetch_one(&mut library.connection)
                .await
                .expect("load original lesson");
            let lesson_id: String = original.try_get("id").unwrap();
            let course_id: String = original.try_get("course_id").unwrap();

            touch(&root.path().join("00 overview.mp4"));
            let topology_result = library
                .scan_and_reconcile(
                    first.revision,
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await;
            assert!(
                matches!(
                    &topology_result,
                    Err(LibraryError::InvalidScan(message))
                        if message.contains("belongs to another stored course")
                ),
                "unexpected topology result: {topology_result:?}"
            );
            assert_eq!(library.revision(), first.revision);
            let unchanged = sqlx::query("SELECT id, course_id FROM lessons")
                .fetch_one(&mut library.connection)
                .await
                .expect("load unchanged lesson");
            assert_eq!(unchanged.try_get::<String, _>("id").unwrap(), lesson_id);
            assert_eq!(
                unchanged.try_get::<String, _>("course_id").unwrap(),
                course_id
            );
            assert_eq!(
                sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM courses")
                    .fetch_one(&mut library.connection)
                    .await
                    .unwrap(),
                1
            );
            assert!(!root.path().join(".melearner-course.json").exists());
        });
    }

    #[test]
    fn reused_paths_allocate_new_course_section_and_lesson_ids() {
        block_on(async {
            let data = tempfile::tempdir().expect("create state directory");
            let root = tempfile::tempdir().expect("create library root");
            touch(&root.path().join("A/01 Intro/01 original.mp4"));
            let mut library = open_test_library(data.path()).await;
            let first = library
                .scan_and_reconcile(
                    library.revision(),
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("initial scan");
            let original = sqlx::query(
                "SELECT courses.id AS course_id, sections.id AS section_id, lessons.id AS lesson_id
                 FROM courses
                 INNER JOIN sections ON sections.course_id = courses.id
                 INNER JOIN lessons ON lessons.section_id = sections.id",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load original ids");
            let original_course_id: String = original.try_get("course_id").unwrap();
            let original_section_id: String = original.try_get("section_id").unwrap();
            let original_lesson_id: String = original.try_get("lesson_id").unwrap();

            std::fs::rename(root.path().join("A"), root.path().join("B"))
                .expect("move marked course");
            let second = library
                .scan_and_reconcile(
                    first.revision,
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("reconcile moved course");
            touch(&root.path().join("A/01 Intro/01 replacement.mp4"));

            library
                .scan_and_reconcile(
                    second.revision,
                    root_text(root.path()),
                    usize::MAX,
                    &MutationControl::new(),
                )
                .await
                .expect("reconcile reused path");

            let retained = sqlx::query(
                "SELECT courses.id AS course_id, sections.id AS section_id, lessons.id AS lesson_id
                 FROM courses
                 INNER JOIN sections ON sections.course_id = courses.id
                 INNER JOIN lessons ON lessons.section_id = sections.id
                 WHERE courses.name = 'B'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load retained ids");
            assert_eq!(
                retained.try_get::<String, _>("course_id").unwrap(),
                original_course_id
            );
            assert_eq!(
                retained.try_get::<String, _>("section_id").unwrap(),
                original_section_id
            );
            assert_eq!(
                retained.try_get::<String, _>("lesson_id").unwrap(),
                original_lesson_id
            );

            let replacement = sqlx::query(
                "SELECT courses.id AS course_id, sections.id AS section_id, lessons.id AS lesson_id
                 FROM courses
                 INNER JOIN sections ON sections.course_id = courses.id
                 INNER JOIN lessons ON lessons.section_id = sections.id
                 WHERE courses.name = 'A'",
            )
            .fetch_one(&mut library.connection)
            .await
            .expect("load replacement ids");
            assert_ne!(
                replacement.try_get::<String, _>("course_id").unwrap(),
                original_course_id
            );
            assert_ne!(
                replacement.try_get::<String, _>("section_id").unwrap(),
                original_section_id
            );
            assert_ne!(
                replacement.try_get::<String, _>("lesson_id").unwrap(),
                original_lesson_id
            );
        });
    }
}
