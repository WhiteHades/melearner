const std = @import("std");

const c = @cImport({
    @cInclude("melearner_core.h");
});

const max_poll_attempts = 2_000;

comptime {
    if (c.ML_ABI_VERSION != 2) @compileError("unexpected core ABI version");
    if (@sizeOf(c.ml_config_v2) != 16 + @sizeOf(usize) * 2) {
        @compileError("ml_config_v2 layout drift");
    }
    if (@offsetOf(c.ml_config_v2, "state_dir") != 16) {
        @compileError("ml_config_v2 state directory offset drift");
    }
    if (@sizeOf(c.ml_library_course_page_request_v1) != 32) {
        @compileError("course page request layout drift");
    }
    if (@sizeOf(c.ml_library_stats_request_v1) != 24 or
        @offsetOf(c.ml_library_stats_request_v1, "expected_revision") != 8 or
        @offsetOf(c.ml_library_stats_request_v1, "reserved") != 16)
    {
        @compileError("Library stats request layout drift");
    }
    if (@sizeOf(c.ml_library_scan_request_v1) != 16 + @sizeOf(usize) * 2) {
        @compileError("scan request layout drift");
    }
    if (@sizeOf(c.ml_progress_put_request_v1) != 40 + @sizeOf(usize) * 2) {
        @compileError("progress request layout drift");
    }
    if (@sizeOf(c.ml_course_access_request_v1) != 24 + @sizeOf(usize) * 2 or
        @offsetOf(c.ml_course_access_request_v1, "reserved") != 16 or
        @offsetOf(c.ml_course_access_request_v1, "course_id") != 24 or
        @offsetOf(c.ml_course_access_request_v1, "course_id_len") != 24 + @sizeOf(usize))
    {
        @compileError("Course access request layout drift");
    }
    if (@offsetOf(c.ml_activity_day_page_request_v1, "expected_revision") != 8) {
        @compileError("activity page request layout drift");
    }
    if (@sizeOf(c.ml_search_rebuild_request_v1) != 24) {
        @compileError("search rebuild request layout drift");
    }
    if (@offsetOf(c.ml_search_rebuild_request_v1, "reserved") != 16) {
        @compileError("search rebuild reserved offset drift");
    }
    if (@sizeOf(c.ml_search_query_request_v1) != 40 + @sizeOf(usize) * 2) {
        @compileError("search query request layout drift");
    }
    if (@offsetOf(c.ml_search_query_request_v1, "expected_index_revision") != 8 or
        @offsetOf(c.ml_search_query_request_v1, "query_id") != 16 or
        @offsetOf(c.ml_search_query_request_v1, "offset") != 24 or
        @offsetOf(c.ml_search_query_request_v1, "limit") != 32 or
        @offsetOf(c.ml_search_query_request_v1, "reserved") != 36 or
        @offsetOf(c.ml_search_query_request_v1, "query") != 40 or
        @offsetOf(c.ml_search_query_request_v1, "query_len") != 40 + @sizeOf(usize))
    {
        @compileError("search query request field drift");
    }
    if (c.ML_MAX_SEARCH_QUERY_BYTES != 64 * 1024) {
        @compileError("unexpected search query bound");
    }
    if (@sizeOf(c.ml_notes_list_request_v1) != 32 + @sizeOf(usize) * 2 or
        @offsetOf(c.ml_notes_list_request_v1, "lesson_id") != 32 or
        @offsetOf(c.ml_notes_list_request_v1, "lesson_id_len") != 32 + @sizeOf(usize))
    {
        @compileError("notes list request layout drift");
    }
    if (@sizeOf(c.ml_notes_save_request_v1) != 32 + @sizeOf(usize) * 6 or
        @offsetOf(c.ml_notes_save_request_v1, "timestamp") != 16 or
        @offsetOf(c.ml_notes_save_request_v1, "reserved") != 24 or
        @offsetOf(c.ml_notes_save_request_v1, "lesson_id") != 32 or
        @offsetOf(c.ml_notes_save_request_v1, "note_id") != 32 + @sizeOf(usize) * 2 or
        @offsetOf(c.ml_notes_save_request_v1, "text") != 32 + @sizeOf(usize) * 4 or
        @offsetOf(c.ml_notes_save_request_v1, "text_len") != 32 + @sizeOf(usize) * 5)
    {
        @compileError("notes save request layout drift");
    }
    if (@sizeOf(c.ml_notes_delete_request_v1) != 24 + @sizeOf(usize) * 2 or
        @offsetOf(c.ml_notes_delete_request_v1, "note_id") != 24 or
        @offsetOf(c.ml_notes_delete_request_v1, "note_id_len") != 24 + @sizeOf(usize))
    {
        @compileError("notes delete request layout drift");
    }
    if (c.ML_MAX_NOTE_TEXT_BYTES != 8 * 1024) {
        @compileError("unexpected note text bound");
    }
    if (@sizeOf(c.ml_core_limits_v1) != 16) @compileError("ml_core_limits_v1 layout drift");
    if (@offsetOf(c.ml_event_v1, "sequence") != 8) @compileError("ml_event_v1 sequence offset drift");
    if (@offsetOf(c.ml_event_v1, "payload") != 40) @compileError("ml_event_v1 payload offset drift");
    if (@offsetOf(c.ml_event_v1, "payload_len") != 40 + @sizeOf(usize)) {
        @compileError("ml_event_v1 payload length offset drift");
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const state_dir = try temporaryStateDirectory(init);
    defer init.gpa.free(state_dir);
    defer std.Io.Dir.cwd().deleteTree(io, state_dir) catch {};

    var config = c.ml_config_v2{
        .struct_size = @sizeOf(c.ml_config_v2),
        .abi_version = c.ML_ABI_VERSION,
        .event_queue_capacity = 4,
        .max_event_payload_bytes = 4096,
        .state_dir = state_dir.ptr,
        .state_dir_len = state_dir.len,
    };
    var core: ?*c.ml_core_t = null;
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_create(&config, &core));
    try expect(core != null);
    defer c.ml_core_destroy(core);
    try expectEqual(@as(u32, c.ML_ABI_VERSION), c.ml_abi_version());
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_set_waker(core, null, null));

    var limits = c.ml_core_limits_v1{
        .struct_size = @sizeOf(c.ml_core_limits_v1),
        .abi_version = c.ML_ABI_VERSION,
        .event_queue_capacity = 0,
        .max_event_payload_bytes = 0,
    };
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_get_limits_v1(core, &limits));
    try expectEqual(@as(u32, 4), limits.event_queue_capacity);
    try expectEqual(@as(u32, 4096), limits.max_event_payload_bytes);

    var ready = try pollEvent(io, core);
    try expect(ready.sequence > 0);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_CORE_READY), ready.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), ready.status);
    try expectEqual(@as(u32, 1), ready.payload_schema_version);
    try expect(ready.payload != null);
    var revision = try std.fmt.parseInt(u64, ready.payload[0..ready.payload_len], 10);
    try expect(revision != 0);
    const initial_revision = revision;
    c.ml_core_release_event(core, &ready);

    var scan_request = c.ml_library_scan_request_v1{
        .struct_size = @sizeOf(c.ml_library_scan_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .root_path = state_dir.ptr,
        .root_path_len = state_dir.len,
    };
    var scan_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_library_scan_v1(core, &scan_request, &scan_request_id),
    );
    try expect(scan_request_id != 0);

    var scanned = try pollEvent(io, core);
    try expectEqual(scan_request_id, scanned.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_LIBRARY_SCAN), scanned.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), scanned.status);
    try expectEqual(@as(u32, 1), scanned.payload_schema_version);
    try expect(scanned.payload != null);
    const scan_payload = scanned.payload[0..scanned.payload_len];
    const scan_prefix = "{\"revision\":";
    const scan_suffix = ",\"courseCount\":0,\"warnings\":[]}";
    try expect(std.mem.startsWith(u8, scan_payload, scan_prefix));
    try expect(std.mem.endsWith(u8, scan_payload, scan_suffix));
    revision = try std.fmt.parseInt(
        u64,
        scan_payload[scan_prefix.len .. scan_payload.len - scan_suffix.len],
        10,
    );
    try expect(revision > initial_revision);
    c.ml_core_release_event(core, &scanned);

    const missing_course_id = "missing-course";
    var access_request = c.ml_course_access_request_v1{
        .struct_size = @sizeOf(c.ml_course_access_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .reserved = 0,
        .course_id = missing_course_id.ptr,
        .course_id_len = missing_course_id.len,
    };
    var access_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_course_access_v1(core, &access_request, &access_request_id),
    );
    try expect(access_request_id != 0);

    var accessed = try pollEvent(io, core);
    try expectEqual(access_request_id, accessed.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_COURSE_ACCESSED), accessed.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_NOT_FOUND), accessed.status);
    try expectPayload(&accessed, "{\"error\":\"courseNotFound\"}");
    c.ml_core_release_event(core, &accessed);

    var rebuild_request = c.ml_search_rebuild_request_v1{
        .struct_size = @sizeOf(c.ml_search_rebuild_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .reserved = 0,
    };
    var rebuild_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_search_rebuild_v1(core, &rebuild_request, &rebuild_request_id),
    );
    try expect(rebuild_request_id != 0);

    var rebuilt = try pollEvent(io, core);
    try expectEqual(rebuild_request_id, rebuilt.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_SEARCH_INDEX_READY), rebuilt.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), rebuilt.status);
    const expected_rebuild = try std.fmt.allocPrint(
        init.gpa,
        "{{\"indexRevision\":{d},\"entryCount\":0}}",
        .{revision},
    );
    defer init.gpa.free(expected_rebuild);
    try expectPayload(&rebuilt, expected_rebuild);
    c.ml_core_release_event(core, &rebuilt);

    const search_text = "   ";
    var search_request = c.ml_search_query_request_v1{
        .struct_size = @sizeOf(c.ml_search_query_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_index_revision = revision,
        .query_id = 7,
        .offset = 0,
        .limit = 20,
        .reserved = 0,
        .query = search_text.ptr,
        .query_len = search_text.len,
    };
    var search_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_search_query_v1(core, &search_request, &search_request_id),
    );
    try expect(search_request_id != 0);

    var searched = try pollEvent(io, core);
    try expectEqual(search_request_id, searched.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_SEARCH_PAGE), searched.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), searched.status);
    const expected_search = try std.fmt.allocPrint(
        init.gpa,
        "{{\"queryId\":7,\"indexRevision\":{d},\"offset\":0,\"total\":0,\"rows\":[]}}",
        .{revision},
    );
    defer init.gpa.free(expected_search);
    try expectPayload(&searched, expected_search);
    c.ml_core_release_event(core, &searched);

    const notes_lesson_id = "missing-lesson";
    var notes_request = c.ml_notes_list_request_v1{
        .struct_size = @sizeOf(c.ml_notes_list_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .offset = 0,
        .limit = 20,
        .reserved = 0,
        .lesson_id = notes_lesson_id.ptr,
        .lesson_id_len = notes_lesson_id.len,
    };
    var notes_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_notes_list_v1(core, &notes_request, &notes_request_id),
    );
    try expect(notes_request_id != 0);

    var notes = try pollEvent(io, core);
    try expectEqual(notes_request_id, notes.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_NOTES_PAGE), notes.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), notes.status);
    const expected_notes = try std.fmt.allocPrint(
        init.gpa,
        "{{\"revision\":{d},\"lessonId\":\"missing-lesson\",\"offset\":0,\"total\":0,\"rows\":[]}}",
        .{revision},
    );
    defer init.gpa.free(expected_notes);
    try expectPayload(&notes, expected_notes);
    c.ml_core_release_event(core, &notes);

    var activity_request = c.ml_activity_day_page_request_v1{
        .struct_size = @sizeOf(c.ml_activity_day_page_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .offset = 0,
        .lookback_days = 84,
        .limit = 20,
        .reserved = 0,
    };
    var activity_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_activity_day_page_v1(core, &activity_request, &activity_request_id),
    );
    try expect(activity_request_id != 0);

    var activity = try pollEvent(io, core);
    try expectEqual(activity_request_id, activity.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_ACTIVITY_DAY_PAGE), activity.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), activity.status);
    const expected_activity = try std.fmt.allocPrint(
        init.gpa,
        "{{\"revision\":{d},\"offset\":0,\"total\":0,\"rows\":[]}}",
        .{revision},
    );
    defer init.gpa.free(expected_activity);
    try expectPayload(&activity, expected_activity);
    c.ml_core_release_event(core, &activity);

    var stats_request = c.ml_library_stats_request_v1{
        .struct_size = @sizeOf(c.ml_library_stats_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .reserved = 0,
    };
    var stats_request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_library_stats_v1(core, &stats_request, &stats_request_id),
    );
    try expect(stats_request_id != 0);

    var stats = try pollEvent(io, core);
    try expectEqual(stats_request_id, stats.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_LIBRARY_STATS), stats.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), stats.status);
    const expected_stats = try std.fmt.allocPrint(
        init.gpa,
        "{{\"revision\":{d},\"totalCourses\":0,\"availableCourses\":0,\"missingCourses\":0,\"sections\":0,\"lessons\":0,\"completedLessons\":0,\"completionPercent\":0,\"bytes\":0,\"watchedSeconds\":0,\"totalSeconds\":0,\"mediaTypes\":[],\"topCourses\":[]}}",
        .{revision},
    );
    defer init.gpa.free(expected_stats);
    try expectPayload(&stats, expected_stats);
    c.ml_core_release_event(core, &stats);

    var request = c.ml_library_course_page_request_v1{
        .struct_size = @sizeOf(c.ml_library_course_page_request_v1),
        .abi_version = c.ML_ABI_VERSION,
        .expected_revision = revision,
        .offset = 0,
        .limit = 20,
        .reserved = 0,
    };
    var request_id: u64 = 0;
    try expectEqual(
        @as(c.ml_status_t, c.ML_STATUS_OK),
        c.ml_library_course_page_v1(core, &request, &request_id),
    );
    try expect(request_id != 0);

    var completed = try pollEvent(io, core);
    try expectEqual(request_id, completed.request_id);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_LIBRARY_COURSE_PAGE), completed.kind);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), completed.status);
    try expectEqual(@as(u32, 1), completed.payload_schema_version);
    const expected_page = try std.fmt.allocPrint(
        init.gpa,
        "{{\"revision\":{d},\"offset\":0,\"total\":0,\"rows\":[]}}",
        .{revision},
    );
    defer init.gpa.free(expected_page);
    try expectPayload(&completed, expected_page);
    c.ml_core_release_event(core, &completed);

    var empty = event();
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_EMPTY), c.ml_core_poll_event(core, &empty));
}

fn temporaryStateDirectory(init: std.process.Init) ![]u8 {
    var random_bytes: [16]u8 = undefined;
    std.Io.random(init.io, &random_bytes);
    const suffix = std.fmt.bytesToHex(random_bytes, .lower);
    const temporary_root = init.environ_map.get("TMPDIR") orelse "/tmp";
    const state_dir = try std.fmt.allocPrint(
        init.gpa,
        "{s}/melearner-core-abi-{s}",
        .{ temporary_root, suffix },
    );
    errdefer init.gpa.free(state_dir);
    try std.Io.Dir.cwd().createDir(init.io, state_dir, .default_dir);
    return state_dir;
}

fn pollEvent(io: std.Io, core: ?*c.ml_core_t) !c.ml_event_v1 {
    for (0..max_poll_attempts) |_| {
        var next = event();
        switch (c.ml_core_poll_event(core, &next)) {
            c.ML_STATUS_OK => return next,
            c.ML_STATUS_EMPTY => try std.Io.sleep(
                io,
                std.Io.Duration.fromMilliseconds(1),
                .awake,
            ),
            else => return error.CoreAbiSmokeFailed,
        }
    }
    return error.CoreAbiSmokeTimedOut;
}

fn event() c.ml_event_v1 {
    return .{
        .struct_size = @sizeOf(c.ml_event_v1),
        .abi_version = c.ML_ABI_VERSION,
        .sequence = 0,
        .request_id = 0,
        .kind = 0,
        .status = 0,
        .payload_schema_version = 0,
        .reserved = 0,
        .payload = null,
        .payload_len = 0,
    };
}

fn expectPayload(event_value: *const c.ml_event_v1, expected: []const u8) !void {
    try expect(event_value.payload != null);
    try expectEqual(expected.len, event_value.payload_len);
    try expect(std.mem.eql(u8, expected, event_value.payload[0..event_value.payload_len]));
}

fn expect(condition: bool) !void {
    if (!condition) return error.CoreAbiSmokeFailed;
}

fn expectEqual(expected: anytype, actual: @TypeOf(expected)) !void {
    if (actual != expected) return error.CoreAbiSmokeFailed;
}
