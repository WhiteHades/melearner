const std = @import("std");
const native_sdk = @import("native_sdk");

pub const page_size: usize = 8;
pub const max_blocks: u64 = 8_192;
pub const max_id_bytes: usize = 128;
pub const max_source_bytes: usize = 16 * 1024;
pub const max_format_bytes: usize = 16;
pub const max_message_bytes: usize = 256;
pub const max_external_path_bytes: usize = native_sdk.platform.max_dialog_path_bytes;

pub const State = enum {
    inactive,
    opening,
    loading,
    ready,
    unsupported,
    failed,
    external_ready,
};

pub const BlockKind = enum {
    text,
    paragraph,
    heading,
    list_item,
    quote,
    code,
    table,
    rule,
};

pub const Block = struct {
    id: u64 = 0,
    kind: BlockKind = .text,
    level: u8 = 0,
    source_storage: [max_source_bytes]u8 = [_]u8{0} ** max_source_bytes,
    source_len: usize = 0,

    pub fn source(block: *const Block) []const u8 {
        return block.source_storage[0..block.source_len];
    }

    pub fn heading(block: *const Block) bool {
        return block.kind == .heading;
    }

    pub fn code(block: *const Block) bool {
        return block.kind == .code or block.kind == .table;
    }

    pub fn rule(block: *const Block) bool {
        return block.kind == .rule;
    }
};

pub const Model = struct {
    state: State = .inactive,
    revision: u64 = 0,
    lesson_id_storage: [max_id_bytes]u8 = [_]u8{0} ** max_id_bytes,
    lesson_id_len: usize = 0,
    document_id_storage: [max_id_bytes]u8 = [_]u8{0} ** max_id_bytes,
    document_id_len: usize = 0,
    format_storage: [max_format_bytes]u8 = [_]u8{0} ** max_format_bytes,
    format_len: usize = 0,
    total_blocks: u64 = 0,
    offset: u64 = 0,
    pending_offset: u64 = 0,
    rows: [page_size]Block = undefined,
    row_count: usize = 0,
    open_request_id: u64 = 0,
    page_request_id: u64 = 0,
    external_request_id: u64 = 0,
    warning_count: u64 = 0,
    message_storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    message_len: usize = 0,
    external_path_storage: [max_external_path_bytes]u8 = [_]u8{0} ** max_external_path_bytes,
    external_path_len: usize = 0,

    pub fn begin(model: *Model, revision: u64, lesson_id: []const u8) !void {
        if (revision == 0 or !validId(lesson_id)) return error.InvalidDocumentLesson;
        model.* = .{
            .state = .opening,
            .revision = revision,
            .lesson_id_len = lesson_id.len,
        };
        @memcpy(model.lesson_id_storage[0..lesson_id.len], lesson_id);
    }

    pub fn close(model: *Model) void {
        model.* = .{};
    }

    pub fn lessonId(model: *const Model) []const u8 {
        return model.lesson_id_storage[0..model.lesson_id_len];
    }

    pub fn documentId(model: *const Model) []const u8 {
        return model.document_id_storage[0..model.document_id_len];
    }

    pub fn format(model: *const Model) []const u8 {
        return model.format_storage[0..model.format_len];
    }

    pub fn message(model: *const Model) []const u8 {
        return model.message_storage[0..model.message_len];
    }

    pub fn blockRows(model: *const Model, arena: std.mem.Allocator) []const Block {
        _ = arena;
        return model.rows[0..model.row_count];
    }

    pub fn loadOpened(model: *Model, bytes: []const u8) !void {
        const Warning = struct {
            code: []const u8,
            count: u32,
        };
        const Payload = struct {
            revision: u64,
            lessonId: []const u8,
            documentId: []const u8,
            format: []const u8,
            totalBlocks: u64,
            warnings: []const Warning,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const value = parsed.value;
        if (model.state != .opening or
            value.revision != model.revision or
            !std.mem.eql(u8, value.lessonId, model.lessonId()) or
            !validId(value.documentId) or
            !validFormat(value.format) or
            value.totalBlocks > max_blocks or
            value.warnings.len > 10)
        {
            return error.InvalidDocumentOpened;
        }
        var warning_count: u64 = 0;
        for (value.warnings) |warning| {
            if (!validWarning(warning.code) or warning.count == 0) {
                return error.InvalidDocumentOpened;
            }
            warning_count = std.math.add(u64, warning_count, warning.count) catch
                return error.InvalidDocumentOpened;
        }
        model.document_id_len = value.documentId.len;
        @memcpy(model.document_id_storage[0..value.documentId.len], value.documentId);
        model.format_len = value.format.len;
        @memcpy(model.format_storage[0..value.format.len], value.format);
        model.total_blocks = value.totalBlocks;
        model.warning_count = warning_count;
        model.offset = 0;
        model.pending_offset = 0;
        model.row_count = 0;
        model.message_len = 0;
        model.state = .loading;
    }

    pub fn loadPage(model: *Model, bytes: []const u8) !void {
        const SourceBlock = struct {
            id: u64,
            kind: []const u8,
            level: u8,
            source: []const u8,
        };
        const Payload = struct {
            revision: u64,
            documentId: []const u8,
            offset: u64,
            total: u64,
            blocks: []const SourceBlock,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const page = parsed.value;
        if (model.state != .loading or
            page.revision != model.revision or
            !std.mem.eql(u8, page.documentId, model.documentId()) or
            page.offset != model.pending_offset or
            page.offset % page_size != 0 or
            page.total != model.total_blocks or
            page.offset > page.total or
            page.blocks.len > page_size)
        {
            return error.InvalidDocumentPage;
        }
        const expected_rows = @min(@as(u64, page_size), page.total - page.offset);
        if (page.blocks.len != expected_rows or (page.total != 0 and page.offset == page.total)) {
            return error.InvalidDocumentPage;
        }

        var rows: [page_size]Block = undefined;
        for (page.blocks, 0..) |source, index| {
            const kind = parseBlockKind(source.kind) orelse return error.InvalidDocumentPage;
            if (source.id != page.offset + @as(u64, @intCast(index)) or
                source.level > 128 or
                source.source.len == 0 or
                source.source.len > max_source_bytes or
                !std.unicode.utf8ValidateSlice(source.source) or
                std.mem.indexOfScalar(u8, source.source, 0) != null)
            {
                return error.InvalidDocumentPage;
            }
            rows[index] = .{
                .id = source.id,
                .kind = kind,
                .level = source.level,
                .source_len = source.source.len,
            };
            @memcpy(rows[index].source_storage[0..source.source.len], source.source);
        }
        @memcpy(model.rows[0..page.blocks.len], rows[0..page.blocks.len]);
        model.row_count = page.blocks.len;
        model.offset = page.offset;
        model.state = .ready;
        model.page_request_id = 0;
        model.message_len = 0;
    }

    pub fn loadExternalReady(model: *Model, bytes: []const u8) !void {
        const Payload = struct {
            revision: u64,
            lessonId: []const u8,
            canonicalPath: []const u8,
        };
        const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const value = parsed.value;
        if (value.revision != model.revision or
            !std.mem.eql(u8, value.lessonId, model.lessonId()) or
            value.canonicalPath.len == 0 or
            value.canonicalPath.len > max_external_path_bytes or
            !std.unicode.utf8ValidateSlice(value.canonicalPath) or
            std.mem.indexOfScalar(u8, value.canonicalPath, 0) != null)
        {
            return error.InvalidDocumentExternalOpen;
        }
        model.external_path_len = value.canonicalPath.len;
        @memcpy(model.external_path_storage[0..value.canonicalPath.len], value.canonicalPath);
        model.external_request_id = 0;
        model.state = .external_ready;
        model.setMessage("The file is validated, but this SDK cannot open local files in another application yet.");
    }

    pub fn markUnsupported(model: *Model, bytes: []const u8) bool {
        const Payload = struct { @"error": []const u8 };
        const parsed = std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        }) catch return false;
        defer parsed.deinit();
        if (!std.mem.eql(u8, parsed.value.@"error", "documentUnsupported")) return false;
        model.open_request_id = 0;
        model.state = .unsupported;
        model.setMessage("This document format is not available in the native reader.");
        return true;
    }

    pub fn setFailure(model: *Model, detail: []const u8) void {
        model.open_request_id = 0;
        model.page_request_id = 0;
        model.external_request_id = 0;
        model.state = .failed;
        model.setMessage(detail);
    }

    pub fn hasPreviousPage(model: *const Model) bool {
        return model.state == .ready and model.offset != 0;
    }

    pub fn hasNextPage(model: *const Model) bool {
        return model.state == .ready and
            model.offset + @as(u64, @intCast(model.row_count)) < model.total_blocks;
    }

    pub fn pageLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.total_blocks == 0) return "Empty document";
        const first = model.offset + 1;
        const last = model.offset + @as(u64, @intCast(model.row_count));
        return std.fmt.allocPrint(arena, "Blocks {d}–{d} of {d}", .{
            first,
            last,
            model.total_blocks,
        }) catch "";
    }

    pub fn warningLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.warning_count == 0) return "";
        return std.fmt.allocPrint(arena, "{d} unsafe or unsupported item{s} omitted", .{
            model.warning_count,
            if (model.warning_count == 1) "" else "s",
        }) catch "";
    }

    fn setMessage(model: *Model, detail: []const u8) void {
        const value = if (detail.len == 0 or
            detail.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(detail))
            "The document could not open."
        else
            detail;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
    }
};

fn validId(value: []const u8) bool {
    return value.len != 0 and
        value.len <= max_id_bytes and
        std.unicode.utf8ValidateSlice(value) and
        std.mem.indexOfScalar(u8, value, 0) == null;
}

fn validFormat(value: []const u8) bool {
    return std.mem.eql(u8, value, "text") or
        std.mem.eql(u8, value, "markdown") or
        std.mem.eql(u8, value, "html") or
        std.mem.eql(u8, value, "docx");
}

fn validWarning(value: []const u8) bool {
    return std.mem.eql(u8, value, "markdownHtmlOmitted") or
        std.mem.eql(u8, value, "markdownImageOmitted") or
        std.mem.eql(u8, value, "markdownUnsafeLinkOmitted") or
        std.mem.eql(u8, value, "htmlActiveContentOmitted") or
        std.mem.eql(u8, value, "htmlCssOmitted") or
        std.mem.eql(u8, value, "htmlRemoteResourceOmitted") or
        std.mem.eql(u8, value, "htmlUnsupportedConstructOmitted") or
        std.mem.eql(u8, value, "docxEmbeddedImageOmitted") or
        std.mem.eql(u8, value, "docxExternalRelationshipOmitted") or
        std.mem.eql(u8, value, "docxUnsupportedConstructOmitted");
}

fn parseBlockKind(value: []const u8) ?BlockKind {
    if (std.mem.eql(u8, value, "text")) return .text;
    if (std.mem.eql(u8, value, "paragraph")) return .paragraph;
    if (std.mem.eql(u8, value, "heading")) return .heading;
    if (std.mem.eql(u8, value, "listItem")) return .list_item;
    if (std.mem.eql(u8, value, "quote")) return .quote;
    if (std.mem.eql(u8, value, "code")) return .code;
    if (std.mem.eql(u8, value, "table")) return .table;
    if (std.mem.eql(u8, value, "rule")) return .rule;
    return null;
}

test "Document model accepts only correlated bounded pages" {
    var model = Model{};
    try model.begin(8, "lesson-guide");
    try model.loadOpened(
        \\{"revision":8,"lessonId":"lesson-guide","documentId":"lesson-guide","format":"markdown","totalBlocks":2,"warnings":[]}
    );
    try model.loadPage(
        \\{"revision":8,"documentId":"lesson-guide","offset":0,"total":2,"blocks":[{"id":0,"kind":"heading","level":1,"source":"# Guide"},{"id":1,"kind":"paragraph","level":0,"source":"Read this."}]}
    );
    try std.testing.expectEqual(State.ready, model.state);
    try std.testing.expectEqualStrings("# Guide", model.rows[0].source());
    try std.testing.expect(!model.hasNextPage());

    model.state = .loading;
    try std.testing.expectError(
        error.InvalidDocumentPage,
        model.loadPage(
            \\{"revision":8,"documentId":"lesson-guide","offset":0,"total":2,"blocks":[{"id":1,"kind":"heading","level":1,"source":"wrong"},{"id":2,"kind":"paragraph","level":0,"source":"wrong"}]}
        ),
    );
}

test "Document model keeps unsupported and external-ready states explicit" {
    var model = Model{};
    try model.begin(8, "lesson.pdf");
    try std.testing.expect(model.markUnsupported(
        \\{"error":"documentUnsupported"}
    ));
    try std.testing.expectEqual(State.unsupported, model.state);
    try model.loadExternalReady(
        \\{"revision":8,"lessonId":"lesson.pdf","canonicalPath":"/courses/lesson.pdf"}
    );
    try std.testing.expectEqual(State.external_ready, model.state);
}
