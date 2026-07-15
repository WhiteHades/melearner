const std = @import("std");
const native_sdk = @import("native_sdk");
const core_adapter = @import("core_adapter.zig");
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

test "Library boot uses the versioned Rust-core effect and renders an empty page" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);

    const request = effects.pendingExternalAt(0).?;
    try testing.expectEqual(core_adapter.library_page_key, request.key);
    try testing.expectEqual(core_adapter.adapter_id, request.adapter_id);
    try testing.expectEqual(@intFromEnum(core_adapter.Operation.load_library_page), request.kind);
    try testing.expectEqual(core_adapter.schema_version, request.schema_version);
    try testing.expectEqualSlices(u8, &([_]u8{0} ** core_adapter.library_page_request_bytes), request.payload);

    try effects.feedExternalResult(request.request_id, .success,
        \\{"revision":1,"offset":0,"total":0,"rows":[]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(model.libraryEmpty());
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .text, "Your Library is empty") != null);
}

test "a populated Rust-core page is copied into the native Library model" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);
    const request = effects.pendingExternalAt(0).?;
    try effects.feedExternalResult(request.request_id, .success,
        \\{"revision":7,"offset":0,"total":1,"rows":[{"id":"course-missing","name":"Systems","lessonCount":10,"completedLessonCount":4,"progressPercent":40}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(model.libraryReady());
    try testing.expectEqual(@as(u64, 7), model.library_revision);
    try testing.expectEqual(@as(usize, 1), model.course_count);
    try testing.expectEqualStrings("Systems", model.courses[0].name());

    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .text, "Systems") != null);
    try testing.expect(findByText(tree.root, .text, "4 of 10 Lessons \u{b7} 40%") != null);

    var compiled_ui = main.LibraryUi.init(arena_state.allocator());
    const compiled = try compiled_ui.finalize(main.CompiledLibraryView.build(&compiled_ui, &model));
    try testing.expect(findByText(compiled.root, .text, "Systems") != null);
}

test "Library paging carries the revision and offset in the core request" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{
        .library_state = .ready,
        .library_revision = 7,
        .total_courses = 21,
        .course_count = 20,
    };
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    try testing.expectEqualStrings("1–20 of 21 Courses", model.courseTotalLabel(arena_state.allocator()));
    main.update(&model, .next_page, &effects);

    const next_request = effects.pendingExternalAt(0).?;
    var expected_next: [core_adapter.library_page_request_bytes]u8 = undefined;
    std.mem.writeInt(u64, expected_next[0..8], 7, .little);
    std.mem.writeInt(u64, expected_next[8..16], 20, .little);
    try testing.expectEqualSlices(u8, &expected_next, next_request.payload);

    try effects.feedExternalResult(next_request.request_id, .success,
        \\{"revision":7,"offset":20,"total":21,"rows":[{"id":"course-21","name":"Course 21","lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);
    try testing.expectEqual(@as(u64, 20), model.page_offset);

    try testing.expectEqualStrings("21 of 21 Courses", model.courseTotalLabel(arena_state.allocator()));
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .button, "Previous") != null);
    try testing.expect(findByText(tree.root, .button, "Next") == null);

    main.update(&model, .previous_page, &effects);
    const previous_request = effects.pendingExternalAt(0).?;
    var expected_previous: [core_adapter.library_page_request_bytes]u8 = undefined;
    std.mem.writeInt(u64, expected_previous[0..8], 7, .little);
    std.mem.writeInt(u64, expected_previous[8..16], 0, .little);
    try testing.expectEqualSlices(u8, &expected_previous, previous_request.payload);
}

test "a Library response for a different page is rejected" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);
    const request = effects.pendingExternalAt(0).?;
    try effects.feedExternalResult(request.request_id, .success,
        \\{"revision":7,"offset":20,"total":21,"rows":[{"id":"course-21","name":"Course 21","lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(model.libraryFailed());
    try testing.expectEqualStrings("The Library service returned invalid data.", model.libraryMessage());
}

test "Rust-core failures become an honest native Library state" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();

    var model = main.Model{};
    main.update(&model, .{ .library_loaded = .{
        .request_id = 1,
        .key = core_adapter.library_page_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.load_library_page),
        .schema_version = core_adapter.schema_version,
        .outcome = .failed,
        .bytes = "The Library database could not open.",
    } }, &effects);

    try testing.expect(model.libraryFailed());
    try testing.expectEqualStrings("The Library database could not open.", model.libraryMessage());
}

test "the live adapter creates and reopens only the fresh native database" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(testing.io, .{ .sub_path = "melearner.db", .data = "previous database sentinel" });
    var state_dir_buffer: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const state_dir_len = try tmp.dir.realPath(testing.io, &state_dir_buffer);
    const state_dir = state_dir_buffer[0..state_dir_len];

    try expectLiveEmptyPage(state_dir);
    try tmp.dir.access(testing.io, "melearner-native.sqlite3", .{});
    var previous_database: [64]u8 = undefined;
    try testing.expectEqualStrings(
        "previous database sentinel",
        try tmp.dir.readFile(testing.io, "melearner.db", &previous_database),
    );
    try expectLiveEmptyPage(state_dir);
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

fn expectLiveEmptyPage(state_dir: []const u8) !void {
    const adapter = try core_adapter.CoreAdapter.create(testing.allocator, testing.io, state_dir);
    errdefer adapter.destroy();
    var effects = main.Effects.init(testing.allocator);
    errdefer effects.deinit();
    effects.bindExternalAdapter(adapter.binding());

    var model = main.Model{};
    main.boot(&model, &effects);
    var waited_ms: usize = 0;
    while (!effects.hasPending() and waited_ms < 5_000) : (waited_ms += 1) {
        try std.Io.sleep(testing.io, std.Io.Duration.fromMilliseconds(1), .awake);
    }
    const msg = effects.takeMsg() orelse return error.TestTimedOut;
    main.update(&model, msg, &effects);
    try testing.expect(model.libraryEmpty());

    effects.deinit();
    adapter.destroy();
}

test "native titlebar geometry reaches the Library model" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    var model = main.Model{};
    const chrome: native_sdk.WindowChrome = .{
        .insets = .{ .top = 52, .left = 78 },
        .buttons = native_sdk.geometry.RectF.init(20, 19, 52, 14),
    };

    main.update(&model, main.onChrome(chrome).?, &effects);

    try testing.expectEqual(@as(f32, 78), model.chrome_leading);
    try testing.expectEqual(@as(f32, 52), model.header_height);
}
