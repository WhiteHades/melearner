#include "melearner_core.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "melearner C ABI smoke test requires ISO C23"
#endif

constexpr uint32_t expected_abi = 2;
constexpr size_t max_poll_attempts = 2'000'000;

static_assert(ML_ABI_VERSION == expected_abi);
static_assert(ML_STATUS_OK == 0);
static_assert(ML_STATUS_TOO_LATE == 11);
static_assert(ML_EVENT_CORE_READY == 1);
static_assert(ML_EVENT_LIBRARY_STATS == 15);
static_assert(ML_EVENT_LIBRARY_STATE == 16);
static_assert(ML_EVENT_SETTINGS == 17);
static_assert(ML_EVENT_APPEARANCE_UPDATED == 18);
static_assert(ML_EVENT_DOCUMENT_OPENED == 19);
static_assert(ML_EVENT_DOCUMENT_PAGE == 20);
static_assert(ML_EVENT_DOCUMENT_EXTERNAL_OPEN_READY == 21);
static_assert(ML_APPEARANCE_LIGHT == 1);
static_assert(ML_APPEARANCE_DARK == 2);
static_assert(ML_APPEARANCE_COZY == 3);
static_assert(ML_SCAN_PHASE_DISCOVERING == 1);
static_assert(ML_SCAN_PHASE_WRITING_MARKERS == 5);
static_assert(sizeof(ml_config_v2) == 16 + sizeof(size_t) * 2);
static_assert(offsetof(ml_config_v2, state_dir) == 16);
static_assert(sizeof(ml_library_stats_request_v1) == 24);
static_assert(offsetof(ml_library_stats_request_v1, expected_revision) == 8);
static_assert(sizeof(ml_library_state_request_v1) == 24);
static_assert(offsetof(ml_library_state_request_v1, expected_revision) == 8);
static_assert(sizeof(ml_settings_get_request_v1) == 16);
static_assert(offsetof(ml_settings_get_request_v1, reserved) == 8);
static_assert(sizeof(ml_settings_put_appearance_request_v1) == 24);
static_assert(offsetof(ml_settings_put_appearance_request_v1, expected_revision) == 8);
static_assert(offsetof(ml_settings_put_appearance_request_v1, appearance) == 16);
static_assert(offsetof(ml_settings_put_appearance_request_v1, reserved) == 20);
static_assert(sizeof(ml_document_open_request_v1) == 24 + sizeof(size_t) * 2);
static_assert(offsetof(ml_document_open_request_v1, expected_revision) == 8);
static_assert(offsetof(ml_document_open_request_v1, reserved) == 16);
static_assert(offsetof(ml_document_open_request_v1, lesson_id) == 24);
static_assert(sizeof(ml_document_page_request_v1) == 32 + sizeof(size_t) * 2);
static_assert(offsetof(ml_document_page_request_v1, expected_revision) == 8);
static_assert(offsetof(ml_document_page_request_v1, offset) == 16);
static_assert(offsetof(ml_document_page_request_v1, limit) == 24);
static_assert(offsetof(ml_document_page_request_v1, reserved) == 28);
static_assert(offsetof(ml_document_page_request_v1, document_id) == 32);
static_assert(sizeof(ml_document_external_open_request_v1) == 24 + sizeof(size_t) * 2);
static_assert(offsetof(ml_document_external_open_request_v1, lesson_id) == 24);
static_assert(sizeof(ml_library_scan_progress_snapshot_v1) == 48);
static_assert(offsetof(ml_library_scan_progress_snapshot_v1, request_id) == 8);
static_assert(offsetof(ml_library_scan_progress_snapshot_v1, processed) == 16);
static_assert(offsetof(ml_library_scan_progress_snapshot_v1, phase) == 40);
static_assert(offsetof(ml_library_scan_progress_snapshot_v1, total_known) == 44);
static_assert(offsetof(ml_library_scan_progress_snapshot_v1, reserved) == 46);
static_assert(offsetof(ml_event_v1, sequence) == 8);
static_assert(offsetof(ml_event_v1, payload) == 40);
static_assert(offsetof(ml_event_v1, payload_len) == 40 + sizeof(void *));

#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);             \
      return EXIT_FAILURE;                                                                         \
    }                                                                                              \
  } while (0)

static ml_status_t poll_event(ml_core_t *core, ml_event_v1 *event) {
  *event = (ml_event_v1){
      .struct_size = sizeof(*event),
      .abi_version = ML_ABI_VERSION,
  };
  for (size_t attempt = 0; attempt < max_poll_attempts; ++attempt) {
    const ml_status_t status = ml_core_poll_event(core, event);
    if (status != ML_STATUS_EMPTY) {
      return status;
    }
    thrd_yield();
  }
  return ML_STATUS_EMPTY;
}

static bool copy_payload(const ml_event_v1 *event, char *buffer, size_t capacity) {
  if (event->payload == nullptr || event->payload_len == 0 || event->payload_len >= capacity) {
    return false;
  }
  memcpy(buffer, event->payload, event->payload_len);
  buffer[event->payload_len] = '\0';
  return true;
}

static bool parse_revision(const ml_event_v1 *event, uint64_t *revision) {
  char text[32];
  if (!copy_payload(event, text, sizeof(text))) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end != text + event->payload_len || value == 0) {
    return false;
  }
  *revision = (uint64_t)value;
  return true;
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  const char *state_dir = argv[1];
  const size_t state_dir_len = strlen(state_dir);
  CHECK(state_dir_len != 0);

  const ml_config_v2 config = {
      .struct_size = sizeof(config),
      .abi_version = ML_ABI_VERSION,
      .event_queue_capacity = 4,
      .max_event_payload_bytes = 4096,
      .state_dir = (const uint8_t *)state_dir,
      .state_dir_len = state_dir_len,
  };
  ml_config_v2 incompatible_config = config;
  incompatible_config.abi_version = ML_ABI_VERSION + 1;
  ml_core_t *incompatible_core = nullptr;
  CHECK(ml_core_create(&incompatible_config, &incompatible_core) == ML_STATUS_ABI_MISMATCH);
  CHECK(incompatible_core == nullptr);

  ml_core_t *core = nullptr;
  CHECK(ml_abi_version() == expected_abi);
  CHECK(ml_core_create(&config, &core) == ML_STATUS_OK);
  CHECK(core != nullptr);
  CHECK(ml_core_set_waker(core, nullptr, nullptr) == ML_STATUS_OK);

  ml_core_limits_v1 limits = {
      .struct_size = sizeof(limits),
      .abi_version = ML_ABI_VERSION,
  };
  CHECK(ml_core_get_limits_v1(core, &limits) == ML_STATUS_OK);
  CHECK(limits.event_queue_capacity == config.event_queue_capacity);
  CHECK(limits.max_event_payload_bytes == config.max_event_payload_bytes);

  ml_event_v1 event;
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.sequence != 0);
  CHECK(event.kind == ML_EVENT_CORE_READY);
  CHECK(event.status == ML_STATUS_OK);
  CHECK(event.payload_schema_version == 1);
  uint64_t revision = 0;
  CHECK(parse_revision(&event, &revision));
  ml_core_release_event(core, &event);

  const ml_library_state_request_v1 state_request = {
      .struct_size = sizeof(state_request),
      .abi_version = ML_ABI_VERSION,
      .expected_revision = revision,
      .reserved = 0,
  };
  uint64_t request_id = 0;
  CHECK(ml_library_state_v1(core, &state_request, &request_id) == ML_STATUS_OK);
  CHECK(request_id != 0);
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.request_id == request_id);
  CHECK(event.kind == ML_EVENT_LIBRARY_STATE);
  CHECK(event.status == ML_STATUS_OK);
  char state_payload[128];
  CHECK(copy_payload(&event, state_payload, sizeof(state_payload)));
  CHECK(strstr(state_payload, "\"rootPath\":null") != nullptr);
  ml_core_release_event(core, &event);

  constexpr char missing_lesson[] = "missing-document";
  const ml_document_open_request_v1 document_request = {
      .struct_size = sizeof(document_request),
      .abi_version = ML_ABI_VERSION,
      .expected_revision = revision,
      .reserved = 0,
      .lesson_id = (const uint8_t *)missing_lesson,
      .lesson_id_len = sizeof(missing_lesson) - 1,
  };
  request_id = 0;
  CHECK(ml_document_open_v1(core, &document_request, &request_id) == ML_STATUS_OK);
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.request_id == request_id);
  CHECK(event.kind == ML_EVENT_DOCUMENT_OPENED);
  CHECK(event.status == ML_STATUS_NOT_FOUND);
  char document_payload[128];
  CHECK(copy_payload(&event, document_payload, sizeof(document_payload)));
  CHECK(strcmp(document_payload, "{\"error\":\"lessonNotFound\"}") == 0);
  ml_core_release_event(core, &event);

  const ml_settings_get_request_v1 settings_request = {
      .struct_size = sizeof(settings_request),
      .abi_version = ML_ABI_VERSION,
      .reserved = 0,
  };
  request_id = 0;
  CHECK(ml_settings_get_v1(core, &settings_request, &request_id) == ML_STATUS_OK);
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.request_id == request_id);
  CHECK(event.kind == ML_EVENT_SETTINGS);
  CHECK(event.status == ML_STATUS_OK);
  char settings_payload[128];
  CHECK(copy_payload(&event, settings_payload, sizeof(settings_payload)));
  CHECK(strcmp(settings_payload, "{\"revision\":1,\"appearance\":\"light\"}") == 0);
  ml_core_release_event(core, &event);

  const ml_settings_put_appearance_request_v1 appearance_request = {
      .struct_size = sizeof(appearance_request),
      .abi_version = ML_ABI_VERSION,
      .expected_revision = 1,
      .appearance = ML_APPEARANCE_COZY,
      .reserved = 0,
  };
  request_id = 0;
  CHECK(ml_settings_put_appearance_v1(core, &appearance_request, &request_id) == ML_STATUS_OK);
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.request_id == request_id);
  CHECK(event.kind == ML_EVENT_APPEARANCE_UPDATED);
  CHECK(event.status == ML_STATUS_OK);
  CHECK(copy_payload(&event, settings_payload, sizeof(settings_payload)));
  CHECK(strcmp(settings_payload, "{\"revision\":2,\"appearance\":\"cozy\"}") == 0);
  ml_core_release_event(core, &event);

  const ml_library_stats_request_v1 stats_request = {
      .struct_size = sizeof(stats_request),
      .abi_version = ML_ABI_VERSION,
      .expected_revision = revision,
      .reserved = 0,
  };
  request_id = 0;
  CHECK(ml_library_stats_v1(core, &stats_request, &request_id) == ML_STATUS_OK);
  CHECK(request_id != 0);
  CHECK(poll_event(core, &event) == ML_STATUS_OK);
  CHECK(event.request_id == request_id);
  CHECK(event.kind == ML_EVENT_LIBRARY_STATS);
  CHECK(event.status == ML_STATUS_OK);
  CHECK(event.payload_schema_version == 1);

  char stats_payload[1024];
  CHECK(copy_payload(&event, stats_payload, sizeof(stats_payload)));
  CHECK(strstr(stats_payload, "\"totalCourses\":0") != nullptr);
  CHECK(strstr(stats_payload, "\"mediaTypes\":[]") != nullptr);
  CHECK(strstr(stats_payload, "\"topCourses\":[]") != nullptr);
  ml_core_release_event(core, &event);

  ml_core_destroy(core);
  ml_core_destroy(nullptr);
  return EXIT_SUCCESS;
}
