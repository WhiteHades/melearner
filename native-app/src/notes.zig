const std = @import("std");
const core_adapter = @import("core_adapter.zig");
const native_sdk = @import("native_sdk");

const canvas = native_sdk.canvas;

pub const max_notes: usize = core_adapter.lesson_page_size;
pub const max_created_at_bytes: usize = 64;
pub const max_message_bytes: usize = 256;

pub const State = enum {
    closed,
    loading,
    empty,
    ready,
    saving,
    deleting,
    failed,
};

pub const Note = struct {
    id_storage: [core_adapter.max_note_id_bytes]u8 = [_]u8{0} ** core_adapter.max_note_id_bytes,
    id_len: usize = 0,
    text_storage: [core_adapter.max_note_text_bytes]u8 = [_]u8{0} ** core_adapter.max_note_text_bytes,
    text_len: usize = 0,
    created_at_storage: [max_created_at_bytes]u8 = [_]u8{0} ** max_created_at_bytes,
    created_at_len: usize = 0,
    timestamp: f64 = 0,
    restore_focus: bool = false,

    pub fn id(note: *const Note) []const u8 {
        return note.id_storage[0..note.id_len];
    }

    pub fn text(note: *const Note) []const u8 {
        return note.text_storage[0..note.text_len];
    }

    pub fn createdAt(note: *const Note) []const u8 {
        return note.created_at_storage[0..note.created_at_len];
    }

    pub fn timestampLabel(note: *const Note, arena: std.mem.Allocator) []const u8 {
        const seconds: u64 = @intFromFloat(@floor(note.timestamp));
        return std.fmt.allocPrint(arena, "{d}:{d:0>2}", .{ seconds / 60, seconds % 60 }) catch "";
    }
};

pub const Model = struct {
    open: bool = false,
    state: State = .closed,
    lesson_id_storage: [core_adapter.max_lesson_id_bytes]u8 = [_]u8{0} ** core_adapter.max_lesson_id_bytes,
    lesson_id_len: usize = 0,
    revision: u64 = 0,
    offset: u64 = 0,
    pending_offset: u64 = 0,
    total: u64 = 0,
    rows: [max_notes]Note = undefined,
    row_count: usize = 0,
    list_request_id: u64 = 0,
    mutation_request_id: u64 = 0,
    pending_delete_id_storage: [core_adapter.max_note_id_bytes]u8 = [_]u8{0} ** core_adapter.max_note_id_bytes,
    pending_delete_id_len: usize = 0,
    pending_delete_index: usize = 0,
    restore_index: ?usize = null,
    editing: bool = false,
    editing_id_storage: [core_adapter.max_note_id_bytes]u8 = [_]u8{0} ** core_adapter.max_note_id_bytes,
    editing_id_len: usize = 0,
    editing_timestamp: f64 = 0,
    draft: canvas.TextBuffer(core_adapter.max_note_text_bytes) = .{},
    message_storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    message_len: usize = 0,

    pub fn lessonId(model: *const Model) []const u8 {
        return model.lesson_id_storage[0..model.lesson_id_len];
    }

    pub fn editingId(model: *const Model) []const u8 {
        return model.editing_id_storage[0..model.editing_id_len];
    }

    pub fn message(model: *const Model) []const u8 {
        return model.message_storage[0..model.message_len];
    }

    pub fn noteRows(model: *const Model, arena: std.mem.Allocator) []const Note {
        _ = arena;
        return model.rows[0..model.row_count];
    }

    pub fn draftText(model: *const Model) []const u8 {
        return model.draft.text();
    }

    pub fn draftValid(model: *const Model) bool {
        return validText(model.draft.text());
    }

    pub fn trimmedDraft(model: *const Model) []const u8 {
        return trimEcmascriptWhitespace(model.draft.text());
    }

    pub fn begin(model: *Model, lesson_id: []const u8) !void {
        if (!validId(lesson_id, core_adapter.max_lesson_id_bytes)) return error.InvalidLessonId;
        model.* = .{
            .open = true,
            .state = .loading,
            .lesson_id_len = lesson_id.len,
        };
        @memcpy(model.lesson_id_storage[0..lesson_id.len], lesson_id);
    }

    pub fn close(model: *Model) void {
        model.* = .{};
    }

    pub fn beginNew(model: *Model, timestamp: f64) void {
        model.editing = true;
        model.editing_id_len = 0;
        model.editing_timestamp = if (std.math.isFinite(timestamp) and timestamp >= 0) timestamp else 0;
        model.draft.clear();
        model.message_len = 0;
    }

    pub fn beginEdit(model: *Model, note_id: []const u8) bool {
        const note = for (model.rows[0..model.row_count]) |*candidate| {
            if (std.mem.eql(u8, candidate.id(), note_id)) break candidate;
        } else return false;
        model.editing = true;
        model.editing_id_len = note.id_len;
        @memcpy(model.editing_id_storage[0..note.id_len], note.id());
        model.editing_timestamp = note.timestamp;
        model.draft.set(note.text());
        model.message_len = 0;
        return true;
    }

    pub fn cancelEdit(model: *Model) void {
        if (model.state == .saving or model.state == .deleting) return;
        model.editing = false;
        model.editing_id_len = 0;
        model.draft.clear();
        model.message_len = 0;
    }

    pub fn loadPage(
        model: *Model,
        bytes: []const u8,
        expected_revision: u64,
        expected_lesson_id: []const u8,
    ) !void {
        const PayloadNote = struct {
            id: []const u8,
            lessonId: []const u8,
            timestamp: f64,
            text: []const u8,
            createdAt: []const u8,
        };
        const Payload = struct {
            revision: u64,
            lessonId: []const u8,
            offset: u64,
            total: u64,
            rows: []const PayloadNote,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
        defer parsed.deinit();
        const page = parsed.value;
        if (page.revision != expected_revision or
            !std.mem.eql(u8, page.lessonId, expected_lesson_id) or
            page.offset != model.pending_offset or
            page.offset > page.total or
            page.offset % core_adapter.lesson_page_size != 0 or
            page.rows.len > max_notes or
            page.rows.len > page.total - page.offset)
        {
            return error.InvalidNotePage;
        }

        var previous_timestamp: f64 = 0;
        var previous_created_at: []const u8 = "";
        var previous_id: []const u8 = "";
        for (page.rows, 0..) |row, index| {
            if (!validId(row.id, core_adapter.max_note_id_bytes) or
                !std.mem.eql(u8, row.lessonId, expected_lesson_id) or
                !std.math.isFinite(row.timestamp) or
                row.timestamp < 0 or
                !validText(row.text) or
                !validId(row.createdAt, max_created_at_bytes))
            {
                return error.InvalidNotePage;
            }
            if (index != 0 and (row.timestamp < previous_timestamp or
                (row.timestamp == previous_timestamp and
                    (std.mem.order(u8, row.createdAt, previous_created_at) == .lt or
                        (std.mem.eql(u8, row.createdAt, previous_created_at) and
                            std.mem.order(u8, row.id, previous_id) != .gt)))))
            {
                return error.InvalidNotePage;
            }
            previous_timestamp = row.timestamp;
            previous_created_at = row.createdAt;
            previous_id = row.id;
        }

        model.revision = page.revision;
        model.offset = page.offset;
        model.total = page.total;
        model.row_count = page.rows.len;
        for (page.rows, 0..) |row, index| {
            model.rows[index] = .{
                .id_len = row.id.len,
                .text_len = row.text.len,
                .created_at_len = row.createdAt.len,
                .timestamp = row.timestamp,
            };
            @memcpy(model.rows[index].id_storage[0..row.id.len], row.id);
            @memcpy(model.rows[index].text_storage[0..row.text.len], row.text);
            @memcpy(model.rows[index].created_at_storage[0..row.createdAt.len], row.createdAt);
        }
        if (model.restore_index) |restore_index| {
            if (model.row_count != 0) {
                model.rows[@min(restore_index, model.row_count - 1)].restore_focus = true;
            }
            model.restore_index = null;
        }
        model.list_request_id = 0;
        model.message_len = 0;
        model.state = if (page.total == 0) .empty else .ready;
    }

    pub fn savedRevision(model: *const Model, bytes: []const u8, previous_revision: u64) !u64 {
        const Payload = struct {
            revision: u64,
            id: []const u8,
            lessonId: []const u8,
            timestamp: f64,
            text: []const u8,
            createdAt: []const u8,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
        defer parsed.deinit();
        const saved = parsed.value;
        if (saved.revision <= previous_revision or
            !validId(saved.id, core_adapter.max_note_id_bytes) or
            !std.mem.eql(u8, saved.lessonId, model.lessonId()) or
            !std.math.isFinite(saved.timestamp) or
            saved.timestamp < 0 or
            saved.timestamp != model.editing_timestamp or
            (model.editing_id_len != 0 and
                !std.mem.eql(u8, saved.id, model.editingId())) or
            !std.mem.eql(u8, saved.text, model.trimmedDraft()) or
            !validText(saved.text) or
            !validId(saved.createdAt, max_created_at_bytes))
        {
            return error.InvalidNoteSaved;
        }
        return saved.revision;
    }

    pub fn deletedRevision(model: *const Model, bytes: []const u8, previous_revision: u64) !u64 {
        const Payload = struct {
            revision: u64,
            noteId: []const u8,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
        defer parsed.deinit();
        if (parsed.value.revision <= previous_revision or
            !validId(parsed.value.noteId, core_adapter.max_note_id_bytes) or
            model.pending_delete_id_len == 0 or
            !std.mem.eql(
                u8,
                parsed.value.noteId,
                model.pending_delete_id_storage[0..model.pending_delete_id_len],
            ))
        {
            return error.InvalidNoteDeleted;
        }
        return parsed.value.revision;
    }

    pub fn beginDelete(model: *Model, note_id: []const u8) bool {
        const index = for (model.rows[0..model.row_count], 0..) |*candidate, candidate_index| {
            if (std.mem.eql(u8, candidate.id(), note_id)) break candidate_index;
        } else return false;
        model.pending_delete_id_len = note_id.len;
        @memcpy(model.pending_delete_id_storage[0..note_id.len], note_id);
        model.pending_delete_index = index;
        model.state = .deleting;
        model.message_len = 0;
        return true;
    }

    pub fn finishDelete(model: *Model) void {
        model.restore_index = model.pending_delete_index;
        model.pending_delete_id_len = 0;
        model.pending_delete_index = 0;
        model.editing = false;
        model.editing_id_len = 0;
        model.draft.clear();
    }

    pub fn finishSave(model: *Model) void {
        model.editing = false;
        model.editing_id_len = 0;
        model.draft.clear();
    }

    pub fn setFailure(model: *Model, detail: []const u8) void {
        const value = if (detail.len == 0 or
            detail.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(detail))
            "Notes are unavailable."
        else
            detail;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
        model.list_request_id = 0;
        model.mutation_request_id = 0;
        model.state = .failed;
    }

    pub fn hasPreviousPage(model: *const Model) bool {
        return model.offset != 0 and model.state == .ready;
    }

    pub fn hasNextPage(model: *const Model) bool {
        return model.state == .ready and
            model.offset + @as(u64, @intCast(model.row_count)) < model.total;
    }
};

fn validId(value: []const u8, max_bytes: usize) bool {
    return value.len != 0 and
        value.len <= max_bytes and
        std.unicode.utf8ValidateSlice(value) and
        std.mem.indexOfScalar(u8, value, 0) == null;
}

fn validText(value: []const u8) bool {
    if (value.len == 0 or
        value.len > core_adapter.max_note_text_bytes or
        !std.unicode.utf8ValidateSlice(value))
    {
        return false;
    }
    var units: usize = 0;
    var has_content = false;
    var view = std.unicode.Utf8View.initUnchecked(value);
    var iterator = view.iterator();
    while (iterator.nextCodepoint()) |codepoint| {
        units += if (codepoint > 0xFFFF) 2 else 1;
        if (!isEcmascriptWhitespace(codepoint)) has_content = true;
    }
    return has_content and units <= 2_000;
}

fn trimEcmascriptWhitespace(value: []const u8) []const u8 {
    var start: usize = 0;
    var end = value.len;
    while (start < end) {
        const sequence_len = std.unicode.utf8ByteSequenceLength(value[start]) catch return "";
        const codepoint = std.unicode.utf8Decode(value[start..][0..sequence_len]) catch return "";
        if (!isEcmascriptWhitespace(codepoint)) break;
        start += sequence_len;
    }
    while (end > start) {
        var codepoint_start = end - 1;
        while (codepoint_start > start and
            (value[codepoint_start] & 0b1100_0000) == 0b1000_0000)
        {
            codepoint_start -= 1;
        }
        const codepoint = std.unicode.utf8Decode(value[codepoint_start..end]) catch return "";
        if (!isEcmascriptWhitespace(codepoint)) break;
        end = codepoint_start;
    }
    return value[start..end];
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

test "note pages are strict bounded and ordered" {
    var model = Model{};
    try model.begin("lesson-1");
    model.pending_offset = 0;
    try model.loadPage(
        \\{"revision":7,"lessonId":"lesson-1","offset":0,"total":2,"rows":[{"id":"note-1","lessonId":"lesson-1","timestamp":12.5,"text":"First","createdAt":"2026-07-23T10:00:00Z"},{"id":"note-2","lessonId":"lesson-1","timestamp":30.0,"text":"Second","createdAt":"2026-07-23T10:01:00Z"}]}
    , 7, "lesson-1");
    try std.testing.expectEqual(State.ready, model.state);
    try std.testing.expectEqual(@as(usize, 2), model.row_count);
    try std.testing.expectEqualStrings("First", model.rows[0].text());

    try std.testing.expectError(error.InvalidNotePage, model.loadPage(
        \\{"revision":7,"lessonId":"lesson-1","offset":0,"total":2,"rows":[{"id":"note-2","lessonId":"lesson-1","timestamp":30.0,"text":"Second","createdAt":"2026-07-23T10:01:00Z"},{"id":"note-1","lessonId":"lesson-1","timestamp":12.5,"text":"First","createdAt":"2026-07-23T10:00:00Z"}]}
    , 7, "lesson-1"));
}

test "note mutation responses must advance the current lesson revision" {
    var model = Model{};
    try model.begin("lesson-1");
    model.editing = true;
    model.editing_timestamp = 42.5;
    model.draft.set("Remember");
    try std.testing.expect(model.beginDelete("note-1") == false);
    model.pending_delete_id_len = "note-1".len;
    @memcpy(model.pending_delete_id_storage[0.."note-1".len], "note-1");
    try std.testing.expectEqual(@as(u64, 8), try model.savedRevision(
        \\{"revision":8,"id":"note-1","lessonId":"lesson-1","timestamp":42.5,"text":"Remember","createdAt":"2026-07-23T10:00:00Z"}
    , 7));
    try std.testing.expectEqual(@as(u64, 9), try model.deletedRevision(
        \\{"revision":9,"noteId":"note-1"}
    , 8));
}

test "draft validation matches the core UTF-16 and whitespace contract" {
    var model = Model{};
    model.draft.set("\u{00a0}\u{2003}");
    try std.testing.expect(!model.draftValid());
    model.draft.set("Remember \u{1f680}");
    try std.testing.expect(model.draftValid());
}
