#ifndef MELEARNER_CORE_H
#define MELEARNER_CORE_H

/* Generated from crates/melearner-core. Do not edit by hand. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ML_ABI_VERSION 1

#define ML_MAX_EVENT_QUEUE_CAPACITY 65536

#define ML_MAX_EVENT_PAYLOAD_BYTES ((16 * 1024) * 1024)

typedef struct ml_core_t ml_core_t;

typedef uint32_t ml_status_t;

typedef struct ml_config_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t event_queue_capacity;
  uint32_t max_event_payload_bytes;
} ml_config_v1;

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

#define ML_EVENT_CORE_READY 1

#define ML_EVENT_REQUEST_CANCELLED 2

#define ML_EVENT_FATAL 3

uint32_t ml_abi_version(void);

/**
 * Creates a core and writes its opaque handle to `out_core`.
 *
 * # Safety
 *
 * `config` must point to a readable `ml_config_v1`. `out_core` must point to
 * writable storage for one handle. Both pointers are borrowed only for this call.
 */
ml_status_t ml_core_create(const struct ml_config_v1 *config, struct ml_core_t **out_core);

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

#endif  /* MELEARNER_CORE_H */
