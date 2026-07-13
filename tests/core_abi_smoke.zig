const c = @cImport({
    @cInclude("melearner_core.h");
});

comptime {
    if (c.ML_ABI_VERSION != 1) @compileError("unexpected core ABI version");
    if (@sizeOf(c.ml_config_v1) != 16) @compileError("ml_config_v1 layout drift");
    if (@sizeOf(c.ml_core_limits_v1) != 16) @compileError("ml_core_limits_v1 layout drift");
    if (@offsetOf(c.ml_event_v1, "sequence") != 8) @compileError("ml_event_v1 sequence offset drift");
    if (@offsetOf(c.ml_event_v1, "payload") != 40) @compileError("ml_event_v1 payload offset drift");
    if (@offsetOf(c.ml_event_v1, "payload_len") != 40 + @sizeOf(usize)) {
        @compileError("ml_event_v1 payload length offset drift");
    }
}

pub fn main() !void {
    var config = c.ml_config_v1{
        .struct_size = @sizeOf(c.ml_config_v1),
        .abi_version = c.ML_ABI_VERSION,
        .event_queue_capacity = 4,
        .max_event_payload_bytes = 1024,
    };
    var core: ?*c.ml_core_t = null;
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_create(&config, &core));
    try expect(core != null);
    defer c.ml_core_destroy(core);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_set_waker(core, null, null));

    var limits = c.ml_core_limits_v1{
        .struct_size = @sizeOf(c.ml_core_limits_v1),
        .abi_version = c.ML_ABI_VERSION,
        .event_queue_capacity = 0,
        .max_event_payload_bytes = 0,
    };
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_get_limits_v1(core, &limits));
    try expectEqual(@as(u32, 4), limits.event_queue_capacity);
    try expectEqual(@as(u32, 1024), limits.max_event_payload_bytes);

    var event = c.ml_event_v1{
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
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_OK), c.ml_core_poll_event(core, &event));
    try expect(event.sequence > 0);
    try expectEqual(@as(c.ml_event_kind_t, c.ML_EVENT_CORE_READY), event.kind);
    try expect(event.payload != null);
    try expectEqual(@as(usize, 1), event.payload_len);
    try expectEqual(@as(u8, '1'), event.payload[0]);

    c.ml_core_release_event(core, &event);
    try expectEqual(@as(u64, 0), event.sequence);
    try expectEqual(@as(c.ml_status_t, c.ML_STATUS_EMPTY), c.ml_core_poll_event(core, &event));
}

fn expect(condition: bool) !void {
    if (!condition) return error.CoreAbiSmokeFailed;
}

fn expectEqual(expected: anytype, actual: @TypeOf(expected)) !void {
    if (actual != expected) return error.CoreAbiSmokeFailed;
}
