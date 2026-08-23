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
pub const search_index_key: u64 = 4;
pub const library_stats_key: u64 = 5;
pub const activity_page_key: u64 = 6;
pub const progress_put_key: u64 = 7;
pub const library_state_key: u64 = 8;
pub const library_scan_key: u64 = 9;
pub const library_scan_cancel_key: u64 = 10;
pub const library_scan_progress_key: u64 = 11;
pub const notes_list_key: u64 = 12;
pub const note_save_key: u64 = 13;
pub const note_delete_key: u64 = 14;
pub const settings_get_key: u64 = 15;
pub const settings_put_appearance_key: u64 = 16;
pub const document_open_key: u64 = 17;
pub const document_page_key: u64 = 18;
pub const document_external_open_key: u64 = 19;
pub const search_query_key_base: u64 = 1_000;
pub const library_page_request_bytes: usize = 16;
pub const library_state_request_bytes: usize = 8;
pub const library_stats_request_bytes: usize = 8;
pub const activity_page_request_bytes: usize = 8;
pub const progress_put_request_header_bytes: usize = 25;
pub const library_scan_request_header_bytes: usize = 8;
pub const notes_list_request_header_bytes: usize = 16;
pub const note_save_request_header_bytes: usize = 20;
pub const note_delete_request_header_bytes: usize = 8;
pub const settings_put_appearance_request_bytes: usize = 12;
pub const document_open_request_header_bytes: usize = 8;
pub const document_page_request_header_bytes: usize = 16;
pub const document_external_open_request_header_bytes: usize = 8;
pub const course_access_request_header_bytes: usize = 8;
pub const lesson_page_request_header_bytes: usize = 16;
pub const search_index_request_bytes: usize = 8;
pub const search_query_request_header_bytes: usize = 24;
pub const library_page_size: u32 = 20;
pub const lesson_page_size: u32 = 20;
pub const search_page_size: u32 = 20;
pub const document_page_size: u32 = 8;
pub const max_course_id_bytes: usize = 128;
pub const max_lesson_id_bytes: usize = 128;
pub const max_search_query_bytes: usize = 512;
pub const max_root_path_bytes: usize = native_sdk.platform.max_dialog_path_bytes;
pub const max_note_id_bytes: usize = 128;
pub const max_note_text_bytes: usize = c.ML_MAX_NOTE_TEXT_BYTES;
pub const scan_cancelled_message = "The Library scan was cancelled.";
pub const scan_progress_result_bytes: usize = 32;

pub const Operation = enum(u32) {
    load_library_page = 1,
    access_course = 2,
    load_lesson_page = 3,
    rebuild_search_index = 4,
    query_search = 5,
    load_library_stats = 6,
    load_activity_page = 7,
    put_progress = 8,
    load_library_state = 9,
    scan_library = 10,
    cancel_library_scan = 11,
    load_library_scan_progress = 12,
    load_notes = 13,
    save_note = 14,
    delete_note = 15,
    load_settings = 16,
    put_appearance = 17,
    open_document = 18,
    load_document_page = 19,
    prepare_document_external_open = 20,
};

const Request = struct {
    operation: Operation,
    expected_revision: u64,
    page_offset: u64 = 0,
    course_id: []const u8 = "",
    lesson_id: []const u8 = "",
    document_id: []const u8 = "",
    watched_time: u64 = 0,
    last_position: f64 = 0,
    completed: bool = false,
    query_id: u64 = 0,
    query: []const u8 = "",
    root_path: []const u8 = "",
    note_id: []const u8 = "",
    note_text: []const u8 = "",
    note_timestamp: f64 = 0,
    appearance: u32 = 0,
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
    lesson_id_storage: [max_lesson_id_bytes]u8 = [_]u8{0} ** max_lesson_id_bytes,
    lesson_id_len: usize = 0,
    document_id_storage: [max_lesson_id_bytes]u8 = [_]u8{0} ** max_lesson_id_bytes,
    document_id_len: usize = 0,
    watched_time: u64 = 0,
    last_position: f64 = 0,
    completed: bool = false,
    query_id: u64 = 0,
    query_storage: [max_search_query_bytes]u8 = [_]u8{0} ** max_search_query_bytes,
    query_len: usize = 0,
    root_path_storage: [max_root_path_bytes]u8 = [_]u8{0} ** max_root_path_bytes,
    root_path_len: usize = 0,
    note_id_storage: [max_note_id_bytes]u8 = [_]u8{0} ** max_note_id_bytes,
    note_id_len: usize = 0,
    note_text_storage: [max_note_text_bytes]u8 = [_]u8{0} ** max_note_text_bytes,
    note_text_len: usize = 0,
    note_timestamp: f64 = 0,
    appearance: u32 = 0,
    cancelled: bool = false,
    completion: ?native_sdk.ExternalEffectCompletion = null,

    fn courseId(slot: *const Slot) []const u8 {
        return slot.course_id_storage[0..slot.course_id_len];
    }

    fn query(slot: *const Slot) []const u8 {
        return slot.query_storage[0..slot.query_len];
    }

    fn lessonId(slot: *const Slot) []const u8 {
        return slot.lesson_id_storage[0..slot.lesson_id_len];
    }

    fn documentId(slot: *const Slot) []const u8 {
        return slot.document_id_storage[0..slot.document_id_len];
    }

    fn rootPath(slot: *const Slot) []const u8 {
        return slot.root_path_storage[0..slot.root_path_len];
    }

    fn noteId(slot: *const Slot) []const u8 {
        return slot.note_id_storage[0..slot.note_id_len];
    }

    fn noteText(slot: *const Slot) []const u8 {
        return slot.note_text_storage[0..slot.note_text_len];
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
            @intFromEnum(Operation.rebuild_search_index) => .rebuild_search_index,
            @intFromEnum(Operation.query_search) => .query_search,
            @intFromEnum(Operation.load_library_stats) => .load_library_stats,
            @intFromEnum(Operation.load_activity_page) => .load_activity_page,
            @intFromEnum(Operation.put_progress) => .put_progress,
            @intFromEnum(Operation.load_library_state) => .load_library_state,
            @intFromEnum(Operation.scan_library) => .scan_library,
            @intFromEnum(Operation.cancel_library_scan) => .cancel_library_scan,
            @intFromEnum(Operation.load_library_scan_progress) => .load_library_scan_progress,
            @intFromEnum(Operation.load_notes) => .load_notes,
            @intFromEnum(Operation.save_note) => .save_note,
            @intFromEnum(Operation.delete_note) => .delete_note,
            @intFromEnum(Operation.load_settings) => .load_settings,
            @intFromEnum(Operation.put_appearance) => .put_appearance,
            @intFromEnum(Operation.open_document) => .open_document,
            @intFromEnum(Operation.load_document_page) => .load_document_page,
            @intFromEnum(Operation.prepare_document_external_open) => .prepare_document_external_open,
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
            .lesson_id_len = decoded.lesson_id.len,
            .document_id_len = decoded.document_id.len,
            .watched_time = decoded.watched_time,
            .last_position = decoded.last_position,
            .completed = decoded.completed,
            .query_id = decoded.query_id,
            .query_len = decoded.query.len,
            .root_path_len = decoded.root_path.len,
            .note_id_len = decoded.note_id.len,
            .note_text_len = decoded.note_text.len,
            .note_timestamp = decoded.note_timestamp,
            .appearance = decoded.appearance,
            .completion = completion,
        };
        @memcpy(slot.course_id_storage[0..decoded.course_id.len], decoded.course_id);
        @memcpy(slot.lesson_id_storage[0..decoded.lesson_id.len], decoded.lesson_id);
        @memcpy(slot.document_id_storage[0..decoded.document_id.len], decoded.document_id);
        @memcpy(slot.query_storage[0..decoded.query.len], decoded.query);
        @memcpy(slot.root_path_storage[0..decoded.root_path.len], decoded.root_path);
        @memcpy(slot.note_id_storage[0..decoded.note_id.len], decoded.note_id);
        @memcpy(slot.note_text_storage[0..decoded.note_text.len], decoded.note_text);
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
            if (self.serviceScanCancellation(handle)) continue;
            if (self.serviceScanProgress(handle)) continue;
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

    fn serviceScanCancellation(self: *CoreAdapter, core: *c.ml_core_t) bool {
        var completion: ?native_sdk.ExternalEffectCompletion = null;
        var outcome: native_sdk.ExternalEffectAdapterOutcome = .failure;
        var response: []const u8 = "No Library scan is active.";

        self.mutex.lockUncancelable(self.io);
        const control = for (&self.slots) |*slot| {
            if (slot.state == .queued and slot.operation == .cancel_library_scan) break slot;
        } else {
            self.mutex.unlock(self.io);
            return false;
        };
        const scan = for (&self.slots) |*slot| {
            if ((slot.state == .active or slot.state == .cancelling) and
                slot.operation == .scan_library)
            {
                break slot;
            }
        } else null;

        completion = control.completion;
        if (scan) |active_scan| {
            switch (c.ml_core_cancel(core, active_scan.core_request_id)) {
                c.ML_STATUS_OK => {
                    active_scan.state = .cancelling;
                    outcome = .success;
                    response = "{\"outcome\":\"accepted\"}";
                },
                c.ML_STATUS_TOO_LATE => {
                    outcome = .success;
                    response = "{\"outcome\":\"tooLate\"}";
                },
                else => {},
            }
        }
        control.* = .{};
        self.mutex.unlock(self.io);

        self.completeWithRetry(completion.?, outcome, response);
        return true;
    }

    fn serviceScanProgress(self: *CoreAdapter, core: *c.ml_core_t) bool {
        var completion: ?native_sdk.ExternalEffectCompletion = null;
        var outcome: native_sdk.ExternalEffectAdapterOutcome = .failure;
        var result_storage: [scan_progress_result_bytes]u8 = [_]u8{0} ** scan_progress_result_bytes;
        var response: []const u8 = "No Library scan is active.";

        self.mutex.lockUncancelable(self.io);
        const query = for (&self.slots) |*slot| {
            if (slot.state == .queued and slot.operation == .load_library_scan_progress) break slot;
        } else {
            self.mutex.unlock(self.io);
            return false;
        };
        const scan = for (&self.slots) |*slot| {
            if ((slot.state == .active or slot.state == .cancelling) and
                slot.operation == .scan_library)
            {
                break slot;
            }
        } else null;

        completion = query.completion;
        if (scan) |active_scan| {
            var snapshot: c.ml_library_scan_progress_snapshot_v1 = .{
                .struct_size = @sizeOf(c.ml_library_scan_progress_snapshot_v1),
                .abi_version = c.ML_ABI_VERSION,
                .request_id = 0,
                .processed = 0,
                .discovered = 0,
                .total = 0,
                .phase = 0,
                .total_known = 0,
                .cancellable = 0,
                .reserved = 0,
            };
            if (c.ml_library_scan_progress_v1(
                core,
                active_scan.core_request_id,
                &snapshot,
            ) == c.ML_STATUS_OK and snapshot.request_id == active_scan.core_request_id) {
                std.mem.writeInt(u32, result_storage[0..4], snapshot.phase, .little);
                result_storage[4] = snapshot.total_known;
                result_storage[5] = snapshot.cancellable;
                std.mem.writeInt(u64, result_storage[8..16], snapshot.processed, .little);
                std.mem.writeInt(u64, result_storage[16..24], snapshot.discovered, .little);
                std.mem.writeInt(u64, result_storage[24..32], snapshot.total, .little);
                outcome = .success;
                response = &result_storage;
            }
        }
        query.* = .{};
        self.mutex.unlock(self.io);

        self.completeWithRetry(completion.?, outcome, response);
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
            .cancel_library_scan, .load_library_scan_progress => unreachable,
            .load_settings => blk: {
                const request: c.ml_settings_get_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_settings_get_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .reserved = 0,
                };
                break :blk c.ml_settings_get_v1(core, &request, &core_request_id);
            },
            .put_appearance => blk: {
                const request: c.ml_settings_put_appearance_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_settings_put_appearance_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = slot.expected_revision,
                    .appearance = slot.appearance,
                    .reserved = 0,
                };
                break :blk c.ml_settings_put_appearance_v1(core, &request, &core_request_id);
            },
            .open_document => blk: {
                const request: c.ml_document_open_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_document_open_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                    .lesson_id = slot.lessonId().ptr,
                    .lesson_id_len = slot.lesson_id_len,
                };
                break :blk c.ml_document_open_v1(core, &request, &core_request_id);
            },
            .load_document_page => blk: {
                const request: c.ml_document_page_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_document_page_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .offset = slot.page_offset,
                    .limit = document_page_size,
                    .reserved = 0,
                    .document_id = slot.documentId().ptr,
                    .document_id_len = slot.document_id_len,
                };
                break :blk c.ml_document_page_v1(core, &request, &core_request_id);
            },
            .prepare_document_external_open => blk: {
                const request: c.ml_document_external_open_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_document_external_open_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                    .lesson_id = slot.lessonId().ptr,
                    .lesson_id_len = slot.lesson_id_len,
                };
                break :blk c.ml_document_external_open_v1(core, &request, &core_request_id);
            },
            .load_notes => blk: {
                const request: c.ml_notes_list_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_notes_list_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .offset = slot.page_offset,
                    .limit = lesson_page_size,
                    .reserved = 0,
                    .lesson_id = slot.lessonId().ptr,
                    .lesson_id_len = slot.lesson_id_len,
                };
                break :blk c.ml_notes_list_v1(core, &request, &core_request_id);
            },
            .save_note => blk: {
                const note_id = slot.noteId();
                const request: c.ml_notes_save_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_notes_save_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .timestamp = slot.note_timestamp,
                    .reserved = 0,
                    .lesson_id = slot.lessonId().ptr,
                    .lesson_id_len = slot.lesson_id_len,
                    .note_id = if (note_id.len == 0) null else note_id.ptr,
                    .note_id_len = note_id.len,
                    .text = slot.noteText().ptr,
                    .text_len = slot.note_text_len,
                };
                break :blk c.ml_notes_save_v1(core, &request, &core_request_id);
            },
            .delete_note => blk: {
                const request: c.ml_notes_delete_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_notes_delete_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                    .note_id = slot.noteId().ptr,
                    .note_id_len = slot.note_id_len,
                };
                break :blk c.ml_notes_delete_v1(core, &request, &core_request_id);
            },
            .scan_library => blk: {
                const request: c.ml_library_scan_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_library_scan_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .root_path = slot.rootPath().ptr,
                    .root_path_len = slot.root_path_len,
                };
                break :blk c.ml_library_scan_v1(core, &request, &core_request_id);
            },
            .load_library_state => blk: {
                const request: c.ml_library_state_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_library_state_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                };
                break :blk c.ml_library_state_v1(core, &request, &core_request_id);
            },
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
            .rebuild_search_index => blk: {
                const request: c.ml_search_rebuild_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_search_rebuild_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                };
                break :blk c.ml_search_rebuild_v1(core, &request, &core_request_id);
            },
            .query_search => blk: {
                const request: c.ml_search_query_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_search_query_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_index_revision = slot.expected_revision,
                    .query_id = slot.query_id,
                    .offset = slot.page_offset,
                    .limit = search_page_size,
                    .reserved = 0,
                    .query = slot.query().ptr,
                    .query_len = slot.query_len,
                };
                break :blk c.ml_search_query_v1(core, &request, &core_request_id);
            },
            .load_library_stats => blk: {
                const request: c.ml_library_stats_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_library_stats_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .reserved = 0,
                };
                break :blk c.ml_library_stats_v1(core, &request, &core_request_id);
            },
            .load_activity_page => blk: {
                const request: c.ml_activity_day_page_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_activity_day_page_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .offset = 0,
                    .lookback_days = 84,
                    .limit = 84,
                    .reserved = 0,
                };
                break :blk c.ml_activity_day_page_v1(core, &request, &core_request_id);
            },
            .put_progress => blk: {
                const request: c.ml_progress_put_request_v1 = .{
                    .struct_size = @sizeOf(c.ml_progress_put_request_v1),
                    .abi_version = c.ML_ABI_VERSION,
                    .expected_revision = expected_revision,
                    .watched_time = slot.watched_time,
                    .last_position = slot.last_position,
                    .completed = @intFromBool(slot.completed),
                    .reserved = 0,
                    .lesson_id = slot.lessonId().ptr,
                    .lesson_id_len = slot.lesson_id_len,
                };
                break :blk c.ml_progress_put_v1(core, &request, &core_request_id);
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
        if (event.kind == c.ML_EVENT_REQUEST_CANCELLED) {
            if (active.operation == .scan_library) {
                if (active.completion) |completion| {
                    self.completeWithRetry(completion, .failure, scan_cancelled_message);
                }
            }
            return;
        }
        if (event.kind != expectedEventKind(active.operation) or event.payload_schema_version != 1) {
            if (active.completion) |completion| {
                self.completeWithRetry(completion, .failure, "The Library service returned an unexpected response.");
            }
            return;
        }
        if (event.status == c.ML_STATUS_OK) {
            if (active.operation == .access_course or
                active.operation == .put_progress or
                active.operation == .scan_library or
                active.operation == .save_note or
                active.operation == .delete_note)
            {
                revision.* = mutationRevision(payload, revision.*) catch {
                    if (active.completion) |completion| {
                        self.completeWithRetry(completion, .failure, "The Library service returned an unexpected response.");
                    }
                    return;
                };
            }
            if (active.completion) |completion| self.completeWithRetry(completion, .success, payload);
            return;
        }
        if (event.status == c.ML_STATUS_STALE and
            active.operation != .query_search and
            active.operation != .put_appearance)
        {
            revision.* = staleActualRevision(payload) catch revision.*;
        }
        if (active.completion) |completion| {
            const failure = if ((active.operation == .query_search and event.status == c.ML_STATUS_STALE) or
                (active.operation == .open_document and event.status == c.ML_STATUS_FAILED))
                payload
            else
                requestFailureMessage(active.operation, event.status);
            self.completeWithRetry(completion, .failure, failure);
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

pub fn encodeLibraryStateRequest(buffer: *[library_state_request_bytes]u8, expected_revision: u64) []const u8 {
    std.mem.writeInt(u64, buffer, expected_revision, .little);
    return buffer;
}

pub fn encodeDocumentOpenRequest(buffer: []u8, expected_revision: u64, lesson_id: []const u8) ![]const u8 {
    const len = document_open_request_header_bytes + lesson_id.len;
    if (buffer.len < len or expected_revision == 0 or !validLessonId(lesson_id)) {
        return error.InvalidDocumentOpenRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    @memcpy(buffer[document_open_request_header_bytes..len], lesson_id);
    return buffer[0..len];
}

pub fn encodeDocumentPageRequest(buffer: []u8, expected_revision: u64, offset: u64, document_id: []const u8) ![]const u8 {
    const len = document_page_request_header_bytes + document_id.len;
    if (buffer.len < len or
        expected_revision == 0 or
        offset > std.math.maxInt(i64) or
        offset % document_page_size != 0 or
        !validLessonId(document_id))
    {
        return error.InvalidDocumentPageRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], offset, .little);
    @memcpy(buffer[document_page_request_header_bytes..len], document_id);
    return buffer[0..len];
}

pub fn encodeDocumentExternalOpenRequest(buffer: []u8, expected_revision: u64, lesson_id: []const u8) ![]const u8 {
    const len = document_external_open_request_header_bytes + lesson_id.len;
    if (buffer.len < len or expected_revision == 0 or !validLessonId(lesson_id)) {
        return error.InvalidDocumentExternalOpenRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    @memcpy(buffer[document_external_open_request_header_bytes..len], lesson_id);
    return buffer[0..len];
}

pub fn encodeSettingsPutAppearanceRequest(
    buffer: *[settings_put_appearance_request_bytes]u8,
    expected_revision: u64,
    appearance: u32,
) ![]const u8 {
    if (expected_revision == 0 or
        appearance < c.ML_APPEARANCE_LIGHT or
        appearance > c.ML_APPEARANCE_COZY)
    {
        return error.InvalidSettingsPutAppearanceRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u32, buffer[8..12], appearance, .little);
    return buffer;
}

pub fn encodeLibraryScanRequest(buffer: []u8, expected_revision: u64, root_path: []const u8) ![]const u8 {
    const len = library_scan_request_header_bytes + root_path.len;
    if (buffer.len < len or expected_revision == 0 or !validRootPath(root_path)) {
        return error.InvalidLibraryScanRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    @memcpy(buffer[library_scan_request_header_bytes..len], root_path);
    return buffer[0..len];
}

pub fn encodeNotesListRequest(
    buffer: []u8,
    expected_revision: u64,
    offset: u64,
    lesson_id: []const u8,
) ![]const u8 {
    const len = notes_list_request_header_bytes + lesson_id.len;
    if (buffer.len < len or
        expected_revision == 0 or
        offset > std.math.maxInt(i64) or
        offset % lesson_page_size != 0 or
        !validLessonId(lesson_id))
    {
        return error.InvalidNotesListRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], offset, .little);
    @memcpy(buffer[notes_list_request_header_bytes..len], lesson_id);
    return buffer[0..len];
}

pub fn encodeNoteSaveRequest(
    buffer: []u8,
    expected_revision: u64,
    timestamp: f64,
    lesson_id: []const u8,
    note_id: []const u8,
    text: []const u8,
) ![]const u8 {
    const len = note_save_request_header_bytes + lesson_id.len + note_id.len + text.len;
    if (buffer.len < len or
        expected_revision == 0 or
        !std.math.isFinite(timestamp) or
        timestamp < 0 or
        !validLessonId(lesson_id) or
        (note_id.len != 0 and !validNoteId(note_id)) or
        !validNoteText(text) or
        lesson_id.len > std.math.maxInt(u16) or
        note_id.len > std.math.maxInt(u16))
    {
        return error.InvalidNoteSaveRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], @bitCast(timestamp), .little);
    std.mem.writeInt(u16, buffer[16..18], @intCast(lesson_id.len), .little);
    std.mem.writeInt(u16, buffer[18..20], @intCast(note_id.len), .little);
    var cursor = note_save_request_header_bytes;
    @memcpy(buffer[cursor..][0..lesson_id.len], lesson_id);
    cursor += lesson_id.len;
    @memcpy(buffer[cursor..][0..note_id.len], note_id);
    cursor += note_id.len;
    @memcpy(buffer[cursor..][0..text.len], text);
    return buffer[0..len];
}

pub fn encodeNoteDeleteRequest(
    buffer: []u8,
    expected_revision: u64,
    note_id: []const u8,
) ![]const u8 {
    const len = note_delete_request_header_bytes + note_id.len;
    if (buffer.len < len or expected_revision == 0 or !validNoteId(note_id)) {
        return error.InvalidNoteDeleteRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    @memcpy(buffer[note_delete_request_header_bytes..len], note_id);
    return buffer[0..len];
}

pub fn encodeLibraryStatsRequest(buffer: *[library_stats_request_bytes]u8, expected_revision: u64) ![]const u8 {
    if (expected_revision == 0) return error.InvalidLibraryStatsRequest;
    std.mem.writeInt(u64, buffer, expected_revision, .little);
    return buffer;
}

pub fn encodeActivityPageRequest(buffer: *[activity_page_request_bytes]u8, expected_revision: u64) ![]const u8 {
    if (expected_revision == 0) return error.InvalidActivityPageRequest;
    std.mem.writeInt(u64, buffer, expected_revision, .little);
    return buffer;
}

pub fn encodeProgressPutRequest(
    buffer: []u8,
    expected_revision: u64,
    watched_time: u64,
    last_position: f64,
    completed: bool,
    lesson_id: []const u8,
) ![]const u8 {
    const len = progress_put_request_header_bytes + lesson_id.len;
    if (buffer.len < len or
        expected_revision == 0 or
        watched_time > std.math.maxInt(i64) or
        !std.math.isFinite(last_position) or
        last_position < 0 or
        !validLessonId(lesson_id))
    {
        return error.InvalidProgressPutRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], watched_time, .little);
    std.mem.writeInt(u64, buffer[16..24], @bitCast(last_position), .little);
    buffer[24] = @intFromBool(completed);
    @memcpy(buffer[progress_put_request_header_bytes..len], lesson_id);
    return buffer[0..len];
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

pub fn encodeSearchIndexRequest(buffer: *[search_index_request_bytes]u8, expected_revision: u64) ![]const u8 {
    if (expected_revision == 0) return error.InvalidSearchIndexRequest;
    std.mem.writeInt(u64, buffer, expected_revision, .little);
    return buffer;
}

pub fn encodeSearchQueryRequest(buffer: []u8, expected_index_revision: u64, query_id: u64, offset: u64, query: []const u8) ![]const u8 {
    const len = search_query_request_header_bytes + query.len;
    if (buffer.len < len or
        expected_index_revision == 0 or
        query_id == 0 or
        offset > std.math.maxInt(i64) or
        offset % search_page_size != 0 or
        !validSearchQuery(query))
    {
        return error.InvalidSearchQueryRequest;
    }
    std.mem.writeInt(u64, buffer[0..8], expected_index_revision, .little);
    std.mem.writeInt(u64, buffer[8..16], query_id, .little);
    std.mem.writeInt(u64, buffer[16..24], offset, .little);
    @memcpy(buffer[24..len], query);
    return buffer[0..len];
}

fn decodeRequest(operation: Operation, payload: []const u8) !Request {
    return switch (operation) {
        .load_settings => blk: {
            if (payload.len != 0) return error.InvalidSettingsGetRequest;
            break :blk .{ .operation = operation, .expected_revision = 0 };
        },
        .put_appearance => blk: {
            if (payload.len != settings_put_appearance_request_bytes) {
                return error.InvalidSettingsPutAppearanceRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const appearance = std.mem.readInt(u32, payload[8..12], .little);
            if (expected_revision == 0 or
                appearance < c.ML_APPEARANCE_LIGHT or
                appearance > c.ML_APPEARANCE_COZY)
            {
                return error.InvalidSettingsPutAppearanceRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .appearance = appearance,
            };
        },
        .open_document, .prepare_document_external_open => blk: {
            const header_bytes = if (operation == .open_document)
                document_open_request_header_bytes
            else
                document_external_open_request_header_bytes;
            if (payload.len <= header_bytes or payload.len > header_bytes + max_lesson_id_bytes) {
                return if (operation == .open_document)
                    error.InvalidDocumentOpenRequest
                else
                    error.InvalidDocumentExternalOpenRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const lesson_id = payload[header_bytes..];
            if (expected_revision == 0 or !validLessonId(lesson_id)) {
                return if (operation == .open_document)
                    error.InvalidDocumentOpenRequest
                else
                    error.InvalidDocumentExternalOpenRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .lesson_id = lesson_id,
            };
        },
        .load_document_page => blk: {
            if (payload.len <= document_page_request_header_bytes or
                payload.len > document_page_request_header_bytes + max_lesson_id_bytes)
            {
                return error.InvalidDocumentPageRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const offset = std.mem.readInt(u64, payload[8..16], .little);
            const document_id = payload[document_page_request_header_bytes..];
            if (expected_revision == 0 or
                offset > std.math.maxInt(i64) or
                offset % document_page_size != 0 or
                !validLessonId(document_id))
            {
                return error.InvalidDocumentPageRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .page_offset = offset,
                .document_id = document_id,
            };
        },
        .load_notes => blk: {
            if (payload.len <= notes_list_request_header_bytes or
                payload.len > notes_list_request_header_bytes + max_lesson_id_bytes)
            {
                return error.InvalidNotesListRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const offset = std.mem.readInt(u64, payload[8..16], .little);
            const lesson_id = payload[notes_list_request_header_bytes..];
            if (expected_revision == 0 or
                offset > std.math.maxInt(i64) or
                offset % lesson_page_size != 0 or
                !validLessonId(lesson_id))
            {
                return error.InvalidNotesListRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .page_offset = offset,
                .lesson_id = lesson_id,
            };
        },
        .save_note => blk: {
            if (payload.len <= note_save_request_header_bytes or
                payload.len > note_save_request_header_bytes + max_lesson_id_bytes + max_note_id_bytes + max_note_text_bytes)
            {
                return error.InvalidNoteSaveRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const timestamp: f64 = @bitCast(std.mem.readInt(u64, payload[8..16], .little));
            const lesson_id_len = std.mem.readInt(u16, payload[16..18], .little);
            const note_id_len = std.mem.readInt(u16, payload[18..20], .little);
            const lesson_len: usize = lesson_id_len;
            const note_len: usize = note_id_len;
            const ids_len = lesson_len + note_len;
            if (ids_len > payload.len - note_save_request_header_bytes) {
                return error.InvalidNoteSaveRequest;
            }
            const lesson_id_start = note_save_request_header_bytes;
            const note_id_start = lesson_id_start + lesson_len;
            const text_start = note_id_start + note_len;
            const lesson_id = payload[lesson_id_start..note_id_start];
            const note_id = payload[note_id_start..text_start];
            const text = payload[text_start..];
            if (expected_revision == 0 or
                !std.math.isFinite(timestamp) or
                timestamp < 0 or
                !validLessonId(lesson_id) or
                (note_id.len != 0 and !validNoteId(note_id)) or
                !validNoteText(text))
            {
                return error.InvalidNoteSaveRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .lesson_id = lesson_id,
                .note_id = note_id,
                .note_text = text,
                .note_timestamp = timestamp,
            };
        },
        .delete_note => blk: {
            if (payload.len <= note_delete_request_header_bytes or
                payload.len > note_delete_request_header_bytes + max_note_id_bytes)
            {
                return error.InvalidNoteDeleteRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const note_id = payload[note_delete_request_header_bytes..];
            if (expected_revision == 0 or !validNoteId(note_id)) {
                return error.InvalidNoteDeleteRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .note_id = note_id,
            };
        },
        .cancel_library_scan, .load_library_scan_progress => blk: {
            if (payload.len != 0) return error.InvalidLibraryScanCancelRequest;
            break :blk .{ .operation = operation, .expected_revision = 0 };
        },
        .scan_library => blk: {
            if (payload.len <= library_scan_request_header_bytes or
                payload.len > library_scan_request_header_bytes + max_root_path_bytes)
            {
                return error.InvalidLibraryScanRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const root_path = payload[library_scan_request_header_bytes..];
            if (expected_revision == 0 or !validRootPath(root_path)) {
                return error.InvalidLibraryScanRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .root_path = root_path,
            };
        },
        .load_library_state => blk: {
            if (payload.len != library_state_request_bytes) return error.InvalidLibraryStateRequest;
            break :blk .{
                .operation = operation,
                .expected_revision = std.mem.readInt(u64, payload[0..8], .little),
            };
        },
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
        .load_library_stats => blk: {
            if (payload.len != library_stats_request_bytes) return error.InvalidLibraryStatsRequest;
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            if (expected_revision == 0) return error.InvalidLibraryStatsRequest;
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
            };
        },
        .load_activity_page => blk: {
            if (payload.len != activity_page_request_bytes) return error.InvalidActivityPageRequest;
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            if (expected_revision == 0) return error.InvalidActivityPageRequest;
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
            };
        },
        .put_progress => blk: {
            if (payload.len <= progress_put_request_header_bytes or
                payload.len > progress_put_request_header_bytes + max_lesson_id_bytes)
            {
                return error.InvalidProgressPutRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const watched_time = std.mem.readInt(u64, payload[8..16], .little);
            const last_position: f64 = @bitCast(std.mem.readInt(u64, payload[16..24], .little));
            const completed = payload[24];
            const lesson_id = payload[progress_put_request_header_bytes..];
            if (expected_revision == 0 or
                watched_time > std.math.maxInt(i64) or
                !std.math.isFinite(last_position) or
                last_position < 0 or
                completed > 1 or
                !validLessonId(lesson_id))
            {
                return error.InvalidProgressPutRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .lesson_id = lesson_id,
                .watched_time = watched_time,
                .last_position = last_position,
                .completed = completed == 1,
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
        .rebuild_search_index => blk: {
            if (payload.len != search_index_request_bytes) return error.InvalidSearchIndexRequest;
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            if (expected_revision == 0) return error.InvalidSearchIndexRequest;
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
            };
        },
        .query_search => blk: {
            if (payload.len <= search_query_request_header_bytes or
                payload.len > search_query_request_header_bytes + max_search_query_bytes)
            {
                return error.InvalidSearchQueryRequest;
            }
            const expected_revision = std.mem.readInt(u64, payload[0..8], .little);
            const query_id = std.mem.readInt(u64, payload[8..16], .little);
            const offset = std.mem.readInt(u64, payload[16..24], .little);
            const query = payload[search_query_request_header_bytes..];
            if (expected_revision == 0 or
                query_id == 0 or
                offset > std.math.maxInt(i64) or
                offset % search_page_size != 0 or
                !validSearchQuery(query))
            {
                return error.InvalidSearchQueryRequest;
            }
            break :blk .{
                .operation = operation,
                .expected_revision = expected_revision,
                .page_offset = offset,
                .query_id = query_id,
                .query = query,
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

fn validLessonId(lesson_id: []const u8) bool {
    return lesson_id.len != 0 and
        lesson_id.len <= max_lesson_id_bytes and
        std.unicode.utf8ValidateSlice(lesson_id) and
        std.mem.indexOfScalar(u8, lesson_id, 0) == null;
}

fn validSearchQuery(query: []const u8) bool {
    return query.len != 0 and
        query.len <= max_search_query_bytes and
        std.unicode.utf8ValidateSlice(query) and
        std.mem.indexOfScalar(u8, query, 0) == null;
}

fn validRootPath(path: []const u8) bool {
    return path.len != 0 and
        path.len <= max_root_path_bytes and
        std.unicode.utf8ValidateSlice(path) and
        std.mem.indexOfScalar(u8, path, 0) == null;
}

fn validNoteId(note_id: []const u8) bool {
    return note_id.len != 0 and
        note_id.len <= max_note_id_bytes and
        std.unicode.utf8ValidateSlice(note_id) and
        std.mem.indexOfScalar(u8, note_id, 0) == null;
}

fn validNoteText(text: []const u8) bool {
    if (text.len == 0 or text.len > max_note_text_bytes or !std.unicode.utf8ValidateSlice(text)) {
        return false;
    }
    var units: usize = 0;
    var has_content = false;
    var view = std.unicode.Utf8View.initUnchecked(text);
    var iterator = view.iterator();
    while (iterator.nextCodepoint()) |codepoint| {
        units += if (codepoint > 0xFFFF) 2 else 1;
        if (!isEcmascriptWhitespace(codepoint)) has_content = true;
    }
    return has_content and units <= 2_000;
}

fn isEcmascriptWhitespace(codepoint: u21) bool {
    return (codepoint >= 0x0009 and codepoint <= 0x000D) or
        codepoint == 0x0020 or
        codepoint == 0x00A0 or
        codepoint == 0x1680 or
        (codepoint >= 0x2000 and codepoint <= 0x200A) or
        codepoint == 0x2028 or
        codepoint == 0x2029 or
        codepoint == 0x202F or
        codepoint == 0x205F or
        codepoint == 0x3000 or
        codepoint == 0xFEFF;
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
        .load_settings => c.ML_EVENT_SETTINGS,
        .put_appearance => c.ML_EVENT_APPEARANCE_UPDATED,
        .open_document => c.ML_EVENT_DOCUMENT_OPENED,
        .load_document_page => c.ML_EVENT_DOCUMENT_PAGE,
        .prepare_document_external_open => c.ML_EVENT_DOCUMENT_EXTERNAL_OPEN_READY,
        .load_notes => c.ML_EVENT_NOTES_PAGE,
        .save_note => c.ML_EVENT_NOTE_SAVED,
        .delete_note => c.ML_EVENT_NOTE_DELETED,
        .cancel_library_scan, .load_library_scan_progress => 0,
        .scan_library => c.ML_EVENT_LIBRARY_SCAN,
        .load_library_state => c.ML_EVENT_LIBRARY_STATE,
        .load_library_page => c.ML_EVENT_LIBRARY_COURSE_PAGE,
        .access_course => c.ML_EVENT_COURSE_ACCESSED,
        .load_lesson_page => c.ML_EVENT_LIBRARY_LESSON_PAGE,
        .rebuild_search_index => c.ML_EVENT_SEARCH_INDEX_READY,
        .query_search => c.ML_EVENT_SEARCH_PAGE,
        .load_library_stats => c.ML_EVENT_LIBRARY_STATS,
        .load_activity_page => c.ML_EVENT_ACTIVITY_DAY_PAGE,
        .put_progress => c.ML_EVENT_PROGRESS_UPDATED,
    };
}

fn mutationRevision(payload: []const u8, previous_revision: u64) !u64 {
    const Mutation = struct { revision: u64 };
    const parsed = try std.json.parseFromSlice(Mutation, std.heap.page_allocator, payload, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    if (parsed.value.revision <= previous_revision) return error.InvalidMutationResponse;
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
            .load_settings => "Appearance settings changed while they were loading.",
            .put_appearance => "Appearance settings changed before they could be saved.",
            .open_document, .load_document_page, .prepare_document_external_open => "The Library changed while this document was opening.",
            .load_notes => "The Lesson changed while notes were loading.",
            .save_note => "The Lesson changed before the note could be saved.",
            .delete_note => "The Lesson changed before the note could be deleted.",
            .cancel_library_scan => "The Library scan cancellation became stale.",
            .load_library_scan_progress => "The Library scan progress became stale.",
            .scan_library => "The Library changed before the scan could commit.",
            .load_library_state => "The Library changed while its root was loading.",
            .load_library_page => "The Library changed while this page was opening.",
            .access_course, .load_lesson_page => "The Library changed while this Course was opening.",
            .rebuild_search_index => "The Library changed while search was preparing.",
            .query_search => "The search index changed while results were loading.",
            .load_library_stats => "The Library changed while the learning ledger was loading.",
            .load_activity_page => "The Library changed while learning activity was loading.",
            .put_progress => "The Library changed before Progress could be saved.",
        },
        c.ML_STATUS_NOT_FOUND => switch (operation) {
            .access_course => "This Course is no longer available.",
            .open_document, .load_document_page, .prepare_document_external_open => "This document is no longer available.",
            .load_notes => "This Lesson is no longer available.",
            .save_note, .delete_note => "This note is no longer available.",
            else => "The requested Library item was not found.",
        },
        c.ML_STATUS_CANCELLED => switch (operation) {
            .load_settings => "Loading appearance settings was cancelled.",
            .put_appearance => "Saving appearance settings was cancelled.",
            .open_document => "Opening the document was cancelled.",
            .load_document_page => "Loading the document page was cancelled.",
            .prepare_document_external_open => "Preparing external open was cancelled.",
            .load_notes => "Loading notes was cancelled.",
            .save_note => "Saving the note was cancelled.",
            .delete_note => "Deleting the note was cancelled.",
            .cancel_library_scan => "Cancelling the Library scan was cancelled.",
            .load_library_scan_progress => "Loading scan progress was cancelled.",
            .scan_library => "The Library scan was cancelled.",
            .load_library_state => "Loading the Library root was cancelled.",
            .load_library_page => "Opening the Library was cancelled.",
            .access_course, .load_lesson_page => "Opening the Course was cancelled.",
            .rebuild_search_index => "Preparing search was cancelled.",
            .query_search => "Searching was cancelled.",
            .load_library_stats => "Loading the learning ledger was cancelled.",
            .load_activity_page => "Loading learning activity was cancelled.",
            .put_progress => "Saving Progress was cancelled.",
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

test "Library state requests allow only the bootstrap revision wire format" {
    var payload: [library_state_request_bytes]u8 = undefined;
    const encoded = encodeLibraryStateRequest(&payload, 0);
    const bootstrap = try decodeRequest(.load_library_state, encoded);
    try std.testing.expectEqual(@as(u64, 0), bootstrap.expected_revision);

    _ = encodeLibraryStateRequest(&payload, 7);
    const current = try decodeRequest(.load_library_state, &payload);
    try std.testing.expectEqual(@as(u64, 7), current.expected_revision);
    try std.testing.expectError(error.InvalidLibraryStateRequest, decodeRequest(.load_library_state, payload[0..7]));
}

test "appearance settings requests use strict typed wire values" {
    const get = try decodeRequest(.load_settings, "");
    try std.testing.expectEqual(Operation.load_settings, get.operation);
    try std.testing.expectError(error.InvalidSettingsGetRequest, decodeRequest(.load_settings, "x"));

    var payload: [settings_put_appearance_request_bytes]u8 = undefined;
    const encoded = try encodeSettingsPutAppearanceRequest(&payload, 7, c.ML_APPEARANCE_COZY);
    try std.testing.expectEqualSlices(u8, &payload, encoded);
    const put = try decodeRequest(.put_appearance, encoded);
    try std.testing.expectEqual(@as(u64, 7), put.expected_revision);
    try std.testing.expectEqual(@as(u32, c.ML_APPEARANCE_COZY), put.appearance);

    try std.testing.expectError(
        error.InvalidSettingsPutAppearanceRequest,
        encodeSettingsPutAppearanceRequest(&payload, 0, c.ML_APPEARANCE_LIGHT),
    );
    try std.testing.expectError(
        error.InvalidSettingsPutAppearanceRequest,
        encodeSettingsPutAppearanceRequest(&payload, 7, 0),
    );
    try std.testing.expectError(
        error.InvalidSettingsPutAppearanceRequest,
        decodeRequest(.put_appearance, payload[0..11]),
    );
}

test "Library scan requests copy one bounded root path after the revision" {
    var storage: [library_scan_request_header_bytes + max_root_path_bytes]u8 = undefined;
    const payload = try encodeLibraryScanRequest(&storage, 7, "/courses/Δ");
    const decoded = try decodeRequest(.scan_library, payload);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);
    try std.testing.expectEqualStrings("/courses/Δ", decoded.root_path);

    try std.testing.expectError(error.InvalidLibraryScanRequest, encodeLibraryScanRequest(&storage, 0, "/courses"));
    try std.testing.expectError(error.InvalidLibraryScanRequest, encodeLibraryScanRequest(&storage, 7, ""));
    try std.testing.expectError(error.InvalidLibraryScanRequest, encodeLibraryScanRequest(&storage, 7, "bad\x00path"));
    try std.testing.expectError(error.InvalidLibraryScanRequest, encodeLibraryScanRequest(&storage, 7, "\xff"));
    try std.testing.expectError(error.InvalidLibraryScanRequest, decodeRequest(.scan_library, payload[0..8]));
}

test "Library scan cancellation is a distinct empty control request" {
    const decoded = try decodeRequest(.cancel_library_scan, "");
    try std.testing.expectEqual(Operation.cancel_library_scan, decoded.operation);
    try std.testing.expectError(
        error.InvalidLibraryScanCancelRequest,
        decodeRequest(.cancel_library_scan, "x"),
    );
}

test "Library scan progress is a distinct empty polling request" {
    const decoded = try decodeRequest(.load_library_scan_progress, "");
    try std.testing.expectEqual(Operation.load_library_scan_progress, decoded.operation);
    try std.testing.expectError(
        error.InvalidLibraryScanCancelRequest,
        decodeRequest(.load_library_scan_progress, "x"),
    );
    try std.testing.expectEqual(@as(usize, 32), scan_progress_result_bytes);
}

test "notes requests use bounded unambiguous binary wire formats" {
    var list_storage: [notes_list_request_header_bytes + max_lesson_id_bytes]u8 = undefined;
    const list = try encodeNotesListRequest(&list_storage, 7, 20, "lesson-1");
    const decoded_list = try decodeRequest(.load_notes, list);
    try std.testing.expectEqual(@as(u64, 7), decoded_list.expected_revision);
    try std.testing.expectEqual(@as(u64, 20), decoded_list.page_offset);
    try std.testing.expectEqualStrings("lesson-1", decoded_list.lesson_id);

    var save_storage: [note_save_request_header_bytes + max_lesson_id_bytes + max_note_id_bytes + max_note_text_bytes]u8 = undefined;
    const create = try encodeNoteSaveRequest(
        &save_storage,
        7,
        42.5,
        "lesson-1",
        "",
        "Remember this.",
    );
    const decoded_create = try decodeRequest(.save_note, create);
    try std.testing.expectEqual(@as(f64, 42.5), decoded_create.note_timestamp);
    try std.testing.expectEqualStrings("lesson-1", decoded_create.lesson_id);
    try std.testing.expectEqualStrings("", decoded_create.note_id);
    try std.testing.expectEqualStrings("Remember this.", decoded_create.note_text);

    const update = try encodeNoteSaveRequest(
        &save_storage,
        8,
        42.5,
        "lesson-1",
        "note-1",
        "Updated.",
    );
    const decoded_update = try decodeRequest(.save_note, update);
    try std.testing.expectEqualStrings("note-1", decoded_update.note_id);
    try std.testing.expectEqualStrings("Updated.", decoded_update.note_text);

    var delete_storage: [note_delete_request_header_bytes + max_note_id_bytes]u8 = undefined;
    const delete = try encodeNoteDeleteRequest(&delete_storage, 9, "note-1");
    const decoded_delete = try decodeRequest(.delete_note, delete);
    try std.testing.expectEqual(@as(u64, 9), decoded_delete.expected_revision);
    try std.testing.expectEqualStrings("note-1", decoded_delete.note_id);

    try std.testing.expectError(error.InvalidNotesListRequest, encodeNotesListRequest(&list_storage, 7, 1, "lesson-1"));
    try std.testing.expectError(error.InvalidNoteSaveRequest, encodeNoteSaveRequest(&save_storage, 7, -1, "lesson-1", "", "text"));
    try std.testing.expectError(error.InvalidNoteSaveRequest, encodeNoteSaveRequest(&save_storage, 7, 1, "lesson-1", "", ""));
    try std.testing.expectError(error.InvalidNoteDeleteRequest, encodeNoteDeleteRequest(&delete_storage, 7, ""));
}

test "Library stats requests use one strict revision wire format" {
    var payload: [library_stats_request_bytes]u8 = undefined;
    const encoded = try encodeLibraryStatsRequest(&payload, 7);
    const decoded = try decodeRequest(.load_library_stats, encoded);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);

    try std.testing.expectError(error.InvalidLibraryStatsRequest, encodeLibraryStatsRequest(&payload, 0));
    try std.testing.expectError(error.InvalidLibraryStatsRequest, decodeRequest(.load_library_stats, payload[0..7]));
}

test "activity requests use one strict revision wire format" {
    var payload: [activity_page_request_bytes]u8 = undefined;
    const encoded = try encodeActivityPageRequest(&payload, 7);
    const decoded = try decodeRequest(.load_activity_page, encoded);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);

    try std.testing.expectError(error.InvalidActivityPageRequest, encodeActivityPageRequest(&payload, 0));
    try std.testing.expectError(error.InvalidActivityPageRequest, decodeRequest(.load_activity_page, payload[0..7]));
}

test "Progress requests use one bounded strict binary wire format" {
    var storage: [progress_put_request_header_bytes + max_lesson_id_bytes]u8 = undefined;
    const payload = try encodeProgressPutRequest(&storage, 7, 340, 338.5, true, "lesson-1");
    const decoded = try decodeRequest(.put_progress, payload);
    try std.testing.expectEqual(@as(u64, 7), decoded.expected_revision);
    try std.testing.expectEqual(@as(u64, 340), decoded.watched_time);
    try std.testing.expectEqual(@as(f64, 338.5), decoded.last_position);
    try std.testing.expect(decoded.completed);
    try std.testing.expectEqualStrings("lesson-1", decoded.lesson_id);

    try std.testing.expectError(error.InvalidProgressPutRequest, encodeProgressPutRequest(&storage, 0, 340, 338.5, true, "lesson-1"));
    try std.testing.expectError(error.InvalidProgressPutRequest, encodeProgressPutRequest(&storage, 7, @as(u64, std.math.maxInt(i64)) + 1, 338.5, true, "lesson-1"));
    try std.testing.expectError(error.InvalidProgressPutRequest, encodeProgressPutRequest(&storage, 7, 340, std.math.nan(f64), true, "lesson-1"));
    try std.testing.expectError(error.InvalidProgressPutRequest, encodeProgressPutRequest(&storage, 7, 340, -1, true, "lesson-1"));
    try std.testing.expectError(error.InvalidProgressPutRequest, encodeProgressPutRequest(&storage, 7, 340, 338.5, true, ""));
    try std.testing.expectError(error.InvalidProgressPutRequest, decodeRequest(.put_progress, payload[0..progress_put_request_header_bytes]));
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

test "Search requests use bounded strict binary wire formats" {
    var rebuild_storage: [search_index_request_bytes]u8 = undefined;
    const rebuild = try encodeSearchIndexRequest(&rebuild_storage, 8);
    const decoded_rebuild = try decodeRequest(.rebuild_search_index, rebuild);
    try std.testing.expectEqual(@as(u64, 8), decoded_rebuild.expected_revision);
    try std.testing.expectError(error.InvalidSearchIndexRequest, encodeSearchIndexRequest(&rebuild_storage, 0));

    var query_storage: [search_query_request_header_bytes + max_search_query_bytes]u8 = undefined;
    const query = try encodeSearchQueryRequest(&query_storage, 8, 42, 20, "binary heaps");
    const decoded_query = try decodeRequest(.query_search, query);
    try std.testing.expectEqual(@as(u64, 8), decoded_query.expected_revision);
    try std.testing.expectEqual(@as(u64, 42), decoded_query.query_id);
    try std.testing.expectEqual(@as(u64, 20), decoded_query.page_offset);
    try std.testing.expectEqualStrings("binary heaps", decoded_query.query);

    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 0, 42, 20, "binary heaps"));
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 8, 0, 20, "binary heaps"));
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 8, 42, 1, "binary heaps"));
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 8, 42, 20, ""));
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 8, 42, 20, "bad\x00query"));
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&query_storage, 8, 42, 20, "\xff"));
    const oversized = [_]u8{'q'} ** (max_search_query_bytes + 1);
    var oversized_storage: [search_query_request_header_bytes + oversized.len]u8 = undefined;
    try std.testing.expectError(error.InvalidSearchQueryRequest, encodeSearchQueryRequest(&oversized_storage, 8, 42, 20, &oversized));
}

test "Document requests use Lesson and Document IDs with strict revisions" {
    var open_storage: [document_open_request_header_bytes + max_lesson_id_bytes]u8 = undefined;
    const open = try encodeDocumentOpenRequest(&open_storage, 8, "lesson-guide");
    const decoded_open = try decodeRequest(.open_document, open);
    try std.testing.expectEqual(@as(u64, 8), decoded_open.expected_revision);
    try std.testing.expectEqualStrings("lesson-guide", decoded_open.lesson_id);

    var page_storage: [document_page_request_header_bytes + max_lesson_id_bytes]u8 = undefined;
    const page = try encodeDocumentPageRequest(&page_storage, 8, 16, "lesson-guide");
    const decoded_page = try decodeRequest(.load_document_page, page);
    try std.testing.expectEqual(@as(u64, 8), decoded_page.expected_revision);
    try std.testing.expectEqual(@as(u64, 16), decoded_page.page_offset);
    try std.testing.expectEqualStrings("lesson-guide", decoded_page.document_id);

    var external_storage: [document_external_open_request_header_bytes + max_lesson_id_bytes]u8 = undefined;
    const external = try encodeDocumentExternalOpenRequest(&external_storage, 8, "lesson-guide");
    const decoded_external = try decodeRequest(.prepare_document_external_open, external);
    try std.testing.expectEqual(@as(u64, 8), decoded_external.expected_revision);
    try std.testing.expectEqualStrings("lesson-guide", decoded_external.lesson_id);

    try std.testing.expectError(error.InvalidDocumentOpenRequest, encodeDocumentOpenRequest(&open_storage, 0, "lesson-guide"));
    try std.testing.expectError(error.InvalidDocumentPageRequest, encodeDocumentPageRequest(&page_storage, 8, 1, "lesson-guide"));
    try std.testing.expectError(error.InvalidDocumentExternalOpenRequest, encodeDocumentExternalOpenRequest(&external_storage, 8, "bad\x00id"));
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
