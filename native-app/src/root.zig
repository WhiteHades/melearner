const std = @import("std");
const core_adapter = @import("core_adapter.zig");
const native_sdk = @import("native_sdk");

pub const max_path_bytes = native_sdk.platform.max_dialog_path_bytes;
pub const max_message_bytes: usize = 256;
pub const max_warnings: usize = 8;

pub const State = enum {
    loading,
    required,
    ready,
    failed,
};

pub const Validation = enum {
    idle,
    picking,
    scanning,
    cancelling,
    committing,
    failed,
};

pub const ScanPhase = enum(u32) {
    none = 0,
    discovering = 1,
    classifying = 2,
    reconciling = 3,
    committing = 4,
    writing_markers = 5,
};

pub const Warning = struct {
    storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    len: usize = 0,

    pub fn text(warning: *const Warning) []const u8 {
        return warning.storage[0..warning.len];
    }
};

pub const Model = struct {
    state: State = .loading,
    validation: Validation = .idle,
    first_run: bool = false,
    committed_storage: [max_path_bytes]u8 = [_]u8{0} ** max_path_bytes,
    committed_len: usize = 0,
    candidate_storage: [max_path_bytes]u8 = [_]u8{0} ** max_path_bytes,
    candidate_len: usize = 0,
    state_request_id: u64 = 0,
    scan_request_id: u64 = 0,
    scan_cancel_request_id: u64 = 0,
    scan_progress_request_id: u64 = 0,
    scan_phase: ScanPhase = .none,
    scan_processed: u64 = 0,
    scan_discovered: u64 = 0,
    scan_total: u64 = 0,
    scan_total_known: bool = false,
    scan_cancellable: bool = true,
    scan_course_count: u64 = 0,
    warnings: [max_warnings]Warning = [_]Warning{.{}} ** max_warnings,
    warning_count: usize = 0,
    warning_total: u64 = 0,
    warnings_omitted: u64 = 0,
    message_storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    message_len: usize = 0,

    pub fn committed(model: *const Model) []const u8 {
        return model.committed_storage[0..model.committed_len];
    }

    pub fn candidate(model: *const Model) []const u8 {
        return model.candidate_storage[0..model.candidate_len];
    }

    pub fn message(model: *const Model) []const u8 {
        return model.message_storage[0..model.message_len];
    }

    pub fn progressLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        const phase = switch (model.scan_phase) {
            .none => "Starting",
            .discovering => "Discovering files",
            .classifying => "Classifying learning items",
            .reconciling => "Reconciling Courses",
            .committing => "Saving the Library",
            .writing_markers => "Writing Course markers",
        };
        if (model.scan_total_known) {
            return std.fmt.allocPrint(
                arena,
                "{s}: {d} of {d}",
                .{ phase, model.scan_processed, model.scan_total },
            ) catch phase;
        }
        if (model.scan_discovered != 0) {
            return std.fmt.allocPrint(
                arena,
                "{s}: {d} discovered",
                .{ phase, model.scan_discovered },
            ) catch phase;
        }
        return phase;
    }

    pub fn applyScanProgress(model: *Model, bytes: []const u8) !void {
        if (bytes.len != core_adapter.scan_progress_result_bytes or
            bytes[4] > 1 or
            bytes[5] > 1 or
            bytes[6] != 0 or
            bytes[7] != 0)
        {
            return error.InvalidScanProgress;
        }
        const phase: ScanPhase = switch (std.mem.readInt(u32, bytes[0..4], .little)) {
            1 => .discovering,
            2 => .classifying,
            3 => .reconciling,
            4 => .committing,
            5 => .writing_markers,
            else => return error.InvalidScanProgress,
        };
        const processed = std.mem.readInt(u64, bytes[8..16], .little);
        const discovered = std.mem.readInt(u64, bytes[16..24], .little);
        const total = std.mem.readInt(u64, bytes[24..32], .little);
        const total_known = bytes[4] == 1;
        const cancellable = bytes[5] == 1;
        if ((total_known and (processed > total or discovered > total)) or
            (!total_known and total != 0) or
            ((phase == .committing or phase == .writing_markers) and cancellable))
        {
            return error.InvalidScanProgress;
        }
        model.scan_phase = phase;
        model.scan_processed = processed;
        model.scan_discovered = discovered;
        model.scan_total = total;
        model.scan_total_known = total_known;
        model.scan_cancellable = cancellable;
        if (phase == .committing or phase == .writing_markers) {
            model.validation = .committing;
        }
    }

    pub fn setCandidate(model: *Model, path: []const u8) !void {
        if (!validPath(path)) return error.InvalidRootPath;
        @memcpy(model.candidate_storage[0..path.len], path);
        model.candidate_len = path.len;
    }

    pub fn loadScanResult(model: *Model, bytes: []const u8, previous_revision: u64) !u64 {
        const Payload = struct {
            revision: u64,
            courseCount: u64,
            warnings: []const []const u8,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
        defer parsed.deinit();
        if (parsed.value.revision <= previous_revision) {
            return error.InvalidScanResult;
        }

        model.scan_course_count = parsed.value.courseCount;
        model.warning_total = @intCast(parsed.value.warnings.len);
        model.warning_count = @min(parsed.value.warnings.len, max_warnings);
        model.warnings_omitted = @intCast(parsed.value.warnings.len - model.warning_count);
        for (parsed.value.warnings[0..model.warning_count], 0..) |warning, index| {
            const value = if (warning.len == 0 or warning.len > max_message_bytes)
                "Scan warning details exceeded the display bound."
            else
                warning;
            if (value.ptr != warning.ptr) model.warnings_omitted += 1;
            model.warnings[index] = .{ .len = value.len };
            @memcpy(model.warnings[index].storage[0..value.len], value);
        }
        model.scan_request_id = 0;
        model.scan_cancel_request_id = 0;
        model.scan_progress_request_id = 0;
        model.scan_phase = .none;
        model.validation = .idle;
        model.scan_phase = .none;
        model.message_len = 0;
        return parsed.value.revision;
    }

    pub fn loadState(model: *Model, bytes: []const u8) !u64 {
        const Payload = struct {
            revision: u64,
            rootPath: ?[]const u8,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
        defer parsed.deinit();
        if (parsed.value.revision == 0) return error.InvalidRootState;
        if (parsed.value.rootPath) |path| {
            if (!validPath(path)) return error.InvalidRootState;
        }

        model.validation = .idle;
        model.candidate_len = 0;
        model.message_len = 0;
        if (parsed.value.rootPath) |path| {
            @memcpy(model.committed_storage[0..path.len], path);
            model.committed_len = path.len;
            model.first_run = false;
            model.state = .ready;
        } else {
            model.committed_len = 0;
            model.first_run = true;
            model.state = .required;
        }
        return parsed.value.revision;
    }

    pub fn setFailure(model: *Model, detail: []const u8) void {
        const value = if (detail.len == 0 or
            detail.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(detail))
            "The Library root could not be loaded."
        else
            detail;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
        model.state_request_id = 0;
        model.state = .failed;
    }

    pub fn setScanFailure(model: *Model, detail: []const u8) void {
        const value = if (detail.len == 0 or
            detail.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(detail))
            "The Library root could not be scanned."
        else
            detail;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
        model.scan_request_id = 0;
        model.scan_cancel_request_id = 0;
        model.scan_progress_request_id = 0;
        model.scan_phase = .none;
        model.validation = .failed;
    }
};

fn validPath(path: []const u8) bool {
    return path.len != 0 and
        path.len <= max_path_bytes and
        std.unicode.utf8ValidateSlice(path) and
        std.mem.indexOfScalar(u8, path, 0) == null;
}

test "root state distinguishes first run from one bounded committed root" {
    var model = Model{};
    try std.testing.expectEqual(@as(u64, 7), try model.loadState(
        \\{"revision":7,"rootPath":null}
    ));
    try std.testing.expectEqual(State.required, model.state);
    try std.testing.expect(model.first_run);
    try std.testing.expectEqualStrings("", model.committed());

    try std.testing.expectEqual(@as(u64, 8), try model.loadState(
        \\{"revision":8,"rootPath":"/courses/Δ"}
    ));
    try std.testing.expectEqual(State.ready, model.state);
    try std.testing.expect(!model.first_run);
    try std.testing.expectEqualStrings("/courses/Δ", model.committed());
}

test "root state rejects schema drift and unsafe paths without changing committed state" {
    var model = Model{};
    _ = try model.loadState(
        \\{"revision":7,"rootPath":"/courses"}
    );
    const before = model;

    try std.testing.expectError(error.UnknownField, model.loadState(
        \\{"revision":8,"rootPath":"/new","extra":true}
    ));
    try std.testing.expectEqualDeep(before, model);
    try std.testing.expectError(error.InvalidRootState, model.loadState(
        \\{"revision":8,"rootPath":""}
    ));
    try std.testing.expectEqualDeep(before, model);
}

test "scan results advance once and copy bounded warnings" {
    var model = Model{};
    try model.setCandidate("/courses");
    model.validation = .scanning;
    model.scan_request_id = 9;
    const revision = try model.loadScanResult(
        \\{"revision":8,"courseCount":2,"warnings":["Skipped one marker."]}
    , 7);
    try std.testing.expectEqual(@as(u64, 8), revision);
    try std.testing.expectEqual(@as(u64, 2), model.scan_course_count);
    try std.testing.expectEqual(@as(usize, 1), model.warning_count);
    try std.testing.expectEqualStrings("Skipped one marker.", model.warnings[0].text());
    try std.testing.expectEqual(Validation.idle, model.validation);

    try std.testing.expectError(error.InvalidScanResult, model.loadScanResult(
        \\{"revision":8,"courseCount":2,"warnings":[]}
    , 8));
}

test "scan progress snapshots are strict and commit disables cancellation" {
    var model = Model{ .validation = .scanning };
    var bytes: [core_adapter.scan_progress_result_bytes]u8 = [_]u8{0} ** core_adapter.scan_progress_result_bytes;
    std.mem.writeInt(u32, bytes[0..4], 2, .little);
    bytes[4] = 1;
    bytes[5] = 1;
    std.mem.writeInt(u64, bytes[8..16], 4, .little);
    std.mem.writeInt(u64, bytes[16..24], 10, .little);
    std.mem.writeInt(u64, bytes[24..32], 10, .little);
    try model.applyScanProgress(&bytes);
    try std.testing.expectEqual(ScanPhase.classifying, model.scan_phase);
    try std.testing.expect(model.scan_cancellable);

    std.mem.writeInt(u32, bytes[0..4], 4, .little);
    bytes[5] = 0;
    try model.applyScanProgress(&bytes);
    try std.testing.expectEqual(Validation.committing, model.validation);
    try std.testing.expect(!model.scan_cancellable);

    bytes[5] = 1;
    try std.testing.expectError(error.InvalidScanProgress, model.applyScanProgress(&bytes));
}
