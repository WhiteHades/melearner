const std = @import("std");
const native_sdk = @import("native_sdk");

const c = @cImport({
    @cInclude("melearner_core.h");
});

pub const adapter_id: u32 = 1;
pub const schema_version: u32 = 1;
pub const library_page_key: u64 = 1;
pub const course_access_key: u64 = 2;
pub const lesson_page_key: u64 = 3;
pub const library_page_request_bytes: usize = 16;
pub const course_access_request_header_bytes: usize = 8;
pub const lesson_page_request_header_bytes: usize = 16;
pub const library_page_size: u32 = 20;
pub const lesson_page_size: u32 = 20;
pub const max_course_id_bytes: usize = 128;

pub const Operation = enum(u32) {
    load_library_page = 1,
    access_course = 2,
    load_lesson_page = 3,
};

const Request = struct {
    operation: Operation,
    expected_revision: u64,
    page_offset: u64 = 0,
    course_id: []const u8 = "",
};

const SlotState = enum {
    free,
    queued,
    active,
    cancelling,
};

const Slot = struct {
    state: SlotState = .free,
    operation: Operation = .load_library_page,
    sdk_request_id: u64 = 0,
    core_request_id: u64 = 0,
    expected_revision: u64 = 0,
    page_offset: u64 = 0,
    course_id_storage: [max_course_id_bytes]u8 = [_]u8{0} ** max_course_id_bytes,
    course_id_len: usize = 0,
    cancelled: bool = false,
    completion: ?native_sdk.ExternalEffectCompletion = null,

    fn courseId(slot: *const Slot) []const u8 {
        return slot.course_id_storage[0..slot.course_id_len];
    }
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
        if (request.adapter_id != adapter_id or request.schema_version != schema_version) {
            return error.UnsupportedCoreRequest;
        }
        const operation: Operation = switch (request.kind) {
            @intFromEnum(Operation.load_library_page) => .load_library_page,
            @intFromEnum(Operation.access_course) => .access_course,
            @intFromEnum(Operation.load_lesson_page) => .load_lesson_page,
            else => return error.UnsupportedCoreRequest,
        };
        const decoded = decodeRequest(operation, request.payload) catch return error.UnsupportedCoreRequest;
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
            .operation = decoded.operation,
            .sdk_request_id = request.request_id,
            .expected_revision = decoded.expected_revision,
            .page_offset = decoded.page_offset,
            .course_id_len = decoded.course_id.len,
            .completion = completion,
        };
        @memcpy(slot.course_id_storage[0..decoded.course_id.len], decoded.course_id);
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
            const cancellation = beginCancellation(slot);
            if (!cancellation.handled) continue;
            core_request_id = cancellation.core_request_id;
            found = true;
            break;
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
            if (slot.state == .active or slot.state == .cancelling) {
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
        const expected_revision = if (slot.expected_revision == 0) revision else slot.expected_revision;
        var core_request_id: u64 = 0;
        const status = switch (slot.operation) {
            .load_library_page => blk: {
                const request: c.ml_library_course_page_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_library_course_page_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .offset = slot.page_offset,
                    .limit = library_page_size,
                    .reserved = 0,
                };
                break :blk c.ml_library_course_page_v1(core, &request, &core_request_id);
            },
            .access_course => blk: {
                const request: c.ml_course_access_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_course_access_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                    .course_id = slot.courseId().ptr,
                    .course_id_len = slot.course_id_len,
                };
                break :blk c.ml_course_access_v1(core, &request, &core_request_id);
            },
            .load_lesson_page => blk: {
                const request: c.ml_library_lesson_page_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_library_lesson_page_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .offset = slot.page_offset,
                    .limit = lesson_page_size,
                    .reserved = 0,
                    .course_id = slot.courseId().ptr,
                    .course_id_len = slot.course_id_len,
                    .section_id = null,
                    .section_id_len = 0,
                };
                break :blk c.ml_library_lesson_page_v1(core, &request, &core_request_id);
            },
        };
        if (status == c.ML_STATUS_OK and core_request_id != 0) {
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

        const active = self.takeActive(event.request_id) orelse return;
        if (event.kind == c.ML_EVENT_REQUEST_CANCELLED) return;
        if (event.kind != expectedEventKind(active.operation) or event.payload_schema_version != 1) {
            if (active.completion) |completion| {
                self.completeWithRetry(completion, .failure, "The Library service returned an unexpected response.");
            }
            return;
        }
        if (event.status == c.ML_STATUS_OK) {
            if (active.operation == .access_course) {
                revision.* = courseAccessRevision(payload, revision.*) catch {
                    if (active.completion) |completion| {
                        self.completeWithRetry(completion, .failure, "The Library service returned an unexpected response.");
                    }
                    return;
                };
            }
            if (active.completion) |completion| self.completeWithRetry(completion, .success, payload);
            return;
        }
        if (event.status == c.ML_STATUS_STALE) revision.* = staleActualRevision(payload) catch revision.*;
        if (active.completion) |completion| {
            self.completeWithRetry(completion, .failure, requestFailureMessage(active.operation, event.status));
        }
    }

    const Active = struct {
        operation: Operation,
        completion: ?native_sdk.ExternalEffectCompletion,
    };

    fn takeActive(self: *CoreAdapter, core_request_id: u64) ?Active {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        for (&self.slots) |*slot| {
            if ((slot.state == .active or slot.state == .cancelling) and slot.core_request_id == core_request_id) {
                const active: Active = .{
                    .operation = slot.operation,
                    .completion = if (slot.cancelled) null else slot.completion,
                };
                slot.* = .{};
                return active;
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

const Cancellation = struct {
    handled: bool = false,
    core_request_id: u64 = 0,
};

fn beginCancellation(slot: *Slot) Cancellation {
    if (!slot.cancelled) return .{};
    return switch (slot.state) {
        .queued => blk: {
            slot.* = .{};
            break :blk .{ .handled = true };
        },
        .active => blk: {
            const core_request_id = slot.core_request_id;
            slot.state = .cancelling;
            break :blk .{ .handled = true, .core_request_id = core_request_id };
        },
        .free, .cancelling => .{},
    };
}

pub fn encodeLibraryPageRequest(buffer: *[library_page_request_bytes]u8, expected_revision: u64, offset: u64) []const u8 {
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], offset, .little);
    return buffer;
}

pub fn encodeCourseAccessRequest(buffer: []u8, expected_revision: u64, course_id: []const u8) ![]const u8 {
    const len = course_access_request_header_bytes + course_id.len;
    if (buffer.len < len or expected_revision == 0 or !validCourseId(course_id)) {
        return error.InvalidCourseAccessRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    @memcpy(buffer[8..len], course_id);
    return buffer[0..len];
}

pub fn encodeLessonPageRequest(buffer: []u8, expected_revision: u64, offset: u64, course_id: []const u8) ![]const u8 {
    const len = lesson_page_request_header_bytes + course_id.len;
    if (buffer.len < len or
        expected_revision == 0 or
        offset > std.math.maxInt(i64) or
        offset % lesson_page_size != 0 or
        !validCourseId(course_id))
    {
        return error.InvalidLessonPageRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], offset, .little);
    @memcpy(buffer[16..len], course_id);
    return buffer[0..len];
}

fn decodeRequest(operation: Operation, payload: []const u8) !Request {
    return switch (operation) {
        .load_library_page => blk: {
            if (payload.len != library_page_request_bytes) return error.InvalidLibraryPageRequest;
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const offset = std.mem.readInt(u64, payload[8..16], .little);
            if (offset > std.math.maxInt(i64) or
                offset % library_page_size != 0 or
                (expected_revision == 0 and offset != 0))
            {
                return error.InvalidLibraryPageRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .page_offset = offset,
            };
        },
        .access_course => blk: {
            if (payload.len <= course_access_request_header_bytes or
                payload.len > course_access_request_header_bytes + max_course_id_bytes)
            {
                return error.InvalidCourseAccessRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const course_id = payload[course_access_request_header_bytes..];
            if (expected_revision == 0 or !validCourseId(course_id)) return error.InvalidCourseAccessRequest;
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .course_id = course_id,
            };
        },
        .load_lesson_page => blk: {
            if (payload.len <= lesson_page_request_header_bytes or
                payload.len > lesson_page_request_header_bytes + max_course_id_bytes)
            {
                return error.InvalidLessonPageRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const offset = std.mem.readInt(u64, payload[8..16], .little);
            const course_id = payload[lesson_page_request_header_bytes..];
            if (expected_revision == 0 or
                offset > std.math.maxInt(i64) or
                offset % lesson_page_size != 0 or
                !validCourseId(course_id))
            {
                return error.InvalidLessonPageRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .page_offset = offset,
                .course_id = course_id,
            };
        },
    };
}

fn validCourseId(course_id: []const u8) bool {
    return course_id.len != 0 and
        course_id.len <= max_course_id_bytes and
        std.unicode.utf8ValidateSlice(course_id) and
        std.mem.indexOfScalar(u8, course_id, 0) == null;
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

fn expectedEventKind(operation: Operation) c.ml_event_kind_t {
    return switch (operation) {
        .load_library_page => c.ML_EVENT_LIBRARY_COURSE_PAGE,
        .access_course => c.ML_EVENT_COURSE_ACCESSED,
        .load_lesson_page => c.ML_EVENT_LIBRARY_LESSON_PAGE,
    };
}

fn courseAccessRevision(payload: []const u8, previous_revision: u64) !u64 {
    const Access = struct { revision: u64 };
    const parsed = try std.json.parseFromSlice(Access, std.heap.page_allocator, payload, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    if (parsed.value.revision <= previous_revision) return error.InvalidCourseAccessResponse;
    return parsed.value.revision;
}

fn staleActualRevision(payload: []const u8) !u64 {
    const Stale = struct { actual: u64 };
    const parsed = try std.json.parseFromSlice(Stale, std.heap.page_allocator, payload, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    if (parsed.value.actual == 0) return error.InvalidStaleResponse;
    return parsed.value.actual;
}

fn requestFailureMessage(operation: Operation, status: c.ml_status_t) []const u8 {
    return switch (status) {
        c.ML_STATUS_STALE => switch (operation) {
            .load_library_page => "The Library changed while this page was opening.",
            .access_course, .load_lesson_page => "The Library changed while this Course was opening.",
        },
        c.ML_STATUS_NOT_FOUND => if (operation == .access_course)
            "This Course is no longer available."
        else
            "The requested Library item was not found.",
        c.ML_STATUS_CANCELLED => switch (operation) {
            .load_library_page => "Opening the Library was cancelled.",
            .access_course, .load_lesson_page => "Opening the Course was cancelled.",
        },
        c.ML_STATUS_BUSY => "The Library is busy. Try again.",
        c.ML_STATUS_PANIC => "The Library service stopped unexpectedly.",
        else => "The Library database could not be read.",
    };
}

test "Library page requests use one strict revision and offset wire format" {
    var payload: [library_page_request_bytes]u8 = undefined;
    _ = encodeLibraryPageRequest(&payload, 7, 20);
    const decoded = try decodeRequest(.load_library_page, &payload);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);
    try std.testing.expectEqual(@as(u64, 20), decoded.page_offset);

    _ = encodeLibraryPageRequest(&payload, 0, 20);
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeRequest(.load_library_page, &payload));

    _ = encodeLibraryPageRequest(&payload, 7, 1);
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeRequest(.load_library_page, &payload));
    try std.testing.expectError(error.InvalidLibraryPageRequest, decodeRequest(.load_library_page, payload[0..15]));
}

test "Course requests use bounded strict binary wire formats" {
    var access_storage: [course_access_request_header_bytes + max_course_id_bytes]u8 = undefined;
    const access = try encodeCourseAccessRequest(&access_storage, 7, "course-1");
    const decoded_access = try decodeRequest(.access_course, access);
    try std.testing.expectEqual(@as(u64, 7), decoded_access.expected_revision);
    try std.testing.expectEqualStrings("course-1", decoded_access.course_id);
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(&access_storage, 0, "course-1"));
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(&access_storage, 7, ""));
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(&access_storage, 7, "bad\x00id"));
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(&access_storage, 7, "\xff"));
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(access_storage[0..8], 7, "course-1"));

    const max_id = [_]u8{'c'} ** max_course_id_bytes;
    _ = try encodeCourseAccessRequest(&access_storage, 7, &max_id);
    const oversized_id = [_]u8{'c'} ** (max_course_id_bytes + 1);
    var oversized_access_storage: [course_access_request_header_bytes + oversized_id.len]u8 = undefined;
    try std.testing.expectError(error.InvalidCourseAccessRequest, encodeCourseAccessRequest(&oversized_access_storage, 7, &oversized_id));

    var lesson_storage: [lesson_page_request_header_bytes + max_course_id_bytes]u8 = undefined;
    const lesson_page = try encodeLessonPageRequest(&lesson_storage, 8, 20, "course-1");
    const decoded_lesson_page = try decodeRequest(.load_lesson_page, lesson_page);
    try std.testing.expectEqual(@as(u64, 8), decoded_lesson_page.expected_revision);
    try std.testing.expectEqual(@as(u64, 20), decoded_lesson_page.page_offset);
    try std.testing.expectEqualStrings("course-1", decoded_lesson_page.course_id);
    try std.testing.expectError(error.InvalidLessonPageRequest, encodeLessonPageRequest(&lesson_storage, 0, 20, "course-1"));
    try std.testing.expectError(error.InvalidLessonPageRequest, encodeLessonPageRequest(&lesson_storage, 8, 1, "course-1"));
    try std.testing.expectError(error.InvalidLessonPageRequest, encodeLessonPageRequest(&lesson_storage, 8, @as(u64, std.math.maxInt(i64)) + 1, "course-1"));
    try std.testing.expectError(error.InvalidLessonPageRequest, encodeLessonPageRequest(&lesson_storage, 8, 20, "bad\x00id"));
}

test "active mutation cancellation stays correlated until its terminal core event" {
    var active = Slot{
        .state = .active,
        .operation = .access_course,
        .core_request_id = 41,
        .cancelled = true,
    };
    const cancellation = beginCancellation(&active);
    try std.testing.expect(cancellation.handled);
    try std.testing.expectEqual(@as(u64, 41), cancellation.core_request_id);
    try std.testing.expectEqual(SlotState.cancelling, active.state);
    try std.testing.expectEqual(@as(u64, 41), active.core_request_id);

    var queued = Slot{ .state = .queued, .cancelled = true };
    const retired = beginCancellation(&queued);
    try std.testing.expect(retired.handled);
    try std.testing.expectEqual(SlotState.free, queued.state);
}
