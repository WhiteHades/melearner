#ifndef MELEARNER_CORE_H
#define MELEARNER_CORE_H

/* Generated from crates/melearner-core. Do not edit by hand. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ML_ABI_VERSION 2

#define ML_MAX_EVENT_QUEUE_CAPACITY 65536

#define ML_MAX_EVENT_PAYLOAD_BYTES ((16 * 1024) * 1024)

#define ML_MIN_EVENT_PAYLOAD_BYTES 20

#define ML_MAX_SEARCH_QUERY_BYTES (64 * 1024)

#define ML_MAX_NOTE_TEXT_BYTES (8 * 1024)

typedef struct ml_core_t ml_core_t;

typedef uint32_t ml_status_t;

typedef struct ml_config_v2 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t event_queue_capacity;
  uint32_t max_event_payload_bytes;
  const uint8_t *state_dir;
  size_t state_dir_len;
} ml_config_v2;

typedef void (*ml_wake_fn)(void *context);

typedef struct ml_core_limits_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t event_queue_capacity;
  uint32_t max_event_payload_bytes;
} ml_core_limits_v1;

typedef uint32_t ml_event_kind_t;

typedef struct ml_event_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t sequence;
  uint64_t request_id;
  ml_event_kind_t kind;
  ml_status_t status;
  uint32_t payload_schema_version;
  uint32_t reserved;
  const uint8_t *payload;
  size_t payload_len;
} ml_event_v1;

typedef struct ml_library_course_page_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t offset;
  uint32_t limit;
  uint32_t reserved;
} ml_library_course_page_request_v1;

typedef struct ml_library_lesson_page_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t offset;
  uint32_t limit;
  uint32_t reserved;
  const uint8_t *course_id;
  size_t course_id_len;
  const uint8_t *section_id;
  size_t section_id_len;
} ml_library_lesson_page_request_v1;

typedef struct ml_library_scan_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  const uint8_t *root_path;
  size_t root_path_len;
} ml_library_scan_request_v1;

typedef struct ml_progress_put_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t watched_time;
  double last_position;
  uint32_t completed;
  uint32_t reserved;
  const uint8_t *lesson_id;
  size_t lesson_id_len;
} ml_progress_put_request_v1;

typedef struct ml_activity_day_page_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t offset;
  uint32_t lookback_days;
  uint32_t limit;
  uint32_t reserved;
} ml_activity_day_page_request_v1;

typedef struct ml_search_rebuild_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t reserved;
} ml_search_rebuild_request_v1;

typedef struct ml_search_query_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_index_revision;
  uint64_t query_id;
  uint64_t offset;
  uint32_t limit;
  uint32_t reserved;
  const uint8_t *query;
  size_t query_len;
} ml_search_query_request_v1;

typedef struct ml_notes_list_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t offset;
  uint32_t limit;
  uint32_t reserved;
  const uint8_t *lesson_id;
  size_t lesson_id_len;
} ml_notes_list_request_v1;

typedef struct ml_notes_save_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  double timestamp;
  uint64_t reserved;
  const uint8_t *lesson_id;
  size_t lesson_id_len;
  const uint8_t *note_id;
  size_t note_id_len;
  const uint8_t *text;
  size_t text_len;
} ml_notes_save_request_v1;

typedef struct ml_notes_delete_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t expected_revision;
  uint64_t reserved;
  const uint8_t *note_id;
  size_t note_id_len;
} ml_notes_delete_request_v1;

#define ML_STATUS_OK 0

#define ML_STATUS_INVALID_ARGUMENT 1

#define ML_STATUS_ABI_MISMATCH 2

#define ML_STATUS_INVALID_HANDLE 3

#define ML_STATUS_EMPTY 4

#define ML_STATUS_BUSY 5

#define ML_STATUS_PANIC 6

#define ML_STATUS_CANCELLED 7

#define ML_STATUS_FAILED 8

#define ML_STATUS_NOT_FOUND 9

#define ML_STATUS_STALE 10

#define ML_EVENT_CORE_READY 1

#define ML_EVENT_REQUEST_CANCELLED 2

#define ML_EVENT_FATAL 3

#define ML_EVENT_LIBRARY_COURSE_PAGE 4

#define ML_EVENT_LIBRARY_LESSON_PAGE 5

#define ML_EVENT_LIBRARY_SCAN 6

#define ML_EVENT_PROGRESS_UPDATED 7

#define ML_EVENT_ACTIVITY_DAY_PAGE 8

#define ML_EVENT_SEARCH_INDEX_READY 9

#define ML_EVENT_SEARCH_PAGE 10

#define ML_EVENT_NOTES_PAGE 11

#define ML_EVENT_NOTE_SAVED 12

#define ML_EVENT_NOTE_DELETED 13

uint32_t ml_abi_version(void);

/**
 * Creates a core and writes its opaque handle to `out_core`.
 *
 * # Safety
 *
 * `config` must point to a readable `ml_config_v2`. Its state-directory bytes
 * must remain readable for this call. `out_core` must point to
 * writable storage for one handle. Both pointers are borrowed only for this call.
 */
ml_status_t ml_core_create(const struct ml_config_v2 *config, struct ml_core_t **out_core);

/**
 * Destroys a core. Null, stale, and already-destroyed handles are ignored.
 *
 * # Safety
 *
 * `core` must be null or an opaque handle previously returned by
 * `ml_core_create`. Any event payload borrows end during this call.
 */
void ml_core_destroy(struct ml_core_t *core);

/**
 * Replaces the thread-safe empty-to-nonempty event waker.
 *
 * Passing a null callback and null context clears the current waker. Clearing
 * or replacing a waker waits for its active calls to finish. Registration does
 * not wake for events that were already queued, so callers must drain once.
 * When called by the active callback itself, retirement completes when that
 * callback returns.
 *
 * # Safety
 *
 * The callback and context must be safe to invoke from any thread until the
 * registration is cleared or the core is destroyed. The callback must not unwind.
 */
ml_status_t ml_core_set_waker(struct ml_core_t *core, ml_wake_fn callback, void *context);

/**
 * Returns the configured transport bounds for a core.
 *
 * # Safety
 *
 * `out_limits` must point to a writable `ml_core_limits_v1` whose versioned
 * prefix is initialized by the caller.
 */
ml_status_t ml_core_get_limits_v1(struct ml_core_t *core, struct ml_core_limits_v1 *out_limits);

/**
 * Polls one event and transfers its payload borrow to the caller.
 *
 * # Safety
 *
 * `core` must be an opaque handle returned by `ml_core_create`. `out_event`
 * must point to writable `ml_event_v1` storage with an initialized versioned
 * prefix. A successful event must be returned with `ml_core_release_event`.
 */
ml_status_t ml_core_poll_event(struct ml_core_t *core, struct ml_event_v1 *out_event);

/**
 * Releases one event previously returned by `ml_core_poll_event`.
 *
 * # Safety
 *
 * `core` must be the handle that produced `event`. `event` must remain writable
 * for this call and must not have been released already.
 */
void ml_core_release_event(struct ml_core_t *core, struct ml_event_v1 *event);

/**
 * Cancels an active asynchronous request.
 */
ml_status_t ml_core_cancel(struct ml_core_t *core, uint64_t request_id);

/**
 * Submits one asynchronous Library course-page request.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_library_course_page_request_v1`, and
 * `out_request_id` must point to writable `u64` storage. Both pointers are
 * borrowed only for this call.
 */
ml_status_t ml_library_course_page_v1(struct ml_core_t *core,
                                      const struct ml_library_course_page_request_v1 *request,
                                      uint64_t *out_request_id);

/**
 * Submits one asynchronous Library lesson-page request.
 *
 * A null `section_id` with zero length selects all Sections in the Course.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_library_lesson_page_request_v1`.
 * Its ID byte ranges must remain readable for this call. `out_request_id`
 * must point to writable `u64` storage. All inputs are copied before return.
 */
ml_status_t ml_library_lesson_page_v1(struct ml_core_t *core,
                                      const struct ml_library_lesson_page_request_v1 *request,
                                      uint64_t *out_request_id);

/**
 * Submits one asynchronous Library scan and reconciliation request.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_library_scan_request_v1`. Its root
 * path bytes must remain readable for this call. `out_request_id` must point
 * to writable `u64` storage. All inputs are copied before return.
 */
ml_status_t ml_library_scan_v1(struct ml_core_t *core,
                               const struct ml_library_scan_request_v1 *request,
                               uint64_t *out_request_id);

/**
 * Submits one asynchronous Lesson Progress update.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_progress_put_request_v1`. Its Lesson
 * ID bytes must remain readable for this call. `out_request_id` must point to
 * writable `u64` storage. All inputs are copied before return.
 */
ml_status_t ml_progress_put_v1(struct ml_core_t *core,
                               const struct ml_progress_put_request_v1 *request,
                               uint64_t *out_request_id);

/**
 * Submits one asynchronous Learning activity day-page request.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_activity_day_page_request_v1`, and
 * `out_request_id` must point to writable `u64` storage. Both pointers are
 * borrowed only for this call.
 */
ml_status_t ml_activity_day_page_v1(struct ml_core_t *core,
                                    const struct ml_activity_day_page_request_v1 *request,
                                    uint64_t *out_request_id);

/**
 * Rebuilds the in-memory Lesson search index from the current Library.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_search_rebuild_request_v1`, and
 * `out_request_id` must point to writable `u64` storage. Both pointers are
 * borrowed only for this call.
 */
ml_status_t ml_search_rebuild_v1(struct ml_core_t *core,
                                 const struct ml_search_rebuild_request_v1 *request,
                                 uint64_t *out_request_id);

/**
 * Submits one asynchronous paged Lesson search query.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_search_query_request_v1`. Its query
 * bytes must remain readable for this call. `out_request_id` must point to
 * writable `u64` storage. The query is copied before return.
 */
ml_status_t ml_search_query_v1(struct ml_core_t *core,
                               const struct ml_search_query_request_v1 *request,
                               uint64_t *out_request_id);

/**
 * Submits one asynchronous paged Lesson-note list request.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_notes_list_request_v1`. Its Lesson
 * ID bytes must remain readable for this call. `out_request_id` must point to
 * writable `u64` storage. The Lesson ID is copied before return.
 */
ml_status_t ml_notes_list_v1(struct ml_core_t *core,
                             const struct ml_notes_list_request_v1 *request,
                             uint64_t *out_request_id);

/**
 * Creates or updates one Lesson note asynchronously.
 *
 * A null `note_id` with a zero length creates a note. A nonempty `note_id`
 * updates that note without changing its Lesson or creation time.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_notes_save_request_v1`. Its Lesson
 * ID, optional note ID, and text bytes must remain readable for this call.
 * `out_request_id` must point to writable `u64` storage. All inputs are
 * copied before return.
 */
ml_status_t ml_notes_save_v1(struct ml_core_t *core,
                             const struct ml_notes_save_request_v1 *request,
                             uint64_t *out_request_id);

/**
 * Deletes one Lesson note asynchronously. Missing note IDs are successful
 * no-ops and keep the current Library revision.
 *
 * # Safety
 *
 * `request` must point to a readable `ml_notes_delete_request_v1`. Its note
 * ID bytes must remain readable for this call. `out_request_id` must point to
 * writable `u64` storage. The note ID is copied before return.
 */
ml_status_t ml_notes_delete_v1(struct ml_core_t *core,
                               const struct ml_notes_delete_request_v1 *request,
                               uint64_t *out_request_id);

#endif  /* MELEARNER_CORE_H */
