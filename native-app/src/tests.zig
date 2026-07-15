const std = @import("std");
const native_sdk = @import("native_sdk");
const main = @import("main.zig");

const canvas = native_sdk.canvas;
const testing = std.testing;

const LibraryMarkup = canvas.MarkupView(main.Model, main.Msg);

fn buildTree(arena: std.mem.Allocator, model: *const main.Model) !main.LibraryUi.Tree {
    var view = try LibraryMarkup.init(arena, main.library_markup);
    var ui = main.LibraryUi.init(arena);
    return ui.finalize(try view.build(&ui, model));
}

fn findByText(widget: canvas.Widget, kind: canvas.WidgetKind, value: []const u8) ?canvas.Widget {
    if (widget.kind == kind and std.mem.eql(u8, widget.text, value)) return widget;
    for (widget.children) |child| {
        if (findByText(child, kind, value)) |found| return found;
    }
    return null;
}

test "the first native frame identifies the Library opening state" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const model = main.Model{};
    const tree = try buildTree(arena_state.allocator(), &model);

    try testing.expect(findByText(tree.root, .text, "Library") != null);
    try testing.expect(findByText(tree.root, .text, "Opening your Library\u{2026}") != null);
}

test "the native shell stays clean at its compact and default sizes" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const model = main.Model{};
    const tree = try buildTree(arena_state.allocator(), &model);
    try canvas.expectLayoutAuditSweepClean(testing.allocator, tree.root, .{
        .min_size = native_sdk.geometry.SizeF.init(main.window_min_width, main.window_min_height),
        .default_size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
    });
    try canvas.expectA11yAuditSweepClean(testing.allocator, tree.root, .{
        .min_size = native_sdk.geometry.SizeF.init(main.window_min_width, main.window_min_height),
        .default_size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
    });
}

test "compiled and interpreted Library views have the same widget identities" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const model = main.Model{};
    const interpreted = try buildTree(arena, &model);
    var compiled_ui = main.LibraryUi.init(arena);
    const compiled = try compiled_ui.finalize(main.CompiledLibraryView.build(&compiled_ui, &model));

    try expectSameTree(interpreted.root, compiled.root);
}

fn expectSameTree(expected: canvas.Widget, actual: canvas.Widget) !void {
    try testing.expectEqual(expected.id, actual.id);
    try testing.expectEqual(expected.children.len, actual.children.len);
    for (expected.children, actual.children) |expected_child, actual_child| {
        try expectSameTree(expected_child, actual_child);
    }
}

test "native titlebar geometry reaches the Library model" {
    var model = main.Model{};
    const chrome: native_sdk.WindowChrome = .{
        .insets = .{ .top = 52, .left = 78 },
        .buttons = native_sdk.geometry.RectF.init(20, 19, 52, 14),
    };

    main.update(&model, main.onChrome(chrome).?);

    try testing.expectEqual(@as(f32, 78), model.chrome_leading);
    try testing.expectEqual(@as(f32, 52), model.header_height);
}
