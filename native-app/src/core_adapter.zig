const std = @import("std");
const native_sdk = @import("native_sdk");

const c = @cImport({
    @cInclude("melearner_core.h");
});

pub const adapter_id: u32 = 1;
pub const schema_version: u32 = 1;
pub const library_page_key: u64 = 1;
pub const library_page_request_bytes: usize = 16;
pub const library_page_size: u32 = 20;

pub const Operation = enum(u32) {
    load_library_page = 1,
};

const LibraryPageRequest = struct {
    expected_revision: u64,
    offset: u64,
};

const SlotState = enum {
    free,
    queued,
    active,
};

const Slot = struct {
    state: SlotState = .free,
    sdk_request_id: u64 = 0,
    core_request_id: u64 = 0,
    expected_revision: u64 = 0,
    page_offset: u64 = 0,
    cancelled: bool = false,
    completion: ?native_sdk.ExternalEffectCompletion = null,
};

pub const CoreAdapter = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    state_dir: []u8,
    mutex: std.Io.Mutex = .init,
    wake: std.Io.Semaphore = .{},
    shutdown_requested: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    failed: bool = false,
    slots: [native_sdk.max_effects]Slot = [_]Slot{.{}} ** native_sdk.max_effects,
    worker: ?std.Thread = null,

    pub fn create(allocator: std.mem.Allocator, io: std.Io, state_dir: []const u8) !*CoreAdapter {
        const self = try allocator.create(CoreAdapter);
        errdefer allocator.destroy(self);
        const state_dir_copy = try allocator.dupe(u8, state_dir);
        errdefer allocator.free(state_dir_copy);
        self.* = .{
            .allocator = allocator,
            .io = io,
            .state_dir = state_dir_copy,
        };
        self.worker = try std.Thread.spawn(.{}, workerMain, .{self});
        return self;
    }

    pub fn destroy(self: *CoreAdapter) void {
        self.shutdown();
        const allocator = self.allocator;
        allocator.free(self.state_dir);
        allocator.destroy(self);
    }

    pub fn binding(self: *CoreAdapter) native_sdk.ExternalEffectAdapter {
        return .{
            .context = self,
            .submit_fn = submit,
            .cancel_fn = cancel,
            .shutdown_fn = shutdownErased,
        };
    }

    fn submit(context: *anyopaque, request: native_sdk.EffectExternalRequest, completion: native_sdk.ExternalEffectCompletion) anyerror!void {
        const self: *CoreAdapter = @ptrCast(@alignCast(context));
        if (request.adapter_id != adapter_id or
            request.kind != @intFromEnum(Operation.load_library_page) or
            request.schema_version != schema_version)
        {
            return error.UnsupportedCoreRequest;
        }
        const page_request = decodeLibraryPageRequest(request.payload) catch return error.UnsupportedCoreRequest;
        if (self.shutdown_requested.load(.acquire)) return error.CoreAdapterStopped;

        self.mutex.lockUncancelable(self.io);
        if (self.failed or self.shutdown_requested.load(.acquire)) {
            self.mutex.unlock(self.io);
            return error.CoreAdapterUnavailable;
        }
        const slot = for (&self.slots) |*candidate| {
            if (candidate.state == .free) break candidate;
        } else {
            self.mutex.unlock(self.io);
            return error.CoreAdapterFull;
        };
        slot.* = .{
            .state = .queued,
            .sdk_request_id = request.request_id,
            .expected_revision = page_request.expected_revision,
            .page_offset = page_request.offset,
            .completion = completion,
        };
        self.mutex.unlock(self.io);
        self.wake.post(self.io);
    }

    fn cancel(context: *anyopaque, request_id: u64) void {
        const self: *CoreAdapter = @ptrCast(@alignCast(context));
        self.mutex.lockUncancelable(self.io);
        for (&self.slots) |*slot| {
            if (slot.state != .free and slot.sdk_request_id == request_id) {
                slot.cancelled = true;
                break;
            }
        }
        self.mutex.unlock(self.io);
        self.wake.post(self.io);
    }

    fn shutdownErased(context: *anyopaque) void {
        const self: *CoreAdapter = @ptrCast(@alignCast(context));
        self.shutdown();
    }

    fn shutdown(self: *CoreAdapter) void {
        _ = self.shutdown_requested.swap(true, .acq_rel);
        self.wake.post(self.io);

        self.mutex.lockUncancelable(self.io);
        const worker = self.worker;
        self.worker = null;
        self.mutex.unlock(self.io);
        if (worker) |thread| thread.join();
    }

    fn workerMain(self: *CoreAdapter) void {
        var core: ?*c.ml_core_t = null;
        const config: c.ml_config_v2 = .{
            .struct_size = @sizeOf(c.ml_config_v2),
            .abi_version = c.ML_ABI_VERSION,
            .event_queue_capacity = 4,
            .max_event_payload_bytes = @intCast(native_sdk.max_effect_external_result_bytes),
            .state_dir = self.state_dir.ptr,
            .state_dir_len = self.state_dir.len,
        };
        if (c.ml_core_create(&config, &core) != c.ML_STATUS_OK or core == null) {
            self.failAll("The Library database could not open.");
            return;
        }
        const handle = core.?;
        defer c.ml_core_destroy(handle);
        if (c.ml_core_set_waker(handle, coreWake, self) != c.ML_STATUS_OK) {
            self.failAll("The Library service could not start.");
            return;
        }
        defer _ = c.ml_core_set_waker(handle, null, null);

        var revision: u64 = 0;
        while (!self.shutdown_requested.load(.acquire)) {
            if (self.retireCancelled(handle)) continue;
            if (revision != 0 and self.startNext(handle, revision)) continue;

            var event = emptyEvent();
            switch (c.ml_core_poll_event(handle, &event)) {
                c.ML_STATUS_OK => self.handleEvent(handle, &event, &revision),
                c.ML_STATUS_EMPTY => self.wake.waitUncancelable(self.io),
                else => {
                    self.failAll("The Library service stopped unexpectedly.");
                    self.wake.waitUncancelable(self.io);
                },
            }
        }

        self.cancelAndClear(handle);
    }

    fn coreWake(context: ?*anyopaque) callconv(.c) void {
        const self: *CoreAdapter = @ptrCast(@alignCast(context.?));
        self.wake.post(self.io);
    }

    fn retireCancelled(self: *CoreAdapter, core: *c.ml_core_t) bool {
        var core_request_id: u64 = 0;
        var found = false;
        self.mutex.lockUncancelable(self.io);
        for (&self.slots) |*slot| {
            if (slot.state != .free and slot.cancelled) {
                core_request_id = slot.core_request_id;
                slot.* = .{};
                found = true;
                break;
            }
        }
        self.mutex.unlock(self.io);
        if (!found) return false;
        if (core_request_id != 0) _ = c.ml_core_cancel(core, core_request_id);
        return true;
    }

    fn startNext(self: *CoreAdapter, core: *c.ml_core_t, revision: u64) bool {
        var completion: ?native_sdk.ExternalEffectCompletion = null;
        var rejected = false;

        self.mutex.lockUncancelable(self.io);
        for (self.slots) |slot| {
            if (slot.state == .active) {
                self.mutex.unlock(self.io);
                return false;
            }
        }
        const slot = for (&self.slots) |*candidate| {
            if (candidate.state == .queued) break candidate;
        } else {
            self.mutex.unlock(self.io);
            return false;
        };
        if (slot.cancelled) {
            slot.* = .{};
            self.mutex.unlock(self.io);
            return true;
        }

        slot.state = .active;
        const request: c.ml_library_course_page_request_v1 = .{
            .struct_size = @sizeOf(c.ml_library_course_page_request_v1),
            .abi_version = c.ML_ABI_VERSION,
            .expected_revision = if (slot.expected_revision == 0) revision else slot.expected_revision,
            .offset = slot.page_offset,
            .limit = library_page_size,
            .reserved = 0,
        };
        var core_request_id: u64 = 0;
        if (c.ml_library_course_page_v1(core, &request, &core_request_id) == c.ML_STATUS_OK and core_request_id != 0) {
            slot.core_request_id = core_request_id;
        } else {
            completion = slot.completion;
            slot.* = .{};
            rejected = true;
        }
        self.mutex.unlock(self.io);

        if (rejected) self.completeWithRetry(completion.?, .failure, "The Library is busy. Try again.");
        return true;
    }

    fn handleEvent(self: *CoreAdapter, core: *c.ml_core_t, event: *c.ml_event_v1, revision: *u64) void {
        defer c.ml_core_release_event(core, event);
        const payload = eventPayload(event);

        if (event.kind == c.ML_EVENT_CORE_READY) {
            if (event.status != c.ML_STATUS_OK or event.payload_schema_version != 1) {
                self.failAll("The Library service stopped unexpectedly.");
                return;
            }
            revision.* = std.fmt.parseInt(u64, payload, 10) catch {
                self.failAll("The Library service stopped unexpectedly.");
                return;
            };
            if (revision.* == 0) self.failAll("The Library service stopped unexpectedly.");
            return;
        }
        if (event.kind == c.ML_EVENT_FATAL) {
            self.failAll("The Library service stopped unexpectedly.");
            return;
        }

        const completion = self.takeCompletion(event.request_id) orelse return;
        if (event.kind == c.ML_EVENT_REQUEST_CANCELLED) return;
        if (event.kind != c.ML_EVENT_LIBRARY_COURSE_PAGE or event.payload_schema_version != 1) {
            self.completeWithRetry(completion, .failure, "The Library service returned an unexpected response.");
            return;
        }
        if (event.status == c.ML_STATUS_OK) {
            self.completeWithRetry(completion, .success, payload);
        } else {
            self.completeWithRetry(completion, .failure, requestFailureMessage(event.status));
        }
    }

    fn takeCompletion(self: *CoreAdapter, core_request_id: u64) ?native_sdk.ExternalEffectCompletion {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        for (&self.slots) |*slot| {
            if (slot.state == .active and slot.core_request_id == core_request_id) {
                const completion = if (slot.cancelled) null else slot.completion;
                slot.* = .{};
                return completion;
            }
        }
        return null;
    }

    fn completeWithRetry(self: *CoreAdapter, completion: native_sdk.ExternalEffectCompletion, outcome: native_sdk.ExternalEffectAdapterOutcome, bytes: []const u8) void {
        var current_outcome = outcome;
        var current_bytes = bytes;
        while (!self.shutdown_requested.load(.acquire)) {
            completion.complete(current_outcome, current_bytes) catch |err| switch (err) {
                error.ExternalEffectQueueFull => {
                    std.Io.sleep(self.io, std.Io.Duration.fromMilliseconds(1), .awake) catch {};
                    continue;
                },
                error.ExternalEffectResultTooLarge => {
                    current_outcome = .failure;
                    current_bytes = "The Library response was too large.";
                    continue;
                },
                error.ExternalEffectStaleResult, error.ExternalEffectDuplicateResult => return,
            };
            return;
        }
    }

    fn failAll(self: *CoreAdapter, message: []const u8) void {
        var completions: [native_sdk.max_effects]native_sdk.ExternalEffectCompletion = undefined;
        var completion_count: usize = 0;
        self.mutex.lockUncancelable(self.io);
        self.failed = true;
        for (&self.slots) |*slot| {
            if (slot.state != .free and !slot.cancelled) {
                completions[completion_count] = slot.completion.?;
                completion_count += 1;
            }
            slot.* = .{};
        }
        self.mutex.unlock(self.io);
        for (completions[0..completion_count]) |completion| {
            self.completeWithRetry(completion, .failure, message);
        }
    }

    fn cancelAndClear(self: *CoreAdapter, core: *c.ml_core_t) void {
        var core_request_ids: [native_sdk.max_effects]u64 = undefined;
        var request_count: usize = 0;
        self.mutex.lockUncancelable(self.io);
        for (&self.slots) |*slot| {
            if (slot.core_request_id != 0) {
                core_request_ids[request_count] = slot.core_request_id;
                request_count += 1;
            }
            slot.* = .{};
        }
        self.mutex.unlock(self.io);
        for (core_request_ids[0..request_count]) |request_id| {
            _ = c.ml_core_cancel(core, request_id);
        }
    }
};

pub fn encodeLibraryPageRequest(buffer: *[library_page_request_bytes]u8, expected_revision: u64, offset: u64) []const u8 {
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], offset, .little);
    return buffer;
}

fn decodeLibraryPageRequest(payload: []const u8) !LibraryPageRequest {
    if (payload.len != library_page_request_bytes) return error.InvalidLibraryPageRequest;
    const request: LibraryPageRequest = .{
        .expected_revision = std.mem.readInt(u64, payload[0..8], .little),
        .offset = std.mem.readInt(u64, payload[8..16], .little),
    };
    if (request.offset > std.math.maxInt(i64) or
        request.offset % library_page_size != 0 or
        (request.expected_revision == 0 and request.offset != 0))
    {
        return error.InvalidLibraryPageRequest;
    }
    return request;
}

fn emptyEvent() c.ml_event_v1 {
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

fn eventPayload(event: *const c.ml_event_v1) []const u8 {
    if (event.payload == null or event.payload_len == 0) return "";
    return event.payload[0..event.payload_len];
}

fn requestFailureMessage(status: c.ml_status_t) []const u8 {
    return switch (status) {
        c.ML_STATUS_STALE => "The Library changed while this page was opening.",
        c.ML_STATUS_CANCELLED => "Opening the Library was cancelled.",
        c.ML_STATUS_BUSY => "The Library is busy. Try again.",
        c.ML_STATUS_PANIC => "The Library service stopped unexpectedly.",
        else => "The Library database could not be read.",
    };
}

test "Library page requests use one strict revision and offset wire format" {
    var payload: [library_page_request_bytes]u8 = undefined;
    _ = encodeLibraryPageRequest(&payload, 7, 20);
    const decoded = try decodeLibraryPageRequest(&payload);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);
    try std.testing.expectEqual(@as(u64, 20), decoded.offset);

    _ = encodeLibraryPageRequest(&payload, 0, 20);
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeLibraryPageRequest(&payload));

    _ = encodeLibraryPageRequest(&payload, 7, 1);
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeLibraryPageRequest(&payload));
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeLibraryPageRequest(payload[0..15]));
}
