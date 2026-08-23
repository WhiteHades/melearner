const std = @import("std");

pub const max_message_bytes: usize = 256;

pub const Appearance = enum(u32) {
    light = 1,
    dark = 2,
    cozy = 3,

    pub fn label(value: Appearance) []const u8 {
        return switch (value) {
            .light => "Light",
            .dark => "Dark",
            .cozy => "Cozy",
        };
    }
};

pub const State = enum {
    loading,
    ready,
    saving,
    failed,
};

pub const Model = struct {
    open: bool = false,
    state: State = .loading,
    revision: u64 = 0,
    selected: Appearance = .light,
    confirmed: Appearance = .light,
    get_request_id: u64 = 0,
    put_request_id: u64 = 0,
    message_storage: [max_message_bytes]u8 = [_]u8{0} ** max_message_bytes,
    message_len: usize = 0,

    pub fn message(model: *const Model) []const u8 {
        return model.message_storage[0..model.message_len];
    }

    pub fn load(model: *Model, bytes: []const u8) !void {
        const settings = try parse(bytes);
        model.revision = settings.revision;
        model.selected = settings.appearance;
        model.confirmed = settings.appearance;
        model.get_request_id = 0;
        model.put_request_id = 0;
        model.message_len = 0;
        model.state = .ready;
    }

    pub fn beginSave(model: *Model, appearance: Appearance) bool {
        if (model.revision == 0 or model.state == .loading or model.state == .saving) return false;
        if (appearance == model.confirmed) {
            model.selected = appearance;
            model.state = .ready;
            model.message_len = 0;
            return false;
        }
        model.selected = appearance;
        model.state = .saving;
        model.message_len = 0;
        return true;
    }

    pub fn applySaved(model: *Model, bytes: []const u8) !void {
        const settings = try parse(bytes);
        if (settings.revision <= model.revision or settings.appearance != model.selected) {
            return error.InvalidSettingsResponse;
        }
        model.revision = settings.revision;
        model.confirmed = settings.appearance;
        model.put_request_id = 0;
        model.message_len = 0;
        model.state = .ready;
    }

    pub fn failLoad(model: *Model, detail: []const u8) void {
        model.get_request_id = 0;
        model.state = .failed;
        model.setMessage(detail, "Appearance settings could not be loaded.");
    }

    pub fn failSave(model: *Model, detail: []const u8) void {
        model.put_request_id = 0;
        model.selected = model.confirmed;
        model.state = .failed;
        model.setMessage(detail, "Appearance settings could not be saved.");
    }

    fn setMessage(model: *Model, detail: []const u8, fallback: []const u8) void {
        const value = if (detail.len == 0 or
            detail.len > max_message_bytes or
            !std.unicode.utf8ValidateSlice(detail))
            fallback
        else
            detail;
        @memcpy(model.message_storage[0..value.len], value);
        model.message_len = value.len;
    }
};

const Parsed = struct {
    revision: u64,
    appearance: Appearance,
};

fn parse(bytes: []const u8) !Parsed {
    const Payload = struct {
        revision: u64,
        appearance: []const u8,
    };
    const parsed = try std.json.parseFromSlice(Payload, std.heap.page_allocator, bytes, .{});
    defer parsed.deinit();
    if (parsed.value.revision == 0) return error.InvalidSettingsResponse;
    const appearance: Appearance = if (std.mem.eql(u8, parsed.value.appearance, "light"))
        .light
    else if (std.mem.eql(u8, parsed.value.appearance, "dark"))
        .dark
    else if (std.mem.eql(u8, parsed.value.appearance, "cozy"))
        .cozy
    else
        return error.InvalidSettingsResponse;
    return .{ .revision = parsed.value.revision, .appearance = appearance };
}

test "settings load and save require typed monotonic responses" {
    var model: Model = .{};
    try model.load("{\"revision\":1,\"appearance\":\"light\"}");
    try std.testing.expectEqual(State.ready, model.state);
    try std.testing.expect(model.beginSave(.cozy));
    try std.testing.expectEqual(Appearance.cozy, model.selected);
    try model.applySaved("{\"revision\":2,\"appearance\":\"cozy\"}");
    try std.testing.expectEqual(Appearance.cozy, model.confirmed);
    try std.testing.expectEqual(@as(u64, 2), model.revision);

    try std.testing.expect(model.beginSave(.dark));
    try std.testing.expectError(
        error.InvalidSettingsResponse,
        model.applySaved("{\"revision\":2,\"appearance\":\"dark\"}"),
    );
    try std.testing.expectError(
        error.InvalidSettingsResponse,
        model.applySaved("{\"revision\":3,\"appearance\":\"light\"}"),
    );
}

test "failed optimistic save restores the confirmed appearance" {
    var model: Model = .{};
    try model.load("{\"revision\":4,\"appearance\":\"dark\"}");
    try std.testing.expect(model.beginSave(.light));
    model.failSave("disk unavailable");
    try std.testing.expectEqual(Appearance.dark, model.selected);
    try std.testing.expectEqual(State.failed, model.state);
    try std.testing.expectEqualStrings("disk unavailable", model.message());
}

test "settings reject unknown appearance and malformed payloads" {
    var model: Model = .{};
    try std.testing.expectError(
        error.InvalidSettingsResponse,
        model.load("{\"revision\":1,\"appearance\":\"system\"}"),
    );
    try std.testing.expectError(
        error.MissingField,
        model.load("{\"revision\":1}"),
    );
}
