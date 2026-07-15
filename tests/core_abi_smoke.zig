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
    if (@sizeOf(c.ml_library_scan_request_v1) != 16 + @sizeOf(usize) * 2) {
        @compileError("scan request layout drift");
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
