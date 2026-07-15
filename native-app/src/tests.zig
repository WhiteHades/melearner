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

fn selectCourse(model: *main.Model, id: []const u8, name: []const u8) void {
    model.selected_course = .{
        .id_len = id.len,
        .name_len = name.len,
    };
    @memcpy(model.selected_course.id_storage[0..id.len], id);
    @memcpy(model.selected_course.name_storage[0..name.len], name);
}

test "the first native frame identifies the Library opening state" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const model = main.Model{};
    const tree = try buildTree(arena_state.allocator(), &model);

    try testing.expect(findByText(tree.root, .text, "Library") != null);
    try testing.expect(findByText(tree.root, .text, "Opening your Library\u{2026}") != null);
    try testing.expect(findByText(tree.root, .text, "melearner") == null);
}

test "static native UI copy stays English-only" {
    var iterator = std.unicode.Utf8Iterator{ .bytes = main.library_markup, .i = 0 };
    while (iterator.nextCodepoint()) |codepoint| {
        try testing.expect(codepoint <= 0x7F or codepoint == 0x2026);
    }
}

test "the native app registers renderable Latin Cyrillic and Japanese course text without changing the system theme" {
    try testing.expectEqual(@as(usize, 1), main.app_fonts.len);
    try testing.expectEqual(main.primary_font_id, main.app_fonts[0].id);
    try testing.expect(main.app_fonts[0].ttf.len <= native_sdk.runtime.max_registered_canvas_font_bytes);

    const face = try canvas.font_ttf.Face.parse(main.app_fonts[0].ttf);
    var mapped: usize = 0;
    var scalar: u21 = 0;
    while (scalar <= 0xFFFF) : (scalar += 1) {
        const glyph = face.glyphIndex(scalar);
        if (canvas.font_ttf.geist_regular.glyphIndex(scalar) != 0) {
            try testing.expect(glyph != 0);
        }
        if (glyph == 0) continue;
        var path = canvas.vector.PathBuilder(canvas.font_ttf.max_simple_glyph_path_elements){};
        try face.glyphOutline(glyph, canvas.Affine.identity(), &path);
        mapped += 1;
    }
    try testing.expectEqual(@as(usize, 3904), mapped);

    for ([_]u21{ 0x0141, 0x0421, 0x044B, 0x2019, 0x2013, 0x2026, 0x65E5, 0x672C, 0x8A9E, 0x5165, 0x9580, 0x6F22, 0x5B57, 0x9B31 }) |codepoint| {
        const glyph = face.glyphIndex(codepoint);
        try testing.expect(glyph != 0);
        var path = canvas.vector.PathBuilder(canvas.font_ttf.max_simple_glyph_path_elements){};
        try face.glyphOutline(glyph, canvas.Affine.identity(), &path);
        try testing.expect(path.slice().len != 0);
    }

    const model = main.Model{ .appearance = .{
        .color_scheme = .dark,
        .high_contrast = true,
        .reduce_motion = true,
    } };
    var expected = canvas.DesignTokens.theme(.{
        .color_scheme = .dark,
        .contrast = .high,
        .reduce_motion = true,
    });
    expected.typography.font_id = main.primary_font_id;
    try testing.expectEqualDeep(expected, main.tokensFromModel(&model));
}

test "the app startup options install and select the course-content font" {
    const options = main.appOptions();
    try testing.expectEqual(main.primary_font_id, options.tokens_fn.?(&main.Model{}).typography.font_id);
    try testing.expectEqualSlices(main.LibraryApp.FontRegistration, &main.app_fonts, options.fonts);

    const app_state = try testing.allocator.create(main.LibraryApp);
    defer testing.allocator.destroy(app_state);
    app_state.* = main.LibraryApp.init(testing.allocator, .{}, options);
    defer app_state.deinit();
    app_state.effects.executor = .fake;

    const harness = try native_sdk.TestHarness().create(testing.allocator, .{
        .size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
    });
    defer harness.destroy(testing.allocator);
    harness.null_platform.gpu_surfaces = true;
    const app = app_state.app();
    try harness.start(app);
    const appearance: native_sdk.Appearance = .{
        .color_scheme = .dark,
        .high_contrast = true,
        .reduce_motion = true,
    };
    try harness.runtime.dispatchPlatformEvent(app, .{ .appearance_changed = appearance });
    try harness.runtime.dispatchPlatformEvent(app, .{ .gpu_surface_frame = .{
        .label = main.canvas_label,
        .size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
        .scale_factor = 1,
        .frame_index = 1,
        .timestamp_ns = 1_000_000,
        .nonblank = true,
    } });

    try testing.expect(app_state.installed);
    try testing.expectEqualDeep(appearance, app_state.model.appearance);
    try testing.expectEqual(@as(usize, 1), harness.runtime.registeredCanvasFontCount());
    const face = harness.runtime.registeredCanvasFontFace(main.primary_font_id).?;
    try testing.expect(face.glyphIndex(0x9B31) != 0);
    try testing.expectEqual(@as(usize, 0), harness.runtime.dispatchErrors().len);
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
        \\{"revision":7,"offset":0,"total":1,"rows":[{"id":"course-1","name":"Systems","missingSince":null,"lessonCount":10,"completedLessonCount":4,"progressPercent":40}]}
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

test "opening a Course records access before loading its Lessons" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);
    const library_request = effects.pendingExternalAt(0).?;
    try effects.feedExternalResult(library_request.request_id, .success,
        \\{"revision":7,"offset":0,"total":1,"rows":[{"id":"course-1","name":"Systems","missingSince":null,"lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    main.update(&model, .{ .open_course = "course-1" }, &effects);
    try testing.expect(model.courseOpening());
    const access_request = effects.pendingExternalAt(0).?;
    try testing.expectEqual(core_adapter.course_access_key, access_request.key);
    try testing.expectEqual(@intFromEnum(core_adapter.Operation.access_course), access_request.kind);
    var expected_access: [core_adapter.course_access_request_header_bytes + "course-1".len]u8 = undefined;
    std.mem.writeInt(u64, expected_access[0..8], 7, .little);
    @memcpy(expected_access[8..], "course-1");
    try testing.expectEqualSlices(u8, &expected_access, access_request.payload);

    try effects.feedExternalResult(access_request.request_id, .success,
        \\{"revision":8,"courseId":"course-1","lastAccessed":"2026-07-15T10:00:00.000Z"}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    const lesson_request = effects.pendingExternalAt(0).?;
    try testing.expectEqual(core_adapter.lesson_page_key, lesson_request.key);
    try testing.expectEqual(@intFromEnum(core_adapter.Operation.load_lesson_page), lesson_request.kind);
    var expected_lessons: [core_adapter.lesson_page_request_header_bytes + "course-1".len]u8 = undefined;
    std.mem.writeInt(u64, expected_lessons[0..8], 8, .little);
    std.mem.writeInt(u64, expected_lessons[8..16], 0, .little);
    @memcpy(expected_lessons[16..], "course-1");
    try testing.expectEqualSlices(u8, &expected_lessons, lesson_request.payload);

    try effects.feedExternalResult(lesson_request.request_id, .success,
        \\{"revision":8,"courseId":"course-1","sectionId":null,"offset":0,"total":1,"rows":[{"id":"lesson-1","courseId":"course-1","sectionId":"section-1","sectionName":"Foundations","name":"Introduction","path":"/courses/systems/intro.mp4","relativePath":"Foundations/intro.mp4","kind":"video","duration":120,"fileSize":1024,"completed":false,"watchedTime":0,"lastPosition":0.0,"order":0,"subtitles":[]}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(model.courseReady());
    try testing.expectEqual(@as(u64, 8), model.library_revision);
    try testing.expectEqual(@as(usize, 1), model.lesson_count);
    try testing.expectEqualStrings("Foundations", model.lessons[0].sectionName());
    try testing.expectEqualStrings("Introduction", model.lessons[0].name());

    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .button, "Back") != null);
    try testing.expect(findByText(tree.root, .text, "Foundations") != null);
    try testing.expect(findByText(tree.root, .text, "Introduction") != null);

    main.update(&model, .back_to_library, &effects);
    const refreshed_library = effects.pendingExternalAt(0).?;
    try testing.expectEqualSlices(u8, &([_]u8{0} ** core_adapter.library_page_request_bytes), refreshed_library.payload);
}

test "missing Courses remain visible without dispatching access" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);
    const request = effects.pendingExternalAt(0).?;
    try effects.feedExternalResult(request.request_id, .success,
        \\{"revision":7,"offset":0,"total":1,"rows":[{"id":"course-missing","name":"Archived Systems","missingSince":"2026-07-15T10:00:00.000Z","lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(!model.courses[0].available);
    main.update(&model, .{ .open_course = "course-missing" }, &effects);
    try testing.expect(effects.pendingExternalAt(0) == null);

    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .text, "Archived Systems") != null);
    try testing.expect(findByText(tree.root, .badge, "Missing") != null);
}

test "a Course that disappears during access becomes an honest error state" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    var model = main.Model{
        .screen = .course,
        .course_state = .accessing,
        .library_revision = 7,
    };
    selectCourse(&model, "course-1", "Systems");

    main.update(&model, .{ .course_accessed = .{
        .request_id = 1,
        .key = core_adapter.course_access_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.access_course),
        .schema_version = core_adapter.schema_version,
        .outcome = .failed,
        .bytes = "This Course is no longer available.",
    } }, &effects);

    try testing.expect(model.courseFailed());
    try testing.expectEqualStrings("This Course is no longer available.", model.courseMessage());
}

test "Back always reloads against the adapter's recovered revision" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;
    var model = main.Model{
        .screen = .course,
        .course_state = .failed,
        .library_revision = 7,
    };
    selectCourse(&model, "course-1", "Systems");

    main.update(&model, .back_to_library, &effects);

    try testing.expect(model.showLibrary());
    try testing.expectEqual(@as(u64, 0), model.library_revision);
    const request = effects.pendingExternalAt(0).?;
    try testing.expectEqualSlices(u8, &([_]u8{0} ** core_adapter.library_page_request_bytes), request.payload);
}

test "Lesson pages reject mismatched Course snapshots" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    var model = main.Model{
        .screen = .course,
        .course_state = .loading,
        .library_revision = 8,
    };
    selectCourse(&model, "course-1", "Systems");

    main.update(&model, .{ .lessons_loaded = .{
        .request_id = 1,
        .key = core_adapter.lesson_page_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.load_lesson_page),
        .schema_version = core_adapter.schema_version,
        .outcome = .ok,
        .bytes =
        \\{"revision":8,"courseId":"course-2","sectionId":null,"offset":0,"total":0,"rows":[]}
        ,
    } }, &effects);

    try testing.expect(model.courseFailed());
    try testing.expectEqualStrings("The Lesson service returned invalid data.", model.courseMessage());
}

test "Lesson paging carries the accessed revision and Course ID" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;
    var model = main.Model{
        .screen = .course,
        .course_state = .ready,
        .library_revision = 8,
        .total_lessons = 21,
        .lesson_count = 20,
    };
    selectCourse(&model, "course-1", "Systems");

    main.update(&model, .next_lesson_page, &effects);
    const request = effects.pendingExternalAt(0).?;
    var expected: [core_adapter.lesson_page_request_header_bytes + "course-1".len]u8 = undefined;
    std.mem.writeInt(u64, expected[0..8], 8, .little);
    std.mem.writeInt(u64, expected[8..16], 20, .little);
    @memcpy(expected[16..], "course-1");
    try testing.expectEqualSlices(u8, &expected, request.payload);

    try effects.feedExternalResult(request.request_id, .success,
        \\{"revision":8,"courseId":"course-1","sectionId":null,"offset":20,"total":21,"rows":[{"id":"lesson-21","courseId":"course-1","sectionId":"section-2","sectionName":"Advanced","name":"Wrap up","kind":"document","duration":0,"fileSize":512,"completed":true,"watchedTime":0,"lastPosition":0.0,"order":0}]}
    );
    main.update(&model, effects.takeMsg().?, &effects);

    try testing.expect(model.courseReady());
    try testing.expectEqual(@as(u64, 20), model.lesson_page_offset);
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    try testing.expectEqualStrings("21 of 21 Lessons", model.lessonTotalLabel(arena_state.allocator()));
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .button, "Previous") != null);
    try testing.expect(findByText(tree.root, .button, "Next") == null);
    try testing.expect(findByText(tree.root, .text, "Advanced") != null);
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
        \\{"revision":7,"offset":20,"total":21,"rows":[{"id":"course-21","name":"Course 21","missingSince":null,"lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
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
        \\{"revision":7,"offset":20,"total":21,"rows":[{"id":"course-21","name":"Course 21","missingSince":null,"lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
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
